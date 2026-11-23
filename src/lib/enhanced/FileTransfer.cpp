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
#include <QStorageInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>

#include <openssl/rand.h>

#include <algorithm>
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

struct SourceEntry
{
  QString absolutePath;
  QDateTime modified;
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
  const auto size = info.isDir() ? 0 : static_cast<quint64>(info.size());
  if (size > MaxTransferBytes - offer.total)
    throw Error(QCoreApplication::translate("Enhanced", "The selection is larger than the 20 GiB transfer limit."));
  offer.total += size;
  offer.manifest.entries.append({relativePath, info.isDir(), size});
  offer.sources.append({info.absoluteFilePath(), info.lastModified()});

  if (!info.isDir())
    return;
  if (!info.isReadable())
    throw Error(QCoreApplication::translate("Enhanced", "A selected directory is unreadable."));

  QDirIterator children(info.absoluteFilePath(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
  while (children.hasNext()) {
    const auto child = QFileInfo(children.next());
    collect(offer, child, relativePath + u'/' + child.fileName(), depth + 1, cancelled);
  }
}

bool sourceMatches(const SourceEntry &source, const FileEntry &entry)
{
  const QFileInfo info(source.absolutePath);
  return !isLink(info) && info.lastModified() == source.modified &&
         (entry.directory ? info.isDir() : info.isFile() && static_cast<quint64>(info.size()) == entry.size);
}

Offer buildOffer(const QStringList &paths, const std::atomic_bool &cancelled)
{
  Offer offer;
  for (const auto &path : paths) {
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

HANDLE openWindowsPath(const QString &path, bool directory)
{
  const auto native = QDir::toNativeSeparators(path);
  const auto handle = CreateFileW(
      reinterpret_cast<LPCWSTR>(native.utf16()), directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
      directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN), nullptr
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

int openSource(const QString &path)
{
  QStringList parents;
  QDir directory = QFileInfo(path).absoluteDir();
  while (!directory.isRoot()) {
    parents.prepend(directory.absolutePath());
    if (!directory.cdUp())
      return -1;
  }
  DirectoryHandles directories;
  directories.handles.reserve(static_cast<size_t>(parents.size()));
  for (const auto &parent : parents) {
    auto handle = openWindowsPath(parent, true);
    if (handle == INVALID_HANDLE_VALUE)
      return -1;
    directories.handles.push_back(handle);
  }
  const auto handle = openWindowsPath(path, false);
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
int openSource(const QString &path)
{
  const auto components = path.split(u'/', Qt::SkipEmptyParts);
  if (components.isEmpty())
    return -1;
  auto current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  for (qsizetype i = 0; current >= 0 && i < components.size(); ++i) {
    const auto type = i == components.size() - 1 ? O_NONBLOCK : O_DIRECTORY;
    const auto next =
        ::openat(current, QFile::encodeName(components[i]).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | type);
    ::close(current);
    current = next;
  }
  return current;
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
    if (!sourceMatches(source, entry))
      throw Error(QCoreApplication::translate("Enhanced", "The source selection changed during the transfer."));
    if (entry.directory)
      continue;

    QFile file;
    const auto descriptor = openSource(source.absolutePath);
    if (descriptor < 0)
      throw Error(QCoreApplication::translate("Enhanced", "A selected file could not be opened safely."));
#if !defined(Q_OS_WIN)
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        static_cast<quint64>(status.st_size) != entry.size) {
      ::close(descriptor);
      throw Error(QCoreApplication::translate("Enhanced", "The source selection changed during the transfer."));
    }
#endif
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
#if defined(Q_OS_WIN)
      _close(descriptor);
#else
      ::close(descriptor);
#endif
      throw Error(QCoreApplication::translate("Enhanced", "A selected file could not be opened safely."));
    }
    if (static_cast<quint64>(file.size()) != entry.size || !sourceMatches(source, entry))
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

    if (static_cast<quint64>(file.size()) != entry.size || !sourceMatches(source, entry))
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
    TlsChannel &data, const FileManifest &manifest, const QDir &destination, quint64 total,
    const std::atomic_bool &cancelled, const TransferProgress &progress
)
{
  quint64 completed = 0;
  if (progress)
    progress(completed, total);
  for (const auto &entry : manifest.entries) {
    checkCancelled(cancelled);
    if (entry.directory) {
      if (!destination.mkdir(entry.path))
        throw Error(QCoreApplication::translate("Enhanced", "Could not create a received folder."));
      continue;
    }

    QFile file(destination.filePath(entry.path));
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

  QTcpSocket controlSocket;
  auto deadline = QDeadlineTimer(HandshakeTimeout);
  connectSocket(controlSocket, options.address, options.controlPort, deadline, cancelled);
  TlsChannel control(controlSocket, options.identity, options.peerFingerprint, cancelled, false, deadline);
  negotiateFiles(control);
  control.send(Message::Offer, manifest);
  const auto token = control.receive(Message::Accept, TokenSize, TokenSize);

  QTcpSocket dataSocket;
  deadline = QDeadlineTimer(DataTimeout);
  connectSocket(dataSocket, options.address, options.dataPort, deadline, cancelled);
  TlsChannel data(dataSocket, options.identity, options.peerFingerprint, cancelled, false, deadline);
  data.send(Message::DataOpen, token);
  data.receive(Message::DataReady, 0, 0, deadline);
  sendSelection(data, offer, cancelled, progress);
  control.send(Message::Finish);
  control.receive(Message::Committed, 0, 0);
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
  if (canonical.isEmpty())
    throw Error(QCoreApplication::translate("Enhanced", "Could not resolve the destination folder."));

  QTcpServer controlServer;
  QTcpServer dataServer;
  listen(controlServer, options.address, options.controlPort);
  if (listening)
    listening();

  const auto controlSocket = accept(controlServer, QDeadlineTimer(OfferTimeout), cancelled);
  controlServer.close();
  TlsChannel control(
      *controlSocket, options.identity, options.peerFingerprint, cancelled, true, QDeadlineTimer(HandshakeTimeout)
  );
  negotiateFiles(control);
  const auto manifest = FileManifest::decode(control.receive(Message::Offer, 4, MaxPayload));
  const auto total = manifest.validate();

  const QStorageInfo storage(canonical);
  if (storage.isReady() && storage.bytesAvailable() >= 0 && static_cast<quint64>(storage.bytesAvailable()) < total)
    throw Error(QCoreApplication::translate("Enhanced", "There is not enough free space to receive this selection."));
  QTemporaryDir staging(QDir(canonical).filePath(QStringLiteral(".deskflow-XXXXXX")));
  if (!staging.isValid())
    throw Error(QCoreApplication::translate("Enhanced", "Could not create a private transfer folder."));
  QByteArray token(TokenSize, Qt::Uninitialized);
  if (RAND_bytes(reinterpret_cast<unsigned char *>(token.data()), static_cast<int>(token.size())) != 1)
    throw Error(QCoreApplication::translate("Enhanced", "Could not authorize the enhanced data connection."));
  listen(dataServer, options.address, options.dataPort);
  control.send(Message::Accept, token);

  const auto deadline = QDeadlineTimer(DataTimeout);
  const auto dataSocket = accept(dataServer, deadline, cancelled);
  dataServer.close();
  TlsChannel data(*dataSocket, options.identity, options.peerFingerprint, cancelled, true, deadline);
  if (data.receive(Message::DataOpen, TokenSize, TokenSize, deadline) != token)
    throw Error(QCoreApplication::translate("Enhanced", "The enhanced data connection was not authorized."));
  data.send(Message::DataReady);
  receiveSelection(data, manifest, QDir(staging.path()), total, cancelled, progress);
  control.receive(Message::Finish, 0, 0);

  checkCancelled(cancelled);
  const auto published =
      QDir(canonical).filePath(QStringLiteral("Deskflow-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  if (!publishDirectory(staging.path(), published))
    throw Error(
        QCoreApplication::translate("Enhanced", "Could not publish the verified files in the selected destination.")
    );
  staging.setAutoRemove(false);

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
