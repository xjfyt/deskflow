/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "InputSession.h"

#include "common/Constants.h"

#include <QCommandLineParser>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
#include <utility>

namespace deskflow::enhanced {

InputSessionOptions parseInputOptions(const QStringList &arguments, const QString &description, bool allowGui)
{
  QCommandLineParser parser;
  parser.setApplicationDescription(description);
  parser.addHelpOption();
  parser.addPositionalArgument(QStringLiteral("mode"), QStringLiteral("listen or connect"));
  const QCommandLineOption certificate(
      QStringLiteral("certificate"), QStringLiteral("Local enhanced PEM identity file."), QStringLiteral("path")
  );
  const QCommandLineOption peer(
      QStringLiteral("peer"), QStringLiteral("Verified SHA-256 fingerprint of the other computer."),
      QStringLiteral("fingerprint")
  );
  const QCommandLineOption address(
      QStringLiteral("address"), QStringLiteral("Explicit local bind IP or remote listener IP."), QStringLiteral("IP")
  );
  const QCommandLineOption edge(
      QStringLiteral("edge"), QStringLiteral("Local screen edge facing the other computer."),
      QStringLiteral("left|right")
  );
  const QCommandLineOption port(
      QStringLiteral("port"), QStringLiteral("Enhanced input port."), QStringLiteral("port"), QStringLiteral("24803")
  );
  const QCommandLineOption guiSession(
      QStringLiteral("gui-session"), QStringLiteral("Run with a GUI-owned stdin control pipe.")
  );
  const QCommandLineOption viaHub(
      QStringLiteral("via-hub"), QStringLiteral("Allow a verified hub to relay input to its other paired endpoint.")
  );
  parser.addOptions({certificate, peer, address, edge, port, viaHub});
  if (allowGui)
    parser.addOption(guiSession);
  auto sessionArguments = arguments;
  sessionArguments.removeAt(1);
  parser.process(sessionArguments);

  const auto mode = parser.positionalArguments().value(0);
  const auto localEdge = parser.value(edge);
  const auto bindPort = parser.value(port).toUShort();
  const QHostAddress ip(parser.value(address));
  const auto pin = parseFingerprint(parser.value(peer));
  if (parser.positionalArguments().size() != 1 ||
      (mode != QStringLiteral("listen") && mode != QStringLiteral("connect")) ||
      (localEdge != QStringLiteral("right") && localEdge != QStringLiteral("left")) || bindPort == 0 || ip.isNull() ||
      pin.isEmpty() || parser.value(certificate).isEmpty()) {
    throw Error(
        QStringLiteral("Specify listen or connect, a certificate, peer fingerprint, IP address and screen edge.")
    );
  }
  const auto identity = loadIdentity(parser.value(certificate));
  if (identity.fingerprint == pin)
    throw Error(QStringLiteral("Enter the fingerprint of the other computer."));
  if (parser.isSet(viaHub) && mode == QStringLiteral("listen"))
    throw Error(QStringLiteral("--via-hub requires connect mode."));
  return {
      identity,
      pin,
      ip,
      bindPort,
      mode == QStringLiteral("listen"),
      localEdge == QStringLiteral("right"),
      allowGui && parser.isSet(guiSession),
      parser.isSet(viaHub)
  };
}

InputSessionGuard::InputSessionGuard() : m_enhancedLock(EnhancedInputLockName), m_classicLock(kCoreBinName)
{
  if (m_enhancedLock.attach())
    m_enhancedLock.detach();
  if (!m_enhancedLock.create(1))
    throw Error(QStringLiteral("An enhanced input session is already running."));

  if (m_classicLock.attach())
    m_classicLock.detach();
  if (!m_classicLock.create(1))
    throw Error(QStringLiteral("Stop the running Deskflow core before starting enhanced control."));
}

InputExchange::Update InputExchange::takeInbound()
{
  std::lock_guard lock(m_mutex);
  const auto update = m_inbound;
  m_inbound.state.events.clear();
  return update;
}

void InputExchange::request(quint32 epoch, quint16 position)
{
  std::lock_guard lock(m_mutex);
  m_outbound = {epoch, position, true, false, {}};
}

void InputExchange::ready(quint32 epoch)
{
  std::lock_guard lock(m_mutex);
  m_outbound = {epoch, 0, false, false, {}, true};
}

void InputExchange::release(quint32 epoch)
{
  std::lock_guard lock(m_mutex);
  m_outbound = {epoch, 0, false, true, {}};
}

bool InputExchange::enqueue(quint32 epoch, InputEvent event)
{
  std::lock_guard lock(m_mutex);
  m_outbound.epoch = epoch;
  if (queueInputEvent(m_outbound.events, event))
    return true;
  cancelled.store(true, std::memory_order_relaxed);
  return false;
}

InputPoll InputExchange::takeOutbound()
{
  std::lock_guard lock(m_mutex);
  return std::exchange(m_outbound, {});
}

void InputExchange::publish(const InputState &state, InputSide localSide)
{
  std::lock_guard lock(m_mutex);
  auto &inbound = m_inbound.state;
  if (inbound.epoch != state.epoch || inbound.owner != state.owner || inbound.active != state.active)
    inbound = {state.owner, state.epoch, state.verticalPosition, {}, state.active};
  if (state.owner != localSide) {
    for (const auto &event : state.events) {
      if (!queueInputEvent(inbound.events, event))
        throw Error(QStringLiteral("Enhanced input playback queue overflow."));
    }
  }
  m_inbound.receivedAt = Clock::now();
}

void runInputNetwork(InputExchange &exchange, const InputSessionOptions &options, QByteArrayView capability)
{
  try {
    QTcpSocket outgoing;
    std::unique_ptr<QTcpSocket> incoming;
    std::unique_ptr<TlsChannel> authenticated;
    if (options.listener) {
      QTcpServer server;
      listen(server, options.address, options.port);
      const QDeadlineTimer deadline(120000);
      while (!authenticated) {
        incoming = accept(server, deadline, exchange.cancelled);
        try {
          authenticated = std::make_unique<TlsChannel>(
              *incoming, options.identity, options.pin, exchange.cancelled, true,
              (std::min)(deadline, QDeadlineTimer(5000))
          );
        } catch (const PeerHandshakeError &) {
          incoming.reset();
        }
      }
    } else {
      connectSocket(outgoing, options.address, options.port, QDeadlineTimer(5000), exchange.cancelled);
      authenticated = std::make_unique<TlsChannel>(
          outgoing, options.identity, options.pin, exchange.cancelled, false, QDeadlineTimer(5000)
      );
    }

    auto &channel = *authenticated;
    QByteArray hello(capability.data(), capability.size());
    if (options.viaHub)
      hello += InputRelayCapability;
    channel.send(Message::Hello, hello, QDeadlineTimer(5000));
    const QDeadlineTimer helloDeadline(options.viaHub ? 120000 : 5000);
    if (channel.receive(Message::Hello, hello.size(), hello.size(), helloDeadline) != hello)
      throw Error(QStringLiteral("The verified peer does not support the same enhanced input backend."));

    InputState lease;
    InputExchange::Clock::time_point pendingUntil{};
    while (!exchange.cancelled.load(std::memory_order_relaxed)) {
      const QDeadlineTimer deadline(InputWatchdog);
      if (options.listener) {
        const auto peer =
            InputPoll::decode(channel.receive(Message::InputPoll, InputHeaderSize, MaxInputPacket, deadline));
        const auto local = exchange.takeOutbound();
        const auto now = InputExchange::Clock::now();
        const auto previousEpoch = lease.epoch;
        if (lease.owner != InputSide::None && !lease.active && now >= pendingUntil) {
          lease.owner = InputSide::None;
          lease.events.clear();
        } else {
          advanceLease(lease, local, peer);
        }
        if (lease.epoch != previousEpoch)
          pendingUntil = now + std::chrono::milliseconds(InputWatchdog);
        exchange.publish(lease, InputSide::Listener);
        if (lease.owner == InputSide::Connector)
          lease.events.clear();
        channel.send(Message::InputState, lease.encode(), deadline);
      } else {
        const auto sent = exchange.takeOutbound();
        channel.send(Message::InputPoll, sent.encode(), deadline);
        const auto state =
            InputState::decode(channel.receive(Message::InputState, InputHeaderSize, MaxInputPacket, deadline));
        validateLeaseTransition(lease, state, sent);
        exchange.publish(state, InputSide::Connector);
        lease = {state.owner, state.epoch, state.verticalPosition, {}, state.active};
        QThread::msleep(8);
      }
    }
  } catch (const std::exception &error) {
    if (!exchange.cancelled.load(std::memory_order_relaxed))
      exchange.error = QString::fromUtf8(error.what());
    exchange.cancelled.store(true, std::memory_order_relaxed);
  }
}

} // namespace deskflow::enhanced
