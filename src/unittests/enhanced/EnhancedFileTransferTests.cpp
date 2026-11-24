/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "enhanced/FileTransfer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

using namespace deskflow::enhanced;

class EnhancedFileTransferTests : public QObject
{
  Q_OBJECT

  enum Scenario
  {
    ValidPins,
    SenderPinRejected,
    ReceiverPinRejected,
    UnauthenticatedProbe,
    ReceiveCancelled,
    SourceReplaced
  };

private Q_SLOTS:
  void transfersAreAuthenticatedAndAtomic_data()
  {
    QTest::addColumn<int>("scenario");
    QTest::newRow("both pins valid") << int(ValidPins);
    QTest::newRow("invalid sender pin") << int(SenderPinRejected);
    QTest::newRow("invalid receiver pin") << int(ReceiverPinRejected);
    QTest::newRow("unauthenticated probe does not consume listener") << int(UnauthenticatedProbe);
    QTest::newRow("receiver cancellation removes staging") << int(ReceiveCancelled);
    QTest::newRow("same-size replacement with original timestamp is rejected") << int(SourceReplaced);
  }

  void transfersAreAuthenticatedAndAtomic()
  {
    QFETCH(int, scenario);

    const QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const auto a = loadIdentity(root.filePath(QStringLiteral("a.pem")), true);
    const auto b = loadIdentity(root.filePath(QStringLiteral("b.pem")), true);
    QVERIFY(root.mkpath(QStringLiteral("源文件/空目录")));
    QVERIFY(root.mkdir(QStringLiteral("received")));
    const auto sourceDirectory = root.filePath(QStringLiteral("源文件"));
    QFile source(QDir(sourceDirectory).filePath(QStringLiteral("样例.bin")));
    const QByteArray content(ChunkSize * 2 + 1, '\x29');
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(content), content.size());
    source.close();
    const auto sourceTime = QFileInfo(source).lastModified();

    QTcpServer controlPort;
    QTcpServer dataPort;
    QVERIFY(controlPort.listen(QHostAddress::LocalHost));
    QVERIFY(dataPort.listen(QHostAddress::LocalHost));
    const auto control = controlPort.serverPort();
    const auto data = dataPort.serverPort();
    controlPort.close();
    dataPort.close();

    const QByteArray wrongPin(TokenSize, '\x00');
    const TransferOptions sender{
        a, scenario == ReceiverPinRejected ? wrongPin : b.fingerprint, QHostAddress::LocalHost, control, data
    };
    const TransferOptions receiver{
        b, scenario == SenderPinRejected ? wrongPin : a.fingerprint, QHostAddress::LocalHost, control, data
    };
    const auto destination = root.filePath(QStringLiteral("received"));
    std::atomic_bool senderCancelled = false;
    std::atomic_bool receiverCancelled = false;
    bool halfwayCancelled = false;
    bool sourceReplaced = false;
    QSemaphore ready;
    ReceivedFiles received;
    QString receiveError;
    std::unique_ptr<QThread> worker(QThread::create([&] {
      try {
        received = receiveFiles(
            receiver, destination, receiverCancelled,
            [&](quint64 done, quint64 total) {
              if (scenario == ReceiveCancelled && done > 0 && done < total) {
                halfwayCancelled = true;
                receiverCancelled.store(true);
              }
            },
            [&] { ready.release(); }
        );
      } catch (const std::exception &error) {
        receiveError = QString::fromUtf8(error.what());
        ready.release();
      }
    }));
    worker->start();

    QString sendError;
    if (ready.tryAcquire(1, 5000)) {
      try {
        if (scenario == UnauthenticatedProbe) {
          QTcpSocket probe;
          connectSocket(probe, QHostAddress::LocalHost, control, QDeadlineTimer(5000), senderCancelled);
          probe.abort();
        }
        const auto selected = scenario == SourceReplaced ? source.fileName() : sourceDirectory;
        sendFiles(sender, {selected}, senderCancelled, [&](quint64 done, quint64) {
          if (scenario != SourceReplaced || done != 0)
            return;
          QFile replacement(source.fileName() + QStringLiteral(".replacement"));
          const QByteArray newContent(content.size(), '\x34');
          if (!replacement.open(QIODevice::WriteOnly) || replacement.write(newContent) != newContent.size() ||
              !replacement.flush()) {
            return;
          }
          replacement.close();
          if (!replacement.open(QIODevice::ReadWrite))
            return;
          const bool timeRestored = replacement.setFileTime(sourceTime, QFileDevice::FileModificationTime);
          replacement.close();
          sourceReplaced =
              timeRestored && QFile::rename(source.fileName(), source.fileName() + QStringLiteral(".original")) &&
              replacement.rename(source.fileName()) && QFileInfo(source.fileName()).lastModified() == sourceTime;
        });
      } catch (const std::exception &error) {
        sendError = QString::fromUtf8(error.what());
        receiverCancelled.store(true);
      }
    } else {
      sendError = QStringLiteral("Receiver did not become ready.");
      receiverCancelled.store(true);
    }
    worker->wait();

    if (scenario != ValidPins && scenario != UnauthenticatedProbe) {
      if (scenario == ReceiveCancelled)
        QVERIFY(halfwayCancelled);
      if (scenario == SourceReplaced)
        QVERIFY(sourceReplaced);
      QVERIFY(!sendError.isEmpty());
      QVERIFY(!receiveError.isEmpty());
      QVERIFY(QDir(destination).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden));
      return;
    }
    QVERIFY2(sendError.isEmpty(), qPrintable(sendError));
    QVERIFY2(receiveError.isEmpty(), qPrintable(receiveError));
    QCOMPARE(received.selection.size(), 1);
    const QDir selection(received.selection.first());
    QVERIFY(selection.exists(QStringLiteral("空目录")));
    QFile result(selection.filePath(QStringLiteral("样例.bin")));
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(result.readAll(), content);
    QVERIFY(source.exists());
  }
};

QTEST_GUILESS_MAIN(EnhancedFileTransferTests)
#include "EnhancedFileTransferTests.moc"
