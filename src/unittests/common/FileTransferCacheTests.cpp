/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "common/FileTransferCache.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <winioctl.h>
#endif

using deskflow::FileTransferCache;
namespace {
bool write(const QString &path, const QByteArray &bytes = QByteArray("test"))
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

#ifdef Q_OS_WIN
bool junction(const QString &link, const QString &destination)
{
  if (!QDir().mkdir(link))
    return false;
  const auto native = QDir::toNativeSeparators(link);
  const auto target = QStringLiteral("\\??\\") + QDir::toNativeSeparators(destination);
  const auto handle = CreateFileW(
      reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr
  );
  if (handle == INVALID_HANDLE_VALUE)
    return false;
  struct Buffer
  {
    DWORD tag;
    WORD length;
    WORD reserved;
    WORD substituteOffset;
    WORD substituteLength;
    WORD printOffset;
    WORD printLength;
    wchar_t paths[1024];
  } buffer{};
  if (target.size() > 1000) {
    CloseHandle(handle);
    return false;
  }
  buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
  buffer.substituteLength = static_cast<WORD>(target.size() * sizeof(wchar_t));
  buffer.printOffset = buffer.substituteLength + sizeof(wchar_t);
  buffer.length = 8 + buffer.substituteLength + 2 * sizeof(wchar_t);
  memcpy(buffer.paths, target.utf16(), buffer.substituteLength);
  DWORD returned = 0;
  const bool result =
      DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer, 8 + buffer.length, nullptr, 0, &returned, nullptr);
  CloseHandle(handle);
  return result;
}
#endif
} // namespace

class FileTransferCacheTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void batchRemovalPreservesUnrelatedFilesAndOtherBatches()
  {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto lease = FileTransferCache::lock(root.path());
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::lock(root.path()), std::runtime_error);
    const auto first = FileTransferCache::createBatch(root.path());
    const auto second = FileTransferCache::createBatch(root.path());
    QVERIFY(write(first + "/files/remove.bin"));
    QVERIFY(write(second + "/files/keep.bin"));
    const auto unrelated = root.filePath("personal.txt");
    QVERIFY(write(unrelated));
    QCOMPARE(FileTransferCache::inspect(root.path()).managedBatches, 2);
    FileTransferCache::removeBatch(root.path(), first);
    QVERIFY(!QFileInfo::exists(first));
    QVERIFY(QFileInfo::exists(second + "/files/keep.bin"));
    QVERIFY(QFileInfo::exists(unrelated));
  }

  void markerAndDirectParentRequired()
  {
    QTemporaryDir root;
    QTemporaryDir outside;
    const auto validElsewhere = FileTransferCache::createBatch(outside.path());
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::removeBatch(root.path(), validElsewhere), std::runtime_error);
    const auto forged = root.filePath("batch-e0323340-d1a5-411b-8c6a-e8f33d17656f");
    QVERIFY(QDir().mkdir(forged));
    QVERIFY(write(forged + "/personal.bin"));
    QVERIFY(write(forged + "/.deskflow-filecopy-cache-v1", "invalid marker"));
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::removeBatch(root.path(), forged), std::runtime_error);
    QVERIFY(QFileInfo::exists(forged + "/personal.bin"));
  }

  void unsafePathsRejected()
  {
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::validatedRoot("relative/cache"), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::validatedRoot(QDir::rootPath()), std::runtime_error);
    QTemporaryDir root;
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::validatedRoot(root.path() + "/../other"), std::runtime_error);
#ifdef Q_OS_WIN
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::validatedRoot("\\\\server\\share\\cache"), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::validatedRoot("C:/cache:stream"), std::runtime_error);
#endif
  }

  void unavailableDriveIsRejected()
  {
#ifdef Q_OS_WIN
    const auto drives = GetLogicalDrives();
    QVERIFY(drives != 0);
    for (int index = 25; index >= 0; --index) {
      if (drives & (DWORD(1) << index))
        continue;
      const auto path = QString(QChar('A' + index)) + QStringLiteral(":/DeskflowCacheTest");
      QVERIFY_EXCEPTION_THROWN(FileTransferCache::validatedRoot(path), std::runtime_error);
      return;
    }
    QSKIP("No unused drive letter is available");
#else
    QSKIP("Windows drive validation test");
#endif
  }

  void mappedNetworkDriveIsRejected()
  {
#ifdef Q_OS_WIN
    const auto path = qEnvironmentVariable("DESKFLOW_TEST_REMOTE_CACHE");
    if (path.isEmpty())
      QSKIP("Set DESKFLOW_TEST_REMOTE_CACHE to an existing mapped network drive folder");
    const auto drive = QDir::toNativeSeparators(path.left(3));
    QCOMPARE(GetDriveTypeW(reinterpret_cast<LPCWSTR>(drive.utf16())), UINT(DRIVE_REMOTE));
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::validatedRoot(path), std::runtime_error);
#else
    QSKIP("Windows mapped network drive validation test");
