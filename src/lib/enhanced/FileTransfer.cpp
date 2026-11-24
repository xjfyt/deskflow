/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransfer.h"

#include "FileManifest.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#if defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(Q_OS_LINUX)
#include <linux/fs.h>
#include <sys/syscall.h>
#elif defined(Q_OS_MACOS)
#include <stdio.h>
#endif
#endif

namespace deskflow::enhanced {
namespace {

constexpr int HandshakeTimeout = 5000;
constexpr int OfferTimeout = 120000;
constexpr int DataTimeout = 15000;
constexpr qint64 TransferTimeout = 4 * 60 * 60 * 1000;
constexpr int ResumePreparationTimeout = 10 * 60 * 1000;
constexpr char ResumeCapability = 0x04;
constexpr char DropCapability = 0x08;

enum class Access
{
  Inspect,
  Read,
  Update
};

struct SourceStamp
{
  quint64 device;
  quint64 file;
  quint64 size;
  std::array<qint64, 4> times;

  bool operator==(const SourceStamp &) const = default;
};

struct SourceEntry
{
  QString absolutePath;
  SourceStamp stamp;
};

struct Offer
{
  FileManifest manifest;
  QList<SourceEntry> sources;
  quint64 total = 0;
};

bool isLink(const QFileInfo &info)
{
#if defined(Q_OS_WIN)
  const auto path = QDir::toNativeSeparators(info.absoluteFilePath());
  const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
  return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return info.isSymLink();
#endif
}

#if defined(Q_OS_WIN)
class DirectoryHandles
{
public:
  ~DirectoryHandles()
  {
    for (const auto handle : handles)
      CloseHandle(handle);
  }

  std::vector<HANDLE> handles;
};

HANDLE openWindowsPath(const QString &path, bool directory, Access access = Access::Inspect)
{
  const auto native = QDir::toNativeSeparators(path);
  const DWORD rights = access == Access::Inspect ? FILE_READ_ATTRIBUTES
                       : access == Access::Read  ? GENERIC_READ
                                                 : GENERIC_READ | GENERIC_WRITE;
  const auto handle = CreateFileW(
      reinterpret_cast<LPCWSTR>(native.utf16()), rights,
      access == Access::Inspect ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr
  );
  if (handle == INVALID_HANDLE_VALUE)
    return INVALID_HANDLE_VALUE;

  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info) || GetFileType(handle) != FILE_TYPE_DISK ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) {
    CloseHandle(handle);
    return INVALID_HANDLE_VALUE;
  }
  return handle;
}

HANDLE openSourcePath(const QString &path, bool directory, Access access = Access::Inspect)
{
  QStringList parents;
  QDir parentDirectory = QFileInfo(path).absoluteDir();
  while (!parentDirectory.isRoot()) {
    parents.prepend(parentDirectory.absolutePath());
    if (!parentDirectory.cdUp())
      return INVALID_HANDLE_VALUE;
  }
  DirectoryHandles directories;
  directories.handles.reserve(static_cast<size_t>(parents.size()));
  for (const auto &parent : parents) {
    auto handle = openWindowsPath(parent, true);
    if (handle == INVALID_HANDLE_VALUE)
      return INVALID_HANDLE_VALUE;
    directories.handles.push_back(handle);
  }
  return openWindowsPath(path, directory, access);
}

std::optional<SourceStamp> stampHandle(HANDLE handle)
{
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_BASIC_INFO basic{};
  if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
      !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
    return std::nullopt;
  return SourceStamp{
      info.dwVolumeSerialNumber,
      (static_cast<quint64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
          ? 0
          : (static_cast<quint64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
      {basic.CreationTime.QuadPart, basic.LastWriteTime.QuadPart, basic.ChangeTime.QuadPart, 0}
  };
}

std::optional<SourceStamp> stampPath(const QString &path, bool directory)
{
  const auto handle = openSourcePath(path, directory);
  if (handle == INVALID_HANDLE_VALUE)
    return std::nullopt;
  const auto stamp = stampHandle(handle);
  CloseHandle(handle);
  return stamp;
}

std::optional<SourceStamp> stampDescriptor(int descriptor)
{
  return stampHandle(reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)));
}

int openSource(const QString &path, Access access)
{
  const auto handle = openSourcePath(path, false, access);
  if (handle == INVALID_HANDLE_VALUE)
    return -1;
  const int mode = access == Access::Update ? _O_RDWR : _O_RDONLY;
  const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), mode | _O_BINARY | _O_NOINHERIT);
  if (descriptor == -1)
    CloseHandle(handle);
  return descriptor;
}

