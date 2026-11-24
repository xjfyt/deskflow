/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransfer.h"

#include "FileManifest.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>

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

HANDLE openWindowsPath(const QString &path, bool directory, bool readData = false)
{
  const auto native = QDir::toNativeSeparators(path);
  const auto handle = CreateFileW(
      reinterpret_cast<LPCWSTR>(native.utf16()), readData ? GENERIC_READ : FILE_READ_ATTRIBUTES,
      readData ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
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

HANDLE openSourcePath(const QString &path, bool directory, bool readData = false)
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
  return openWindowsPath(path, directory, readData);
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

int openSource(const QString &path)
{
  const auto handle = openSourcePath(path, false, true);
  if (handle == INVALID_HANDLE_VALUE)
    return -1;
  const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
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
int openSourcePath(const QString &path, bool directory)
{
  const auto components = path.split(u'/', Qt::SkipEmptyParts);
  if (components.isEmpty() && !directory)
    return -1;
  auto current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  for (qsizetype i = 0; current >= 0 && i < components.size(); ++i) {
    const auto type = (directory || i < components.size() - 1) ? O_DIRECTORY : O_NONBLOCK;
    const auto next =
        ::openat(current, QFile::encodeName(components[i]).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | type);
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

int openSource(const QString &path)
{
  return openSourcePath(path, false);
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

void checkOptions(const TransferOptions &options)
{
  if (options.peerFingerprint.size() != TokenSize || options.peerFingerprint == options.identity.fingerprint ||
      options.controlPort == 0 || options.dataPort == 0 || options.controlPort == options.dataPort ||
      options.address.isNull()) {
    throw Error(
        QCoreApplication::translate(
            "Enhanced", "Enter a different peer's SHA-256 fingerprint, an IP address and two distinct ports."
        )
    );
  }
}

void negotiateFiles(TlsChannel &control)
{
  control.send(Message::Hello, fileCapability());
  const auto peer = control.receive(Message::Hello, 1, 1);
  if ((peer.at(0) & fileCapability().at(0)) == 0)
    throw Error(QCoreApplication::translate("Enhanced", "The peer does not support enhanced file transfer."));
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
    const auto descriptor = openSource(source.absolutePath);
    if (descriptor < 0)
      throw Error(QCoreApplication::translate("Enhanced", "A selected file could not be opened safely."));
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
#if defined(Q_OS_WIN)
      _close(descriptor);
#else
      ::close(descriptor);
#endif
      throw Error(QCoreApplication::translate("Enhanced", "A selected file could not be opened safely."));
    }
    if (stampDescriptor(descriptor) != source.stamp)
      throw Error(QCoreApplication::translate("Enhanced", "The source selection changed during the transfer."));

    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (quint64 offset = 0; offset < entry.size;) {
      checkCancelled(cancelled);
      const auto count = static_cast<qint64>(std::min<quint64>(entry.size - offset, ChunkSize));
      const auto bytes = file.read(count);
      if (bytes.size() != count)
        throw Error(QCoreApplication::translate("Enhanced", "A selected file changed or could not be read."));
      hash.addData(bytes);
      data.send(Message::Chunk, bytes);
      offset += count;
      completed += count;
      if (progress)
        progress(completed, offer.total);
    }

    if (stampDescriptor(descriptor) != source.stamp || !sourceMatches(source, entry))
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
    for (quint64 offset = 0; offset < entry.size;) {
      const auto count = static_cast<qsizetype>(std::min<quint64>(entry.size - offset, ChunkSize));
      const auto bytes = data.receive(Message::Chunk, count, count);
      if (file.write(bytes) != count)
        throw Error(
            QCoreApplication::translate(
                "Enhanced", "Could not write the received file. Check the available disk space."
            )
        );
      hash.addData(bytes);
      offset += count;
      completed += count;
      if (progress)
        progress(completed, total);
    }

    if (data.receive(Message::Digest, TokenSize, TokenSize) != hash.result())
      throw Error(QCoreApplication::translate("Enhanced", "A received file failed SHA-256 verification."));
    if (!file.flush())
      throw Error(QCoreApplication::translate("Enhanced", "Could not finish writing a received file."));
    file.close();
    if (file.error() != QFileDevice::NoError)
      throw Error(QCoreApplication::translate("Enhanced", "Could not close a received file."));
    data.send(Message::FileAck);
  }
  data.receive(Message::Finish, 0, 0);
}

} // namespace

void sendFiles(
    const TransferOptions &options, const QStringList &paths, const std::atomic_bool &cancelled,
    const TransferProgress &progress
)
{
  checkOptions(options);
  const auto offer = buildOffer(paths, cancelled);
  const auto manifest = offer.manifest.encode();

  const QDeadlineTimer lifetime(TransferTimeout);
  QTcpSocket controlSocket;
  auto deadline = QDeadlineTimer(HandshakeTimeout);
  connectSocket(controlSocket, options.address, options.controlPort, deadline, cancelled);
  TlsChannel control(controlSocket, options.identity, options.peerFingerprint, cancelled, false, deadline, lifetime);
  negotiateFiles(control);
  control.send(Message::Offer, manifest);
  const auto token = control.receive(Message::Accept, TokenSize, TokenSize);

  QTcpSocket dataSocket;
  deadline = QDeadlineTimer(DataTimeout);
  connectSocket(dataSocket, options.address, options.dataPort, deadline, cancelled);
  TlsChannel data(
      dataSocket, options.identity, options.peerFingerprint, cancelled, false, deadline, lifetime, &controlSocket
  );
  data.send(Message::DataOpen, token, deadline);
  data.receive(Message::DataReady, 0, 0, deadline);
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
}

ReceivedFiles receiveFiles(
    const TransferOptions &options, const QString &directory, const std::atomic_bool &cancelled,
    const TransferProgress &progress, const std::function<void()> &listening
)
{
  checkOptions(options);
  const QFileInfo destination(directory);
  if (!destination.isDir() || isLink(destination))
    throw Error(QCoreApplication::translate("Enhanced", "Select an existing local destination folder."));
  const auto canonical = destination.canonicalFilePath();
  const auto destinationStamp = canonical.isEmpty() ? std::nullopt : stampPath(canonical, true);
  if (!destinationStamp)
    throw Error(QCoreApplication::translate("Enhanced", "Could not resolve the destination folder."));

  QTcpServer controlServer;
  QTcpServer dataServer;
  const QDeadlineTimer lifetime(TransferTimeout);
  listen(controlServer, options.address, options.controlPort);
  if (listening)
    listening();

  const auto controlConnection =
      acceptPinnedChannel(controlServer, options, cancelled, QDeadlineTimer(OfferTimeout), lifetime);
  controlServer.close();
  auto &control = *controlConnection.tls;
  negotiateFiles(control);
  const auto manifest = FileManifest::decode(control.receive(Message::Offer, 4, MaxPayload));
  const auto total = manifest.validate();

  const QStorageInfo storage(canonical);
  if (storage.isReady() && storage.bytesAvailable() >= 0 && static_cast<quint64>(storage.bytesAvailable()) < total)
    throw Error(QCoreApplication::translate("Enhanced", "There is not enough free space to receive this selection."));
  if (!isSameDirectory(canonical, destinationStamp))
    throw Error(QCoreApplication::translate("Enhanced", "The destination changed during the transfer."));
  StagingDirectory staging(canonical, *destinationStamp);
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
  receiveSelection(data, manifest, staging, total, cancelled, progress);
  control.receive(Message::Finish, 0, 0);

  checkCancelled(cancelled);
  const auto published =
      QDir(canonical).filePath(QStringLiteral("Deskflow-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  if (!staging.publish(published))
    throw Error(
        QCoreApplication::translate("Enhanced", "Could not publish the verified files in the selected destination.")
    );

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
  return {published, selection};
}

} // namespace deskflow::enhanced