#endif
  }

  void cancellationPreservesFiles()
  {
    QTemporaryDir root;
    const auto batch = FileTransferCache::createBatch(root.path());
    QVERIFY(write(batch + "/files/keep.bin"));
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::inspect(root.path(), [] { return true; }), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(
        FileTransferCache::removeBatch(root.path(), batch, [] { return true; }), std::runtime_error
    );
    QVERIFY(QFileInfo::exists(batch + "/files/keep.bin"));
  }

  void quotaAndLowDiskSpaceAreIndependent()
  {
    constexpr auto gib = FileTransferCache::gibibyte;
    FileTransferCache::requireCapacity(gib, gib, 2 * gib, 3 * gib);
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::requireCapacity(gib, gib + 1, 2 * gib, 4 * gib), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::requireCapacity(3 * gib, 0, 2 * gib, 4 * gib), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::requireCapacity(0, gib, 20 * gib, gib), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::requireCapacity(0, 0, 20 * gib, -1), std::runtime_error);
  }

  void junctionInsideManagedBatchNeverDeletesOutsideFiles()
  {
#ifdef Q_OS_WIN
    QTemporaryDir root;
    QTemporaryDir outside;
    const auto batch = FileTransferCache::createBatch(root.path());
    QVERIFY(write(outside.filePath("keep.bin")));
    const auto link = batch + "/files/junction";
    QVERIFY2(junction(link, outside.path()), "NTFS junction creation failed");
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::removeBatch(root.path(), batch), std::runtime_error);
    QVERIFY(QFileInfo::exists(outside.filePath("keep.bin")));
    QVERIFY_EXCEPTION_THROWN(FileTransferCache::validatedRoot(link), std::runtime_error);
    QVERIFY(QDir().rmdir(link));
#else
    QSKIP("Windows junction protection test");
#endif
  }

  void lockedCachedFileReportsFailureAndCanBeRetried()
  {
#ifdef Q_OS_WIN
    QTemporaryDir root;
    const auto batch = FileTransferCache::createBatch(root.path());
    const auto file = batch + "/files/locked.bin";
    QVERIFY(write(file));
    const auto native = QDir::toNativeSeparators(file);
    const auto handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr
    );
    QVERIFY(handle != INVALID_HANDLE_VALUE);
    bool removalFailed = false;
    try {
      FileTransferCache::removeBatch(root.path(), batch);
    } catch (const std::runtime_error &) {
      removalFailed = true;
    }
    CloseHandle(handle);
    QVERIFY(removalFailed);
    QVERIFY(QFileInfo::exists(file));
    FileTransferCache::removeBatch(root.path(), batch);
    QVERIFY(!QFileInfo::exists(batch));
#else
    QSKIP("Windows file sharing test");
#endif
  }
};

QTEST_GUILESS_MAIN(FileTransferCacheTests)
#include "FileTransferCacheTests.moc"