bool publishDirectory(const QString &source, const QString &destination)
{
  return MoveFileW(
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(source).utf16()),
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(destination).utf16())
  );
}
#else
int openSourcePath(const QString &path, bool directory, Access access = Access::Inspect)
{
  if (!QDir::isAbsolutePath(path))
    return -1;
  const auto components = path.split(u'/', Qt::SkipEmptyParts);
  if (components.isEmpty() && !directory)
    return -1;
  auto current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  for (qsizetype i = 0; current >= 0 && i < components.size(); ++i) {
    const auto type = (directory || i < components.size() - 1) ? O_DIRECTORY : O_NONBLOCK;
    const auto mode = type == O_NONBLOCK && access == Access::Update ? O_RDWR : O_RDONLY;
    const auto next =
        ::openat(current, QFile::encodeName(components[i]).constData(), mode | O_NOFOLLOW | O_CLOEXEC | type);
    ::close(current);
    current = next;
  }
  return current;
}

std::optional<SourceStamp> stampHandle(int descriptor, bool directory)
{
  struct stat info{};
  if (::fstat(descriptor, &info) != 0 || (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)))
    return std::nullopt;
#if defined(Q_OS_MACOS)
  const auto &modified = info.st_mtimespec;
  const auto &changed = info.st_ctimespec;
#else
  const auto &modified = info.st_mtim;
  const auto &changed = info.st_ctim;
#endif
  return SourceStamp{
      static_cast<quint64>(info.st_dev),
      static_cast<quint64>(info.st_ino),
      directory ? 0 : static_cast<quint64>(info.st_size),
      {modified.tv_sec, modified.tv_nsec, changed.tv_sec, changed.tv_nsec}
  };
}

std::optional<SourceStamp> stampPath(const QString &path, bool directory)
{
  const auto descriptor = openSourcePath(path, directory);
  if (descriptor < 0)
    return std::nullopt;
  const auto stamp = stampHandle(descriptor, directory);
  ::close(descriptor);
  return stamp;
}

std::optional<SourceStamp> stampDescriptor(int descriptor)
{
  return stampHandle(descriptor, false);
}

int openSource(const QString &path, Access access)
{
  return openSourcePath(path, false, access);
}

bool publishDirectory(const QString &source, const QString &destination)
{
  const auto from = QFile::encodeName(source);
  const auto to = QFile::encodeName(destination);
#if defined(Q_OS_MACOS)
  return ::renamex_np(from.constData(), to.constData(), RENAME_EXCL) == 0;
#elif defined(Q_OS_LINUX)
  return ::syscall(SYS_renameat2, AT_FDCWD, from.constData(), AT_FDCWD, to.constData(), RENAME_NOREPLACE) == 0;
#else
  return QDir().rename(source, destination);
#endif
}
#endif

bool isSameDirectory(const QString &path, const std::optional<SourceStamp> &expected)
{
  const auto current = stampPath(path, true);
  return current && expected && current->device == expected->device && current->file == expected->file;
}

class StagingDirectory
{
public:
  StagingDirectory(const QString &destination, const SourceStamp &destinationStamp)
      : m_path(destination),
        m_destinationStamp(destinationStamp),
        m_temporary(QDir(destination).filePath(QStringLiteral(".deskflow-XXXXXX"))),
        m_stamp(stampPath(m_temporary.path(), true))
  {
    // Removal by path is safe only while it still names this staging directory.
    m_temporary.setAutoRemove(false);
    if (!m_temporary.isValid() || !m_stamp)
      throw Error(QCoreApplication::translate("Enhanced", "Could not create a private transfer folder."));
  }

  ~StagingDirectory()
  {
    if (!m_published && isSameDirectory(m_path, m_destinationStamp) && isSameDirectory(path(), m_stamp))
      QDir(path()).removeRecursively();
  }

  QString path() const
  {
    return m_temporary.path();
  }

  void check() const
  {
    if (!isSameDirectory(m_path, m_destinationStamp) || !isSameDirectory(path(), m_stamp))
      throw Error(QCoreApplication::translate("Enhanced", "The destination changed during the transfer."));
  }

  bool publish(const QString &destination)
  {
    check();
    m_published = publishDirectory(path(), destination);
    return m_published;
  }

private:
  const QString m_path;
  const std::optional<SourceStamp> m_destinationStamp;
  QTemporaryDir m_temporary;
  const std::optional<SourceStamp> m_stamp;
  bool m_published = false;
};

