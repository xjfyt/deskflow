/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "Protocol.h"

#include <QDeadlineTimer>
#include <QHostAddress>

#include <atomic>
#include <memory>

class QTcpServer;
class QTcpSocket;

namespace deskflow::enhanced {

struct PeerIdentity
{
  QString pemPath;
  QByteArray fingerprint;
};

PeerIdentity loadIdentity(const QString &path, bool create = false);

class TlsChannel
{
public:
  TlsChannel(
      QTcpSocket &socket, const PeerIdentity &identity, const QByteArray &pin, const std::atomic_bool &cancelled,
      bool server, QDeadlineTimer deadline
  );
  ~TlsChannel();

  void send(Message type, QByteArrayView data = {}, QDeadlineTimer deadline = QDeadlineTimer(15000));
  QByteArray
  receive(Message type, qsizetype minimum, qsizetype maximum, QDeadlineTimer deadline = QDeadlineTimer(15000));

private:
  struct State;
  std::unique_ptr<State> m_state;
  quint64 m_sendSequence = 0;
  quint64 m_receiveSequence = 0;
};

void checkCancelled(const std::atomic_bool &cancelled);
void listen(QTcpServer &server, const QHostAddress &address, quint16 port);
std::unique_ptr<QTcpSocket> accept(QTcpServer &server, QDeadlineTimer deadline, const std::atomic_bool &cancelled);
void connectSocket(
    QTcpSocket &socket, const QHostAddress &address, quint16 port, QDeadlineTimer deadline,
    const std::atomic_bool &cancelled
);

} // namespace deskflow::enhanced
