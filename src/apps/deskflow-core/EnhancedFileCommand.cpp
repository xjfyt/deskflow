/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EnhancedFileCommand.h"

#include "enhanced/FileTransfer.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>

#include <csignal>

namespace deskflow::enhanced {
namespace {

std::atomic_bool cancelled = false;
static_assert(std::atomic_bool::is_always_lock_free);

void cancel(int)
{
  cancelled.store(true, std::memory_order_relaxed);
}

quint16 port(const QCommandLineParser &parser, const QCommandLineOption &option)
{
  const auto value = parser.value(option).toUShort();
  if (value == 0)
    throw Error(QStringLiteral("Enter a port between 1 and 65535."));
  return value;
}

} // namespace

int fileCommand(int argc, char **argv)
{
  const QCoreApplication app(argc, argv);
  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(
      "Deskflow Enhanced file transfer\n"
      "Usage: deskflow-core --enhanced-files <identity|send|receive|discard-resume> [options] [paths...]\n"
      "Send uses the receiver IP. Receive binds to an explicit local IP and accepts one transfer.\n"
      "Use --resume on both peers for one file. An interrupted receive retains a checkpoint in the destination."
  ));
  parser.addHelpOption();
  parser.addPositionalArgument(QStringLiteral("mode"), QStringLiteral("identity, send, receive or discard-resume"));
  parser.addPositionalArgument(
      QStringLiteral("paths"), QStringLiteral("Source files/folders, or one existing destination folder."),
      QStringLiteral("[paths...]")
  );
  const QCommandLineOption certificate(
      QStringLiteral("certificate"), QStringLiteral("Local enhanced PEM identity file."), QStringLiteral("path")
  );
  const QCommandLineOption peer(
      QStringLiteral("peer"), QStringLiteral("Verified SHA-256 fingerprint of the other computer."),
      QStringLiteral("fingerprint")
  );
  const QCommandLineOption address(
      QStringLiteral("address"), QStringLiteral("Numeric receiver IP (send) or local bind IP (receive)."),
      QStringLiteral("IP")
  );
  const QCommandLineOption controlPort(
      QStringLiteral("control-port"), QStringLiteral("Enhanced control port."), QStringLiteral("port"),
      QString::number(ControlPort)
  );
  const QCommandLineOption dataPort(
      QStringLiteral("data-port"), QStringLiteral("Enhanced file data port."), QStringLiteral("port"),
      QString::number(DataPort)
  );
  const QCommandLineOption resume(
      QStringLiteral("resume"), QStringLiteral("Enable one authenticated resumable file on both peers.")
  );
  parser.addOptions({certificate, peer, address, controlPort, dataPort, resume});
  auto arguments = QCoreApplication::arguments();
  arguments.removeAt(1);
  parser.process(arguments);

  QTextStream output(stdout);
  try {
    const auto positions = parser.positionalArguments();
    const auto mode = positions.value(0);
    if (mode == QStringLiteral("discard-resume")) {
      if (positions.size() != 2 || !parser.optionNames().isEmpty())
        throw Error(QStringLiteral("Specify only the destination directory after discard-resume."));
      discardResumeCheckpoint(positions.at(1));
      output << "No resume checkpoint remains in the destination." << Qt::endl;
      return 0;
    }
    const auto path = parser.value(certificate);
    if (path.isEmpty())
      throw Error(QStringLiteral("--certificate is required."));
    if (mode == QStringLiteral("identity") && positions.size() == 1) {
      if (parser.isSet(resume))
        throw Error(QStringLiteral("--resume applies only to send and receive."));
      const auto identity = loadIdentity(path, true);
      output << identity.fingerprint.toHex(':').toUpper() << Qt::endl;
      return 0;
    }

    const TransferOptions options{
        loadIdentity(path),
        parseFingerprint(parser.value(peer)),
        QHostAddress(parser.value(address)),
        port(parser, controlPort),
        port(parser, dataPort),
        parser.isSet(resume)
    };
    std::signal(SIGINT, cancel);
    QElapsedTimer timer;
    timer.start();
    bool reportedProgress = false;
    const auto progress = [&](quint64 done, quint64 total) {
      if (!reportedProgress || done == total || timer.elapsed() >= 500) {
        output << done << " / " << total << " bytes" << Qt::endl;
        reportedProgress = true;
        timer.restart();
      }
    };
    if (mode == QStringLiteral("send") && positions.size() >= 2) {
      if (options.resume) {
        if (positions.size() != 2)
          throw Error(QStringLiteral("--resume requires exactly one source file."));
        output << "Verifying the source SHA-256 before connecting." << Qt::endl;
      }
      sendFiles(options, positions.sliced(1), cancelled, progress);
      output << "Receiver confirmed the transfer." << Qt::endl;
      return 0;
    }
    if (mode == QStringLiteral("receive") && positions.size() == 2) {
      const auto received = receiveFiles(options, positions.at(1), cancelled, progress, [&] {
        output << "Waiting for the verified peer on port " << options.controlPort << '.' << Qt::endl;
      });
      output << received.directory << Qt::endl;
      return 0;
    }
    throw Error(
        QStringLiteral("Specify identity, send with source paths, receive or discard-resume with one destination path.")
    );
  } catch (const std::exception &error) {
    QTextStream(stderr) << QString::fromUtf8(error.what()) << Qt::endl;
    return 1;
  }
}

} // namespace deskflow::enhanced