void collect(
    Offer &offer, const QFileInfo &info, const QString &relativePath, qsizetype depth, const std::atomic_bool &cancelled
)
{
  checkCancelled(cancelled);
  if (offer.sources.size() >= MaxEntries || depth > MaxDepth || relativePath.toUtf8().size() > MaxPathBytes ||
      isLink(info) || (!info.isFile() && !info.isDir())) {
    throw Error(
        QCoreApplication::translate("Enhanced", "The selection contains a link, unavailable item or unsupported path.")
    );
  }
  const bool directory = info.isDir();
  const auto sourcePath = info.absoluteFilePath();
  const auto stamp = stampPath(sourcePath, directory);
  if (!stamp)
    throw Error(QCoreApplication::translate("Enhanced", "A selected file could not be opened safely."));
  if (stamp->size > MaxTransferBytes - offer.total)
    throw Error(QCoreApplication::translate("Enhanced", "The selection is larger than the 20 GiB transfer limit."));
  offer.total += stamp->size;
  offer.manifest.entries.append({relativePath, directory, stamp->size});
  offer.sources.append({sourcePath, *stamp});

  if (!directory)
    return;
  if (!info.isReadable())
    throw Error(QCoreApplication::translate("Enhanced", "A selected directory is unreadable."));

  QDirIterator children(sourcePath, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
  while (children.hasNext()) {
    const auto child = QFileInfo(children.next());
    collect(offer, child, relativePath + u'/' + child.fileName(), depth + 1, cancelled);
  }
  if (stampPath(sourcePath, true) != stamp)
    throw Error(QCoreApplication::translate("Enhanced", "The source selection changed during the transfer."));
}

bool sourceMatches(const SourceEntry &source, const FileEntry &entry)
{
  return stampPath(source.absolutePath, entry.directory) == source.stamp;
}

Offer buildOffer(const QStringList &paths, const std::atomic_bool &cancelled)
{
  Offer offer;
  for (const auto &path : paths) {
    checkCancelled(cancelled);
    const QFileInfo info(path);
    const auto parent = info.absoluteDir().canonicalPath();
    if (info.fileName().isEmpty() || isLink(info) || parent.isEmpty())
      throw Error(
          QCoreApplication::translate("Enhanced", "The source selection includes a root, link or missing path.")
      );
    const QFileInfo resolved(QDir(parent).filePath(info.fileName()));
    collect(offer, resolved, info.fileName(), 1, cancelled);
  }
  return offer;
}

void openTransferFile(QFile &file, const QString &path, Access access)
{
  const auto descriptor = openSource(path, access);
  if (descriptor < 0)
    throw Error(QCoreApplication::translate("Enhanced", "A selected file could not be opened safely."));
  const auto mode = access == Access::Update ? QIODevice::ReadWrite | QIODevice::Unbuffered : QIODevice::ReadOnly;
  if (!file.open(descriptor, mode, QFileDevice::AutoCloseHandle)) {
#if defined(Q_OS_WIN)
    _close(descriptor);
#else
    ::close(descriptor);
#endif
    throw Error(QCoreApplication::translate("Enhanced", "A selected file could not be opened safely."));
  }
}

bool isSingleFile(const FileManifest &manifest)
{
  return manifest.entries.size() == 1 && !manifest.entries.front().directory;
}

QByteArray sourceVersion(const SourceStamp &stamp)
{
  // No sender-local name, volume number or inode is exposed on the wire.
  QByteArray version(7 * static_cast<qsizetype>(sizeof(quint64)), Qt::Uninitialized);
  qToBigEndian(stamp.device, version.data());
  qToBigEndian(stamp.file, version.data() + 8);
  qToBigEndian(stamp.size, version.data() + 16);
  for (qsizetype i = 0; i < static_cast<qsizetype>(stamp.times.size()); ++i)
    qToBigEndian(stamp.times[i], version.data() + 24 + 8 * i);
  return QCryptographicHash::hash(version, QCryptographicHash::Sha256);
}

void hashPrefix(QFile &file, QCryptographicHash &hash, quint64 size, const std::atomic_bool &cancelled)
{
  if (!file.seek(0))
    throw Error(QCoreApplication::translate("Enhanced", "Could not read the transfer prefix."));
  while (size != 0) {
    checkCancelled(cancelled);
    const auto count = static_cast<qint64>(std::min<quint64>(size, ChunkSize));
    const auto bytes = file.read(count);
    if (bytes.size() != count)
      throw Error(QCoreApplication::translate("Enhanced", "Could not read the transfer prefix."));
    hash.addData(bytes);
    size -= count;
  }
}

constexpr qsizetype ResumeIdentitySize = 2 * TokenSize;
constexpr qsizetype ResumeOffsetSize = sizeof(quint64) + TokenSize;
constexpr qsizetype CheckpointHeaderSize = 4 + 2 * TokenSize + ResumeIdentitySize;
constexpr qint64 CheckpointLifetime = 24 * 60 * 60;
constexpr auto PrivateDirectory = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;

QString invalidCheckpoint()
{
  return QCoreApplication::translate(
      "Enhanced", "The resume checkpoint is unavailable, expired or does not match. Inspect it before clearing it."
  );
}

QString canonicalDestination(const QString &directory)
{
  const QFileInfo destination(directory);
  if (!destination.isDir() || isLink(destination))
    throw Error(QCoreApplication::translate("Enhanced", "Select an existing local destination folder."));
  return destination.canonicalFilePath();
}

class ResumeCheckpoint
{
public:
  ResumeCheckpoint(const QString &root, const SourceStamp &rootStamp)
      : m_root(root),
        m_rootStamp(rootStamp),
        m_checkpoint(m_root.filePath(QStringLiteral(".deskflow-resume-v1"))),
        m_content(m_checkpoint.filePath(QStringLiteral("files"))),
        m_metadataPath(m_checkpoint.filePath(QStringLiteral("metadata"))),
        m_lock(m_checkpoint.path() + QStringLiteral(".lock"))
  {
    m_lock.setStaleLockTime(0);
    if (!m_lock.tryLock(0))
      throw Error(
          QCoreApplication::translate("Enhanced", "Another process is using this destination's resume checkpoint.")
      );
  }

  void
  prepare(const TransferOptions &options, QByteArrayView identity, const QByteArray &manifest, const FileEntry &entry)
  {
    QByteArray expected("DFR1");
    expected += options.identity.fingerprint;
    expected += options.peerFingerprint;
    expected.append(identity.data(), identity.size());
    expected += manifest;

    checkRoot();
    const QFileInfo path(m_checkpoint.path());
    if (!path.exists() && !path.isSymLink()) {
      StagingDirectory fresh(m_root.path(), m_rootStamp);
      const QDir freshDir(fresh.path());
      QSaveFile metadata(freshDir.filePath(QStringLiteral("metadata")));
      if (!freshDir.mkdir(QStringLiteral("files"), PrivateDirectory) || !metadata.open(QIODevice::WriteOnly) ||
          metadata.write(expected) != expected.size() || !metadata.commit() || !fresh.publish(m_checkpoint.path())) {
        throw Error(QCoreApplication::translate("Enhanced", "Could not create the resume checkpoint."));
      }
    }

    m_checkpointStamp = stampPath(m_checkpoint.path(), true);
    m_contentStamp = stampPath(m_content.path(), true);
    checkPaths();
    if (readMetadata() != expected)
      throw Error(invalidCheckpoint());
    const auto age = QFileInfo(m_metadataPath).lastModified().secsTo(QDateTime::currentDateTimeUtc());
    if (age < 0 || age > CheckpointLifetime)
      throw Error(invalidCheckpoint());
    m_name = entry.path;
    checkContents();
    const auto distinctPath = m_content.filePath(m_name);
    if (!QFileInfo::exists(distinctPath)) {
      QFile fresh(distinctPath);
      if (!fresh.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw Error(invalidCheckpoint());
    }
    const auto stamp = stampPath(distinctPath, false);
    openTransferFile(file, distinctPath, Access::Update);
    if (!stamp || stampDescriptor(file.handle()) != stamp || stamp->size > entry.size)
      throw Error(invalidCheckpoint());
    m_fileStamp = *stamp;
  }

  quint64 retainedSize(quint64 declared) const
  {
    return m_fileStamp.size == declared ? declared : m_fileStamp.size - m_fileStamp.size % ChunkSize;
  }

  void publish(const QString &path)
  {
    checkPaths();
    checkContents();
    const auto stored = stampPath(m_content.filePath(m_name), false);
    if (!stored || stored->device != m_fileStamp.device || stored->file != m_fileStamp.file ||
        !publishDirectory(m_content.path(), path)) {
      throw Error(
          QCoreApplication::translate("Enhanced", "Could not publish the verified files in the selected destination.")
      );
    }
    // Publication is already committed. Leave an inert checkpoint if metadata removal fails.
    if (QFile::remove(m_metadataPath))
      m_root.rmdir(m_checkpoint.path());
  }

  void discard()
  {
    checkRoot();
    const QFileInfo path(m_checkpoint.path());
    if (!path.exists() && !path.isSymLink())
      return;
    m_checkpointStamp = stampPath(m_checkpoint.path(), true);
    if (!m_checkpointStamp)
      throw Error(invalidCheckpoint());
    const auto metadata = readMetadata();
    if (metadata.size() <= CheckpointHeaderSize || !metadata.startsWith("DFR1"))
      throw Error(invalidCheckpoint());
    const auto manifest = FileManifest::decode(QByteArrayView(metadata).sliced(CheckpointHeaderSize));
    if (!isSingleFile(manifest))
      throw Error(invalidCheckpoint());
    m_name = manifest.entries.front().path;
    m_contentStamp = stampPath(m_content.path(), true);
    checkContents();
    if (m_contentStamp) {
      checkPaths();
      const auto payload = m_content.filePath(m_name);
      if ((QFileInfo::exists(payload) && (!stampPath(payload, false) || !QFile::remove(payload))) ||
          !m_checkpoint.rmdir(QStringLiteral("files"))) {
        throw Error(invalidCheckpoint());
      }
    }
    checkRoot();
    if (!isSameDirectory(m_checkpoint.path(), m_checkpointStamp) || !QFile::remove(m_metadataPath) ||
        !m_root.rmdir(m_checkpoint.path())) {
      throw Error(invalidCheckpoint());
    }
  }

  QFile file;

private:
  void checkRoot() const
  {
    if (!isSameDirectory(m_root.path(), m_rootStamp))
      throw Error(QCoreApplication::translate("Enhanced", "The destination changed during the transfer."));
  }

  void checkPaths() const
  {
    checkRoot();
    if (!isSameDirectory(m_checkpoint.path(), m_checkpointStamp) || !isSameDirectory(m_content.path(), m_contentStamp))
      throw Error(invalidCheckpoint());
  }

  QByteArray readMetadata() const
  {
    const auto stamp = stampPath(m_metadataPath, false);
    if (!stamp || stamp->size > MaxPayload + CheckpointHeaderSize)
      throw Error(invalidCheckpoint());
    QFile metadata;
    openTransferFile(metadata, m_metadataPath, Access::Read);
    if (stampDescriptor(metadata.handle()) != stamp)
      throw Error(invalidCheckpoint());
    const auto result = metadata.read(static_cast<qint64>(stamp->size));
    if (result.size() != static_cast<qsizetype>(stamp->size))
      throw Error(invalidCheckpoint());
    return result;
  }

  void checkContents() const
  {
    constexpr auto visible = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
    const QStringList required = m_contentStamp ? QStringList{QStringLiteral("files"), QStringLiteral("metadata")}
                                                : QStringList{QStringLiteral("metadata")};
    if (m_checkpoint.entryList(visible, QDir::Name) != required)
      throw Error(invalidCheckpoint());
    if (m_contentStamp) {
      const auto files = m_content.entryList(visible, QDir::Name);
      if (!files.isEmpty() && files != QStringList{m_name})
        throw Error(invalidCheckpoint());
    }
  }

  const QDir m_root;
  const SourceStamp m_rootStamp;
  const QDir m_checkpoint;
  const QDir m_content;
  const QString m_metadataPath;
  QLockFile m_lock;
  std::optional<SourceStamp> m_checkpointStamp;
  std::optional<SourceStamp> m_contentStamp;
  SourceStamp m_fileStamp{};
  QString m_name;
};

void checkOptions(const TransferOptions &options)
{
  if (options.peerFingerprint.size() != TokenSize || options.peerFingerprint == options.identity.fingerprint ||
      options.controlPort == 0 || options.dataPort == 0 || options.controlPort == options.dataPort ||
      options.address.isNull() || (options.resume && options.dropHandoff)) {
    throw Error(
        QCoreApplication::translate(
            "Enhanced", "Enter a different peer's SHA-256 fingerprint, an IP address and two distinct ports."
        )
    );
  }
}

bool negotiateFiles(TlsChannel &control, const TransferOptions &options, bool sender)
{
  auto supported = fileCapability();
  if (options.resume)
    supported[0] |= ResumeCapability;
  if (options.dropHandoff)
    supported[0] |= DropCapability;
  control.send(Message::Hello, supported);
  const auto peer = control.receive(Message::Hello, 1, 1);
  if ((peer.at(0) & fileCapability().at(0)) == 0)
    throw Error(QCoreApplication::translate("Enhanced", "The peer does not support enhanced file transfer."));
  if (options.resume != ((peer.at(0) & ResumeCapability) != 0))
    throw Error(QCoreApplication::translate("Enhanced", "Both peers must enable single-file resume."));
  const bool dropHandoff = options.dropHandoff && (peer.at(0) & DropCapability) != 0;
  if (sender && options.dropHandoff && !dropHandoff)
    throw Error(QCoreApplication::translate("Enhanced", "The receiver has not enabled local file drop placement."));
  return dropHandoff;
}

struct AcceptedChannel
{
  std::unique_ptr<QTcpSocket> socket;
  std::unique_ptr<TlsChannel> tls;
};

AcceptedChannel acceptPinnedChannel(
    QTcpServer &server, const TransferOptions &options, const std::atomic_bool &cancelled, QDeadlineTimer deadline,
    QDeadlineTimer lifetime, QTcpSocket *controlSocket = nullptr, QByteArrayView token = {}
)
{
  for (;;) {
    auto socket = accept(server, deadline, cancelled, controlSocket);
    std::unique_ptr<TlsChannel> tls;
    const auto handshakeDeadline = (std::min)(deadline, QDeadlineTimer(HandshakeTimeout));
    try {
      tls = std::make_unique<TlsChannel>(
          *socket, options.identity, options.peerFingerprint, cancelled, true, handshakeDeadline, lifetime,
          controlSocket
      );
    } catch (const PeerHandshakeError &) {
      continue;
    }

    if (!token.isEmpty()) {
      // Possession of the pinned certificate alone does not authorize this batch.
      try {
        const auto proof = tls->receive(Message::DataOpen, token.size(), token.size(), handshakeDeadline);
        if (CRYPTO_memcmp(proof.constData(), token.data(), static_cast<size_t>(token.size())) != 0)
          continue;
      } catch (const Error &) {
        checkCancelled(cancelled);
        continue;
      }
    }
    return {std::move(socket), std::move(tls)};
  }
}

void sendContent(
    TlsChannel &data, QFile &file, QCryptographicHash &hash, quint64 offset, quint64 size, quint64 &completed,
    quint64 total, const std::atomic_bool &cancelled, const TransferProgress &progress
)
{
  while (offset < size) {
    checkCancelled(cancelled);
    const auto count = static_cast<qint64>(std::min<quint64>(size - offset, ChunkSize));
    const auto bytes = file.read(count);
    if (bytes.size() != count)
      throw Error(QCoreApplication::translate("Enhanced", "A selected file changed or could not be read."));
    hash.addData(bytes);
    data.send(Message::Chunk, bytes);
    offset += count;
    completed += count;
    if (progress)
      progress(completed, total);
  }
}

struct ResumeSource
{
  QFile file;
  QByteArray identity;
  QCryptographicHash hash{QCryptographicHash::Sha256};
  quint64 offset = 0;
};

void prepareResumeSource(ResumeSource &resume, const SourceEntry &source, const std::atomic_bool &cancelled)
{
  openTransferFile(resume.file, source.absolutePath, Access::Read);
  QCryptographicHash fullHash(QCryptographicHash::Sha256);
  hashPrefix(resume.file, fullHash, source.stamp.size, cancelled);
  if (stampDescriptor(resume.file.handle()) != source.stamp)
    throw Error(QCoreApplication::translate("Enhanced", "The source selection changed during the transfer."));
  resume.identity = sourceVersion(source.stamp) + fullHash.result();
}

void verifyResumePrefix(TlsChannel &control, ResumeSource &resume, quint64 total, const std::atomic_bool &cancelled)
{
  control.send(Message::ResumeIdentity, resume.identity);
  const auto offer = control.receive(
      Message::ResumeOffset, ResumeOffsetSize, ResumeOffsetSize, QDeadlineTimer(ResumePreparationTimeout)
  );
  resume.offset = qFromBigEndian<quint64>(offer.constData());
  if (resume.offset > total || (resume.offset != total && resume.offset % ChunkSize != 0))
    throw Error(QCoreApplication::translate("Enhanced", "The peer offered an invalid resume offset."));
  hashPrefix(resume.file, resume.hash, resume.offset, cancelled);
  if (resume.hash.result() != offer.sliced(sizeof(quint64)))
    throw Error(QCoreApplication::translate("Enhanced", "The retained transfer prefix differs from the source file."));
  control.send(Message::ResumeVerified);
}

void sendResume(
    TlsChannel &data, ResumeSource &resume, const Offer &offer, const std::atomic_bool &cancelled,
    const TransferProgress &progress
)
{
  quint64 completed = resume.offset;
  if (progress)
    progress(completed, offer.total);
  sendContent(data, resume.file, resume.hash, resume.offset, offer.total, completed, offer.total, cancelled, progress);
  const auto &source = offer.sources.front();
  const auto digest = resume.hash.result();
  if (stampDescriptor(resume.file.handle()) != source.stamp || !sourceMatches(source, offer.manifest.entries.front()) ||
      digest != resume.identity.last(TokenSize)) {
    throw Error(QCoreApplication::translate("Enhanced", "The source selection changed during the transfer."));
  }
  data.send(Message::Digest, digest);
  data.receive(Message::FileAck, 0, 0);
  data.send(Message::Finish);
}

void sendSelection(
    TlsChannel &data, const Offer &offer, const std::atomic_bool &cancelled, const TransferProgress &progress
)
{
  quint64 completed = 0;
  if (progress)
    progress(completed, offer.total);
  for (qsizetype i = 0; i < offer.sources.size(); ++i) {
    checkCancelled(cancelled);
    const auto &entry = offer.manifest.entries.at(i);
    const auto &source = offer.sources.at(i);
    if (entry.directory) {
      if (!sourceMatches(source, entry))
        throw Error(QCoreApplication::translate("Enhanced", "The source selection changed during the transfer."));
      continue;
    }

    QFile file;
    openTransferFile(file, source.absolutePath, Access::Read);
    if (stampDescriptor(file.handle()) != source.stamp)
      throw Error(QCoreApplication::translate("Enhanced", "The source selection changed during the transfer."));

    QCryptographicHash hash(QCryptographicHash::Sha256);
    sendContent(data, file, hash, 0, entry.size, completed, offer.total, cancelled, progress);

    if (stampDescriptor(file.handle()) != source.stamp || !sourceMatches(source, entry))
      throw Error(QCoreApplication::translate("Enhanced", "The source selection changed during the transfer."));
    data.send(Message::Digest, hash.result());
    data.receive(Message::FileAck, 0, 0);
  }
  for (qsizetype i = 0; i < offer.sources.size(); ++i) {
    checkCancelled(cancelled);
    if (!sourceMatches(offer.sources.at(i), offer.manifest.entries.at(i)))
      throw Error(QCoreApplication::translate("Enhanced", "The source selection changed during the transfer."));
  }
  data.send(Message::Finish);
}

void receiveContent(
    TlsChannel &data, QFile &file, QCryptographicHash &hash, quint64 offset, quint64 size, quint64 &completed,
    quint64 total, const TransferProgress &progress
)
{
  while (offset < size) {
    const auto count = static_cast<qsizetype>(std::min<quint64>(size - offset, ChunkSize));
    const auto bytes = data.receive(Message::Chunk, count, count);
    if (file.write(bytes) != count)
      throw Error(
          QCoreApplication::translate("Enhanced", "Could not write the received file. Check the available disk space.")
      );
    hash.addData(bytes);
    offset += count;
    completed += count;
    if (progress)
      progress(completed, total);
  }
}

void finishReceivingFile(TlsChannel &data, QFile &file, const QCryptographicHash &hash, QByteArrayView expected = {})
{
  const auto digest = hash.result();
  if (data.receive(Message::Digest, TokenSize, TokenSize) != digest || (!expected.isEmpty() && expected != digest))
    throw Error(QCoreApplication::translate("Enhanced", "A received file failed SHA-256 verification."));
  if (!file.flush())
    throw Error(QCoreApplication::translate("Enhanced", "Could not finish writing a received file."));
  file.close();
  if (file.error() != QFileDevice::NoError)
    throw Error(QCoreApplication::translate("Enhanced", "Could not close a received file."));
  data.send(Message::FileAck);
}

void receiveSelection(
    TlsChannel &data, const FileManifest &manifest, const StagingDirectory &staging, quint64 total,
    const std::atomic_bool &cancelled, const TransferProgress &progress
)
{
  const QDir destination(staging.path());
  quint64 completed = 0;
  if (progress)
    progress(completed, total);
  for (const auto &entry : manifest.entries) {
    checkCancelled(cancelled);
    staging.check();
    const auto entryPath = destination.filePath(entry.path);
    if (!stampPath(QFileInfo(entryPath).absolutePath(), true))
      throw Error(QCoreApplication::translate("Enhanced", "The destination changed during the transfer."));
    if (entry.directory) {
      if (!destination.mkdir(entry.path))
        throw Error(QCoreApplication::translate("Enhanced", "Could not create a received folder."));
      continue;
    }

    QFile file(entryPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
      throw Error(QCoreApplication::translate("Enhanced", "Could not create a received file."));

    QCryptographicHash hash(QCryptographicHash::Sha256);
    receiveContent(data, file, hash, 0, entry.size, completed, total, progress);
    finishReceivingFile(data, file, hash);
  }
  data.receive(Message::Finish, 0, 0);
}

} // namespace

bool sendFiles(
    const TransferOptions &options, const QStringList &paths, const std::atomic_bool &cancelled,
    const TransferProgress &progress
)
{
  checkOptions(options);
  const auto offer = buildOffer(paths, cancelled);
  const auto manifest = offer.manifest.encode();
  std::optional<ResumeSource> resume;
  if (options.resume) {
    if (!isSingleFile(offer.manifest))
      throw Error(QCoreApplication::translate("Enhanced", "Resume requires one ordinary file."));
    resume.emplace();
    prepareResumeSource(*resume, offer.sources.front(), cancelled);
  }

  const QDeadlineTimer lifetime(TransferTimeout);
  QTcpSocket controlSocket;
  auto deadline = QDeadlineTimer(HandshakeTimeout);
  connectSocket(controlSocket, options.address, options.controlPort, deadline, cancelled);
  TlsChannel control(controlSocket, options.identity, options.peerFingerprint, cancelled, false, deadline, lifetime);
  const bool dropHandoff = negotiateFiles(control, options, true);
  control.send(Message::Offer, manifest);
  if (resume)
    verifyResumePrefix(control, *resume, offer.total, cancelled);
  const auto token = control.receive(Message::Accept, TokenSize, TokenSize);

  QTcpSocket dataSocket;
  deadline = QDeadlineTimer(DataTimeout);
  connectSocket(dataSocket, options.address, options.dataPort, deadline, cancelled);
  TlsChannel data(
      dataSocket, options.identity, options.peerFingerprint, cancelled, false, deadline, lifetime, &controlSocket
  );
  data.send(Message::DataOpen, token, deadline);
  data.receive(Message::DataReady, 0, 0, deadline);
  if (resume)
    sendResume(data, *resume, offer, cancelled, progress);
  else
    sendSelection(data, offer, cancelled, progress);
  try {
    control.send(Message::Finish);
    control.receive(Message::Committed, 0, 0);
  } catch (const Error &error) {
    throw Error(
        QCoreApplication::translate(
            "Enhanced",
            "Could not confirm whether the receiver saved the files. Check the destination before retrying. %1"
        )
            .arg(error.message)
    );
  }
  return dropHandoff;
}

ReceivedFiles receiveFiles(
    const TransferOptions &options, const QString &directory, const std::atomic_bool &cancelled,
    const TransferProgress &progress, const std::function<void()> &listening
)
{
  checkOptions(options);
  const auto canonical = canonicalDestination(directory);
  const auto destinationStamp = canonical.isEmpty() ? std::nullopt : stampPath(canonical, true);
  if (!destinationStamp)
    throw Error(QCoreApplication::translate("Enhanced", "Could not resolve the destination folder."));

  QTcpServer controlServer;
  QTcpServer dataServer;
  const QDeadlineTimer lifetime(TransferTimeout);
  listen(controlServer, options.address, options.controlPort);
  if (listening)
    listening();

  const auto controlConnection = acceptPinnedChannel(
      controlServer, options, cancelled, QDeadlineTimer(options.resume ? ResumePreparationTimeout : OfferTimeout),
      lifetime
  );
  controlServer.close();
  auto &control = *controlConnection.tls;
  const bool dropHandoff = negotiateFiles(control, options, false);
  const auto encodedManifest = control.receive(Message::Offer, 4, MaxPayload);
  const auto manifest = FileManifest::decode(encodedManifest);
  const auto total = manifest.validate();

  if (!isSameDirectory(canonical, destinationStamp))
    throw Error(QCoreApplication::translate("Enhanced", "The destination changed during the transfer."));
  std::optional<StagingDirectory> staging;
  std::optional<ResumeCheckpoint> checkpoint;
  QCryptographicHash resumeHash(QCryptographicHash::Sha256);
  QByteArray resumeIdentity;
  quint64 offset = 0;
  if (options.resume) {
    if (!isSingleFile(manifest))
      throw Error(QCoreApplication::translate("Enhanced", "Resume requires one ordinary file."));
    resumeIdentity = control.receive(Message::ResumeIdentity, ResumeIdentitySize, ResumeIdentitySize);
    checkpoint.emplace(canonical, *destinationStamp);
    checkpoint->prepare(options, resumeIdentity, encodedManifest, manifest.entries.front());
    offset = checkpoint->retainedSize(total);
  }

  const QStorageInfo storage(canonical);
  if (storage.isReady() && storage.bytesAvailable() >= 0 &&
      static_cast<quint64>(storage.bytesAvailable()) < total - offset) {
    throw Error(QCoreApplication::translate("Enhanced", "There is not enough free space to receive this selection."));
  }
  if (checkpoint) {
    auto &file = checkpoint->file;
    if (static_cast<quint64>(file.size()) != offset && !file.resize(static_cast<qint64>(offset)))
      throw Error(invalidCheckpoint());
    hashPrefix(file, resumeHash, offset, cancelled);
    QByteArray prefix(sizeof(quint64), Qt::Uninitialized);
    qToBigEndian(offset, prefix.data());
    prefix += resumeHash.result();
    control.send(Message::ResumeOffset, prefix);
    control.receive(Message::ResumeVerified, 0, 0, QDeadlineTimer(ResumePreparationTimeout));
  } else {
    staging.emplace(canonical, *destinationStamp);
  }
  QByteArray token(TokenSize, Qt::Uninitialized);
  if (RAND_bytes(reinterpret_cast<unsigned char *>(token.data()), static_cast<int>(token.size())) != 1)
    throw Error(QCoreApplication::translate("Enhanced", "Could not authorize the enhanced data connection."));
  listen(dataServer, options.address, options.dataPort);
  control.send(Message::Accept, token);

  const auto deadline = QDeadlineTimer(DataTimeout);
  const auto dataConnection =
      acceptPinnedChannel(dataServer, options, cancelled, deadline, lifetime, controlConnection.socket.get(), token);
  dataServer.close();
  auto &data = *dataConnection.tls;
  data.send(Message::DataReady);
  if (checkpoint) {
    quint64 completed = offset;
    if (progress)
      progress(completed, total);
    receiveContent(data, checkpoint->file, resumeHash, offset, total, completed, total, progress);
    finishReceivingFile(data, checkpoint->file, resumeHash, QByteArrayView(resumeIdentity).last(TokenSize));
    data.receive(Message::Finish, 0, 0);
  } else {
    receiveSelection(data, manifest, *staging, total, cancelled, progress);
  }
  control.receive(Message::Finish, 0, 0);

  checkCancelled(cancelled);
  const auto published =
      QDir(canonical).filePath(QStringLiteral("Deskflow-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  if (checkpoint) {
    checkpoint->publish(published);
  } else if (!staging->publish(published)) {
    throw Error(
        QCoreApplication::translate("Enhanced", "Could not publish the verified files in the selected destination.")
    );
  }

  // The local commit stands even if the final acknowledgement cannot reach the sender.
  try {
    control.send(Message::Committed);
  } catch (const Error &) {
  }

  QStringList selection;
  const QDir publishedDirectory(published);
  for (const auto &entry : manifest.entries) {
    if (!entry.path.contains(u'/'))
      selection.append(publishedDirectory.filePath(entry.path));
  }
  return {published, selection, dropHandoff};
}

void discardResumeCheckpoint(const QString &directory)
{
  const auto canonical = canonicalDestination(directory);
  const auto stamp = stampPath(canonical, true);
  if (!stamp)
    throw Error(QCoreApplication::translate("Enhanced", "Could not resolve the destination folder."));
  ResumeCheckpoint checkpoint(canonical, *stamp);
  checkpoint.discard();
}

} // namespace deskflow::enhanced
