/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "InputHub.h"

#include "InputRouter.h"
#include "InputSession.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <csignal>

namespace deskflow::enhanced {
namespace {

constexpr InputPeerId FirstEndpoint = 1;
constexpr InputPeerId SecondEndpoint = 2;
std::atomic_bool interrupted = false;
static_assert(std::atomic_bool::is_always_lock_free);

void interruptHub(int)
{
  interrupted.store(true, std::memory_order_relaxed);
}

struct HubPeer
{
  std::unique_ptr<QTcpSocket> socket;
  std::unique_ptr<TlsChannel> channel;
};

HubPeer authenticate(
    QTcpServer &server, const PeerIdentity &identity, const QByteArray &pin, const QByteArray &hello,
    const QDeadlineTimer &deadline
)
{
  while (true) {
    auto socket = accept(server, deadline, interrupted);
    const auto handshake = (std::min)(deadline, QDeadlineTimer(5000));
    std::unique_ptr<TlsChannel> channel;
    try {
      channel = std::make_unique<TlsChannel>(*socket, identity, pin, interrupted, true, handshake);
    } catch (const PeerHandshakeError &) {
      continue;
    }
    if (channel->receive(Message::Hello, hello.size(), hello.size(), handshake) != hello)
      throw Error(QStringLiteral("The endpoint must enable --via-hub and use the configured native backend."));
    server.close();
    return {std::move(socket), std::move(channel)};
  }
}

InputState stateForPeer(const RoutedInputLease &lease, InputPeerId peer)
{
  InputState state{InputSide::None, lease.epoch, lease.verticalPosition, {}, false};
  if (lease.route.origin == peer) {
    state.owner = InputSide::Connector;
    state.active = lease.active;
  } else if (lease.route.target == peer) {
    state.owner = InputSide::Listener;
    state.active = lease.active;
    state.events = lease.events;
  }
  return state;
}

void relayInput(TlsChannel &first, TlsChannel &second)
{
  constexpr std::array routes{InputRoute{FirstEndpoint, SecondEndpoint}, InputRoute{SecondEndpoint, FirstEndpoint}};
  RoutedInputLease lease;
  InputExchange::Clock::time_point pendingUntil{};
  while (!interrupted.load(std::memory_order_relaxed)) {
    const QDeadlineTimer deadline(InputWatchdog);
    const auto pollA = InputPoll::decode(first.receive(Message::InputPoll, InputHeaderSize, MaxInputPacket, deadline));
    const auto pollB = InputPoll::decode(second.receive(Message::InputPoll, InputHeaderSize, MaxInputPacket, deadline));
    const auto now = InputExchange::Clock::now();
    const auto previousEpoch = lease.epoch;
    if (lease.route.valid() && !lease.active && now >= pendingUntil) {
      advanceInputRoute(lease, {}, {});
    } else {
      const std::array polls{
          RoutedInputPoll{FirstEndpoint, SecondEndpoint, pollA}, RoutedInputPoll{SecondEndpoint, FirstEndpoint, pollB}
      };
      advanceInputRoute(lease, routes, polls);
    }
    if (lease.epoch != previousEpoch)
      pendingUntil = now + std::chrono::milliseconds(InputWatchdog);
    first.send(Message::InputState, stateForPeer(lease, FirstEndpoint).encode(), deadline);
    second.send(Message::InputState, stateForPeer(lease, SecondEndpoint).encode(), deadline);
  }
}

char inputFamily(const QString &family)
{
  if (family == QStringLiteral("windows"))
    return '\x02';
  if (family == QStringLiteral("x11"))
    return '\x03';
  if (family == QStringLiteral("macos"))
    return '\x04';
  return '\0';
}

} // namespace

int inputHubCommand(int argc, char **argv)
{
  const QCoreApplication app(argc, argv);
  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("Deskflow Enhanced input relay for two verified endpoints. Connect endpoint A first, then B.")
  );
  parser.addHelpOption();
  const QCommandLineOption certificate(
      QStringLiteral("certificate"), QStringLiteral("Local enhanced PEM identity file."), QStringLiteral("path")
  );
  const QCommandLineOption address(
      QStringLiteral("address"), QStringLiteral("Local IP to listen on."), QStringLiteral("IP")
  );
  const QCommandLineOption peerA(
      QStringLiteral("peer-a"), QStringLiteral("Verified SHA-256 fingerprint of endpoint A."), QStringLiteral("pin")
  );
  const QCommandLineOption peerB(
      QStringLiteral("peer-b"), QStringLiteral("Verified SHA-256 fingerprint of endpoint B."), QStringLiteral("pin")
  );
  const QCommandLineOption portA(
      QStringLiteral("port-a"), QStringLiteral("Input port for endpoint A."), QStringLiteral("port"),
      QStringLiteral("24803")
  );
  const QCommandLineOption portB(
      QStringLiteral("port-b"), QStringLiteral("Input port for endpoint B."), QStringLiteral("port"),
      QStringLiteral("24804")
  );
  const QCommandLineOption family(
      QStringLiteral("family"), QStringLiteral("Native input family shared by both endpoints."),
      QStringLiteral("windows|x11|macos")
  );
  parser.addOptions({certificate, address, peerA, peerB, portA, portB, family});
  auto args = QCoreApplication::arguments();
  args.removeAt(1);
  parser.process(args);

  try {
    const auto identity = loadIdentity(parser.value(certificate));
    const auto firstPin = parseFingerprint(parser.value(peerA));
    const auto secondPin = parseFingerprint(parser.value(peerB));
    const QHostAddress localAddress(parser.value(address));
    const auto firstPort = parser.value(portA).toUShort();
    const auto secondPort = parser.value(portB).toUShort();
    const auto version = inputFamily(parser.value(family));
    if (firstPin.isEmpty() || secondPin.isEmpty() || firstPin == secondPin || firstPin == identity.fingerprint ||
        secondPin == identity.fingerprint || firstPort == 0 || secondPort == 0 || firstPort == secondPort ||
        localAddress.isNull() || version == 0 || !parser.positionalArguments().isEmpty()) {
      throw Error(QStringLiteral("Specify distinct verified endpoint fingerprints and ports, a bind IP and a backend.")
      );
    }
    const std::array bytes{'\x02', version, InputRelayCapability};
    const QByteArray hello(bytes.data(), bytes.size());
    std::signal(SIGINT, interruptHub);
    QTcpServer firstServer;
    QTcpServer secondServer;
    listen(firstServer, localAddress, firstPort);
    listen(secondServer, localAddress, secondPort);
    QTextStream output(stdout);
    output << "Waiting for endpoint A on port " << firstPort << '.' << Qt::endl;
    const QDeadlineTimer deadline(120000);
    const auto a = authenticate(firstServer, identity, firstPin, hello, deadline);
    output << "Endpoint A verified. Waiting for endpoint B on port " << secondPort << '.' << Qt::endl;
    const auto b = authenticate(secondServer, identity, secondPin, hello, deadline);
    output << "Both endpoints verified. Relaying input between them." << Qt::endl;
    a.channel->send(Message::Hello, hello);
    b.channel->send(Message::Hello, hello);
    relayInput(*a.channel, *b.channel);
    return 0;
  } catch (const std::exception &error) {
    if (interrupted.load(std::memory_order_relaxed))
      return 0;
    QTextStream(stderr) << QString::fromUtf8(error.what()) << Qt::endl;
    return 1;
  }
}

} // namespace deskflow::enhanced
