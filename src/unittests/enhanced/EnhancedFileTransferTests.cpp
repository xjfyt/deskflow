/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "enhanced/FileTransfer.h"

#include <QDir>
#include <QFile>
#include <QSemaphore>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

using namespace deskflow::enhanced;

class EnhancedFileTransferTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void transfersFilesOnlyWithBothPins_data()
  {
    QTest::addColumn<bool>("senderTrustsReceiver");
    QTest::addColumn<bool>("receiverTrustsSender");
    QTest::newRow("both pins valid") << true << true;
    QTest::newRow("invalid sender pin") << true << false;
    QTest::newRow("invalid receiver pin") << false << true;
  }

  void transfersFilesOnlyWithBothPins()
  {
    QFETCH(bool, senderTrustsReceiver);
    QFETCH(bool, receiverTrustsSender);

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
        a, senderTrustsReceiver ? b.fingerprint : wrongPin, QHostAddress::LocalHost, control, data
    };
    const TransferOptions receiver{
        b, receiverTrustsSender ? a.fingerprint : wrongPin, QHostAddress::LocalHost, control, data
    };
    const auto destination = root.filePath(QStringLiteral("received"));
    std::atomic_bool cancelled = false;
    QSemaphore ready;
    ReceivedFiles received;
    QString receiveError;
    std::unique_ptr<QThread> worker(QThread::create([&] {
      try {
        received = receiveFiles(receiver, destination, cancelled, {}, [&] { ready.release(); });
      } catch (const std::exception &error) {
        receiveError = QString::fromUtf8(error.what());
        ready.release();
      }
    }));
    worker->start();

    QString sendError;
    if (ready.tryAcquire(1, 5000)) {
      try {
        sendFiles(sender, {sourceDirectory}, cancelled);
      } catch (const std::exception &error) {
        sendError = QString::fromUtf8(error.what());
        cancelled.store(true);
      }
    } else {
      sendError = QStringLiteral("Receiver did not become ready.");
      cancelled.store(true);
    }
    worker->wait();

    if (!senderTrustsReceiver || !receiverTrustsSender) {
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
