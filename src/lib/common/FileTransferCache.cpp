/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileTransferCache.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>
#include <limits>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace deskflow {
namespace {
const auto markerName = QStringLiteral(".deskflow-filecopy-cache-v1");
const auto lockName = QStringLiteral(".deskflow-filecopy-cache.lock");

void require(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void checkCancelled(const FileTransferCache::Cancelled &cancelled)
{
  require(!cancelled || !cancelled(), "file cache: operation cancelled");
}

bool linked(const QString &path)
{
  if (QFileInfo(path).isSymLink())
    return true;
#ifdef Q_OS_WIN
  auto native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
  if (!native.startsWith(QStringLiteral("\\\\?\\")))
    native = native.startsWith(QStringLiteral("\\\\")) ? QStringLiteral("\\\\?\\UNC\\") + native.mid(2)
                                                       : QStringLiteral("\\\\?\\") + native;
  const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return false;
#endif
}

void checkAncestors(QString path)
{
  for (;;) {
    require(!linked(path), "file cache: symlink or reparse point is not supported");
    const auto parent = QFileInfo(path).dir().absolutePath();
    if (parent == path)
      break;
    path = parent;
  }
}

bool isManaged(const QString &root, const QString &batch)
{
  const QFileInfo info(batch);
  if (!info.isDir() || linked(batch) || info.dir().absolutePath() != root)
    return false;
  const auto name = info.fileName();
  if (!name.startsWith(QStringLiteral("batch-")))
    return false;
  const QUuid id(name.mid(6));
  if (id.isNull() || id.toString(QUuid::WithoutBraces) != name.mid(6))
    return false;
  const auto markerPath = QDir(batch).filePath(markerName);
  if (linked(markerPath))
    return false;
  QFile marker(markerPath);
  return marker.open(QIODevice::ReadOnly) && marker.size() <= 100 &&
         marker.readAll() == QByteArray("Deskflow FileCopy cache v1\n") + name.toUtf8() + '\n';
}

void walk(
    const QString &directory, FileTransferCache::Usage &usage, QStringList *paths,
    const FileTransferCache::Cancelled &cancelled, int depth = 0
)
{
  require(depth <= 66, "file cache: directory depth exceeds limit");
  checkCancelled(cancelled);
  checkAncestors(directory);
  require(QFileInfo(directory).isReadable(), "file cache: directory is unreadable");
#ifdef Q_OS_WIN
  // QDirIterator silently returns no entries after an access error.
  auto native = QDir::toNativeSeparators(directory) + QStringLiteral("\\*");
  if (!native.startsWith(QStringLiteral("\\\\?\\")))
    native = QStringLiteral("\\\\?\\") + native;
  WIN32_FIND_DATAW data{};
  const auto search = FindFirstFileW(reinterpret_cast<LPCWSTR>(native.utf16()), &data);
  if (search == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    require(error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES, "file cache: directory scan failed");
  } else {
    FindClose(search);
  }
#endif
  QDirIterator children(directory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
  while (children.hasNext()) {
    checkCancelled(cancelled);
    children.next();
    const auto child = children.fileInfo();
    require(++usage.entries <= 100000, "file cache: more than 100000 entries; use a dedicated cache folder");
    require(!linked(child.absoluteFilePath()), "file cache: symlink or reparse point is not supported");
    if (child.isDir()) {
      walk(child.absoluteFilePath(), usage, paths, cancelled, depth + 1);
    } else {
      require(child.isFile() && child.size() >= 0, "file cache: unsupported entry");
      const auto size = static_cast<quint64>(child.size());
      require(size <= (std::numeric_limits<quint64>::max)() - usage.bytes, "file cache: size overflow");
      usage.bytes += size;
    }
    if (paths)
      paths->append(child.absoluteFilePath());
  }
}

QString normalizedPath(const QString &path)
{
  auto nativePath = QDir::fromNativeSeparators(path);
#ifdef Q_OS_WIN
  if (nativePath.startsWith(QStringLiteral("//?/UNC/"), Qt::CaseInsensitive))
    nativePath = QStringLiteral("//") + nativePath.mid(8);
  else if (nativePath.startsWith(QStringLiteral("//?/")))
    nativePath = nativePath.mid(4);
#endif
  const auto canonical = QFileInfo(nativePath).canonicalFilePath();
  return QDir::cleanPath(canonical.isEmpty() ? QFileInfo(nativePath).absoluteFilePath() : canonical);
}

bool within(const QString &path, const QString &directory)
{
#ifdef Q_OS_WIN
  constexpr auto sensitivity = Qt::CaseInsensitive;
#else
  constexpr auto sensitivity = Qt::CaseSensitive;
#endif
  const auto normalized = normalizedPath(path);
  const auto parent = normalizedPath(directory);
  const auto prefix = parent.endsWith('/') ? parent : parent + '/';
  return normalized.compare(parent, sensitivity) == 0 || normalized.startsWith(prefix, sensitivity);
}

} // namespace

QString FileTransferCache::defaultRoot()
{
#ifdef Q_OS_WIN
  const auto base = qEnvironmentVariable("LOCALAPPDATA");
#else
  const auto base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
#endif
  require(!base.isEmpty(), "file cache: user cache directory is unavailable");
  return QDir(base).filePath(QStringLiteral("Deskflow/file-transfer"));
}

QString FileTransferCache::validatedRoot(const QString &root)
{
  const auto path = QDir::fromNativeSeparators(root.isEmpty() ? defaultRoot() : root);
  require(QFileInfo(path).isAbsolute(), "file cache: choose an absolute local folder");
  require(!path.split('/').contains(QStringLiteral("..")), "file cache: parent traversal is not allowed");
#ifdef Q_OS_WIN
  require(!path.startsWith('/'), "file cache: network and device paths are not supported");
  require(path.size() >= 3 && path[1] == ':' && path[2] == '/', "file cache: choose a local drive folder");
  require(!path.mid(2).contains(':'), "file cache: invalid drive path");
  const auto drive = QDir::toNativeSeparators(path.left(3));
  const auto driveType = GetDriveTypeW(reinterpret_cast<LPCWSTR>(drive.utf16()));
  require(driveType != DRIVE_REMOTE, "file cache: mapped network drives are not supported");
  require(driveType != DRIVE_UNKNOWN && driveType != DRIVE_NO_ROOT_DIR, "file cache: local drive is unavailable");
#endif
  const auto absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  require(!QDir(absolute).isRoot(), "file cache: a drive root cannot be used as the cache folder");
  checkAncestors(absolute);
  require(!QFileInfo::exists(absolute) || QFileInfo(absolute).isDir(), "file cache: path is not a folder");
  return absolute;
}

std::shared_ptr<QLockFile> FileTransferCache::lock(const QString &root)
{
  const auto path = validatedRoot(root);
  require(QDir().mkpath(path), "file cache: cannot create cache folder");
  checkAncestors(path);
  const auto lockPath = QDir(path).filePath(lockName);
  require(!linked(lockPath), "file cache: unsafe lock file");
  auto guard = std::make_shared<QLockFile>(lockPath);
  // A live transfer can legitimately last hours. Never break a live-process
  // lock just because it is old; QLockFile still detects dead local processes.
  guard->setStaleLockTime(0);
  require(guard->tryLock(0), "file cache: busy receiving files or managing cache; try again when finished");
  return guard;
}

QString FileTransferCache::createBatch(const QString &root)
{
  const auto path = validatedRoot(root);
  const auto name = QStringLiteral("batch-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto batch = QDir(path).filePath(name);
  require(QDir(path).mkdir(name), "file cache: cannot create unique batch folder");
  QFile marker(QDir(batch).filePath(markerName));
  const auto content = QByteArray("Deskflow FileCopy cache v1\n") + name.toUtf8() + '\n';
  if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly) || marker.write(content) != content.size() ||
      !marker.flush()) {
    marker.close();
    QFile::remove(marker.fileName());
    QDir().rmdir(batch);
    throw std::runtime_error("file cache: cannot mark batch folder");
  }
  marker.close();
  if (!QDir(batch).mkdir(QStringLiteral("files"))) {
    QFile::remove(marker.fileName());
    QDir().rmdir(batch);
    throw std::runtime_error("file cache: cannot create batch payload folder");
  }
  return batch;
}

FileTransferCache::Usage FileTransferCache::inspect(const QString &root, const Cancelled &cancelled)
{
  const auto path = validatedRoot(root);
  Usage usage;
  if (!QFileInfo::exists(path))
    return usage;
  walk(path, usage, nullptr, cancelled);
  const auto children = QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const auto &child : children) {
    checkCancelled(cancelled);
    usage.managedBatches += isManaged(path, child.absoluteFilePath()) ? 1 : 0;
  }
  return usage;
}

