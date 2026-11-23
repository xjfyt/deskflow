/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "enhanced/FileManifest.h"
#include "enhanced/Protocol.h"

#include <QtTest>

using namespace deskflow::enhanced;

class EnhancedProtocolTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void rejectsInvalidFrames()
  {
    const auto frame = encodeHeader(Message::Offer, 42, 1);
    QCOMPARE(decodeHeader(frame, 1).type, Message::Offer);
    QCOMPARE(decodeHeader(frame, 1).size, 42);
    QVERIFY_EXCEPTION_THROWN(decodeHeader(frame, 2), Error);
    QVERIFY_EXCEPTION_THROWN(decodeHeader(frame.first(4), 1), Error);
    auto badVersion = frame;
    badVersion[5] = 2;
    QVERIFY_EXCEPTION_THROWN(decodeHeader(badVersion, 1), Error);
    auto oversized = frame;
    oversized[12] = 1;
    QVERIFY_EXCEPTION_THROWN(decodeHeader(oversized, 1), Error);
  }

  void manifestPreservesUnicodeAndDirectories()
  {
    const FileManifest manifest{
        {{QStringLiteral("文件"), true, 0},
         {QStringLiteral("文件/空文件夹"), true, 0},
         {QStringLiteral("文件/表情😀.txt"), false, 42},
         {QStringLiteral("empty"), false, 0}}
    };
    const auto decoded = FileManifest::decode(manifest.encode());
    QCOMPARE(decoded.validate(), 42);
    QCOMPARE(decoded.entries.size(), 4);
    QCOMPARE(decoded.entries.at(2).path, manifest.entries.at(2).path);
  }

  void rejectsUnsafeManifest()
  {
    const FileManifest traversal{{{QStringLiteral("../outside"), false, 0}}};
    const FileManifest device{{{QStringLiteral("LPT¹.txt"), false, 0}}};
    const FileManifest duplicate{{{QStringLiteral("A"), true, 0}, {QStringLiteral("a"), false, 0}}};
    const FileManifest missingParent{{{QStringLiteral("a/b"), false, 0}}};
    const FileManifest overflow{{{QStringLiteral("large"), false, MaxTransferBytes + 1}}};
    QVERIFY_EXCEPTION_THROWN(traversal.encode(), Error);
    QVERIFY_EXCEPTION_THROWN(device.encode(), Error);
    QVERIFY_EXCEPTION_THROWN(duplicate.encode(), Error);
    QVERIFY_EXCEPTION_THROWN(missingParent.encode(), Error);
    QVERIFY_EXCEPTION_THROWN(overflow.encode(), Error);

    auto incomplete = FileManifest{{{QStringLiteral("a"), false, 1}}}.encode();
    incomplete.chop(1);
    QVERIFY_EXCEPTION_THROWN(FileManifest::decode(incomplete), Error);
  }
};

QTEST_GUILESS_MAIN(EnhancedProtocolTests)
#include "EnhancedProtocolTests.moc"
