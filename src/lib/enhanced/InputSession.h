/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "InputLease.h"
#include "TlsChannel.h"

#include <QSharedMemory>

#include <chrono>
#include <mutex>

namespace deskflow::enhanced {

inline constexpr auto EnhancedInputLockName = "deskflow-enhanced-input";
inline constexpr int InputWatchdog = 600;
inline constexpr char InputRelayCapability = '\x01';

struct InputSessionOptions
{
  PeerIdentity identity;
  QByteArray pin;
  QHostAddress address;
  quint16 port;
  bool listener;
  bool rightEdge;
  bool guiSession;
  bool viaHub;
};

InputSessionOptions parseInputOptions(const QStringList &arguments, const QString &description, bool allowGui);

class InputSessionGuard
{
public:
  InputSessionGuard();

private:
  QSharedMemory m_enhancedLock;
  QSharedMemory m_classicLock;
};

class InputExchange
{
public:
  using Clock = std::chrono::steady_clock;

  struct Update
  {
    InputState state;
    Clock::time_point receivedAt;
  };

  Update takeInbound();
  void request(quint32 epoch, quint16 position);
  void ready(quint32 epoch);
  void release(quint32 epoch);
  bool enqueue(quint32 epoch, InputEvent event);

  std::atomic_bool cancelled = false;
  QString error;

private:
  friend void runInputNetwork(InputExchange &, const InputSessionOptions &, QByteArrayView);

  InputPoll takeOutbound();
  void publish(const InputState &state, InputSide localSide);

  std::mutex m_mutex;
  InputPoll m_outbound;
  Update m_inbound{};
};

void runInputNetwork(InputExchange &exchange, const InputSessionOptions &options, QByteArrayView capability);

} // namespace deskflow::enhanced
