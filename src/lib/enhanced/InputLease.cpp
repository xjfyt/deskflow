/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "InputLease.h"

#include "Protocol.h"

#include <QtEndian>

#include <limits>

namespace deskflow::enhanced {
namespace {

QByteArray encode(quint8 flags, quint32 epoch, quint16 position, const QList<InputEvent> &events)
{
  if (events.size() > MaxInputEvents)
    throw Error(QStringLiteral("Enhanced input queue overflow."));
  QByteArray packet(InputHeaderSize + 5 * events.size(), Qt::Uninitialized);
  packet[0] = static_cast<char>(flags);
  qToBigEndian(epoch, packet.data() + 1);
  qToBigEndian(position, packet.data() + 5);
  auto *cursor = packet.data() + InputHeaderSize;
  for (const auto &event : events) {
    cursor[0] = static_cast<char>(event.kind);
    qToBigEndian(event.first, cursor + 1);
    qToBigEndian(event.second, cursor + 3);
    cursor += 5;
  }
  return packet;
}

bool valid(const InputEvent &event)
{
  switch (event.kind) {
  case InputKind::Move:
    return qAbs(static_cast<int>(event.first)) <= 8192 && qAbs(static_cast<int>(event.second)) <= 8192;
  case InputKind::Wheel:
    return (event.first == 0) != (event.second == 0);
  case InputKind::Button:
    return event.first >= 1 && event.first <= 5 && (event.second == 0 || event.second == 1);
  case InputKind::Key:
    return event.first >= 1 && event.first <= 255 && event.second >= 0 && event.second <= 3;
  }
  return false;
}

QList<InputEvent> decodeEvents(QByteArrayView data)
{
  if (data.size() < InputHeaderSize || data.size() > MaxInputPacket || (data.size() - InputHeaderSize) % 5 != 0)
    throw Error(QStringLiteral("Invalid enhanced input packet."));

  QList<InputEvent> events;
  for (qsizetype offset = InputHeaderSize; offset < data.size(); offset += 5) {
    const auto *cursor = data.data() + offset;
    const InputEvent event{
        static_cast<InputKind>(cursor[0]), qFromBigEndian<qint16>(cursor + 1), qFromBigEndian<qint16>(cursor + 3)
    };
    if (!valid(event))
      throw Error(QStringLiteral("Invalid enhanced input event."));
    events.append(event);
  }
  return events;
}

} // namespace

QByteArray InputPoll::encode() const
{
  return enhanced::encode((request ? 1 : 0) | (release ? 2 : 0), epoch, verticalPosition, events);
}

InputPoll InputPoll::decode(QByteArrayView data)
{
  auto events = decodeEvents(data);
  const auto flags = static_cast<quint8>(data[0]);
  const auto epoch = qFromBigEndian<quint32>(data.data() + 1);
  if (flags > 2 || (flags != 0 && !events.isEmpty()) || (!events.isEmpty() && epoch == 0))
    throw Error(QStringLiteral("Invalid enhanced input request."));
  return {epoch, qFromBigEndian<quint16>(data.data() + 5), flags == 1, flags == 2, std::move(events)};
}

QByteArray InputState::encode() const
{
  return enhanced::encode(static_cast<quint8>(owner), epoch, verticalPosition, events);
}

InputState InputState::decode(QByteArrayView data)
{
  auto events = decodeEvents(data);
  const auto owner = static_cast<InputSide>(data[0]);
  const auto epoch = qFromBigEndian<quint32>(data.data() + 1);
  if (owner > InputSide::Connector || (owner == InputSide::None && !events.isEmpty()) ||
      (owner != InputSide::None && epoch == 0)) {
    throw Error(QStringLiteral("Invalid enhanced input lease."));
  }
  return {owner, epoch, qFromBigEndian<quint16>(data.data() + 5), std::move(events)};
}

void advanceLease(InputState &state, const InputPoll &listener, const InputPoll &connector)
{
  state.events.clear();
  const auto releases = [&state](const InputPoll &poll) { return poll.release && poll.epoch == state.epoch; };
  if (state.owner != InputSide::None) {
    if (releases(listener) || releases(connector)) {
      state.owner = InputSide::None;
      return;
    }
    const auto &owner = state.owner == InputSide::Listener ? listener : connector;
    if (owner.epoch == state.epoch)
      state.events = owner.events;
    return;
  }
  const bool listenerRequest = listener.request && listener.epoch == state.epoch;
  const bool connectorRequest = connector.request && connector.epoch == state.epoch;
  if (listenerRequest == connectorRequest)
    return;
  if (state.epoch == std::numeric_limits<quint32>::max())
    throw Error(QStringLiteral("Enhanced input lease sequence exhausted."));
  ++state.epoch;
  state.owner = listenerRequest ? InputSide::Listener : InputSide::Connector;
  state.verticalPosition = listenerRequest ? listener.verticalPosition : connector.verticalPosition;
}

} // namespace deskflow::enhanced