void FileTransferCache::removeBatch(const QString &root, const QString &batch, const Cancelled &cancelled)
{
  const auto path = validatedRoot(root);
  require(isManaged(path, batch), "file cache: refusing to remove an unmanaged batch");
  Usage usage;
  QStringList children;
  walk(batch, usage, &children, cancelled);
  // Validate every descendant before the first deletion. No recursive delete
  // API is used, and the marker remains until all payload entries are removed.
  const auto marker = QDir(batch).filePath(markerName);
  children.removeAll(marker);
  for (const auto &child : children) {
    checkCancelled(cancelled);
    require(within(child, batch), "file cache: deletion escaped batch folder");
    checkAncestors(child);
    const bool removed = QFileInfo(child).isDir() ? QDir().rmdir(child) : QFile::remove(child);
    require(removed, "file cache: cannot remove a cached entry; close programs using these files");
  }
  checkAncestors(marker);
  require(QFile::remove(marker), "file cache: cannot remove batch marker");
  require(QDir().rmdir(batch), "file cache: cannot remove batch folder");
}

void FileTransferCache::requireCapacity(quint64 used, quint64 incoming, quint64 limit, qint64 diskAvailable)
{
  require(
      used <= limit && incoming <= limit - used, "file transfer: cache quota exceeded; free space in the file cache"
  );
  require(
      diskAvailable >= 16 * 1024 * 1024 && incoming <= static_cast<quint64>(diskAvailable - 16 * 1024 * 1024),
      "file transfer: insufficient cache disk space"
  );
}
} // namespace deskflow
