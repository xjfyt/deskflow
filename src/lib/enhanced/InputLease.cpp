/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "InputLease.h"

#include "InputRouter.h"
#include "Protocol.h"

#include <QtEndian>

#include <algorithm>
#include <array>
#include <limits>

namespace deskflow::enhanced {
namespace {

constexpr quint8 Request = 1;
constexpr quint8 Release = 2;
constexpr quint8 Ready = 4;
constexpr quint8 Active = 0x80;
constexpr int MaxMovement = 8192;

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
    return qAbs(static_cast<int>(event.first)) <= MaxMovement && qAbs(static_cast<int>(event.second)) <= MaxMovement;
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
  return enhanced::encode(
      (request ? Request : 0) | (release ? Release : 0) | (ready ? Ready : 0), epoch, verticalPosition, events
  );
}

InputPoll InputPoll::decode(QByteArrayView data)
{
  auto events = decodeEvents(data);
  const auto flags = static_cast<quint8>(data[0]);
  const auto epoch = qFromBigEndian<quint32>(data.data() + 1);
  if ((flags != 0 && flags != Request && flags != Release && flags != Ready) || (flags != 0 && !events.isEmpty()) ||
      (epoch == 0 && (flags == Ready || flags == Release || !events.isEmpty())))
    throw Error(QStringLiteral("Invalid enhanced input request."));
  return {
      epoch,         qFromBigEndian<quint16>(data.data() + 5), flags == Request, flags == Release, std::move(events),
      flags == Ready
  };
}

QByteArray InputState::encode() const
{
  return enhanced::encode(static_cast<quint8>(owner) | (active ? Active : 0), epoch, verticalPosition, events);
}

InputState InputState::decode(QByteArrayView data)
{
  auto events = decodeEvents(data);
  const auto flags = static_cast<quint8>(data[0]);
  const auto owner = static_cast<InputSide>(flags & ~Active);
  const bool active = (flags & Active) != 0;
  const auto epoch = qFromBigEndian<quint32>(data.data() + 1);
  if (owner > InputSide::Connector || (owner == InputSide::None && active) || (!active && !events.isEmpty()) ||
      (owner != InputSide::None && epoch == 0)) {
    throw Error(QStringLiteral("Invalid enhanced input lease."));
  }
  return {owner, epoch, qFromBigEndian<quint16>(data.data() + 5), std::move(events), active};
}

void advanceLease(InputState &state, const InputPoll &listener, const InputPoll &connector)
{
  constexpr InputPeerId listenerId = 1;
  constexpr InputPeerId connectorId = 2;
  constexpr std::array routes{InputRoute{listenerId, connectorId}, InputRoute{connectorId, listenerId}};
  const std::array polls{
      RoutedInputPoll{listenerId, connectorId, listener}, RoutedInputPoll{connectorId, listenerId, connector}
  };
  RoutedInputLease lease{{}, state.epoch, state.verticalPosition, {}, state.active};
  if (state.owner == InputSide::Listener)
    lease.route = routes.front();
  else if (state.owner == InputSide::Connector)
    lease.route = routes.back();

  advanceInputRoute(lease, routes, polls);
  state.owner = InputSide::None;
  if (lease.route.origin == listenerId)
    state.owner = InputSide::Listener;
  else if (lease.route.origin == connectorId)
    state.owner = InputSide::Connector;
  state.epoch = lease.epoch;
  state.verticalPosition = lease.verticalPosition;
  state.events = std::move(lease.events);
  state.active = lease.active;
}

void validateLeaseTransition(const InputState &previous, const InputState &next, const InputPoll &sent)
{
  bool valid = false;
  if (next.epoch != previous.epoch) {
    const bool localRequest =
        sent.request && sent.epoch == previous.epoch && sent.verticalPosition == next.verticalPosition;
    valid = previous.owner == InputSide::None && previous.epoch < std::numeric_limits<quint32>::max() &&
            next.epoch == previous.epoch + 1 && !next.active && next.events.isEmpty() &&
            (next.owner == InputSide::Listener || (next.owner == InputSide::Connector && localRequest));
  } else if (next.owner == InputSide::None) {
    valid = !next.active && next.events.isEmpty();
  } else if (next.owner == previous.owner && next.verticalPosition == previous.verticalPosition) {
    if (previous.active) {
      valid = next.active && (next.owner == InputSide::Listener || next.events.isEmpty());
    } else {
      valid = next.events.isEmpty() &&
              (!next.active || next.owner == InputSide::Connector || (sent.ready && sent.epoch == next.epoch));
    }
  }
  if (!valid)
    throw Error(QStringLiteral("Invalid enhanced input lease transition."));
}

bool queueInputEvent(QList<InputEvent> &queue, InputEvent event)
{
  if (event.kind == InputKind::Move) {
    if (event.first == 0 && event.second == 0)
      return true;
    if (!queue.isEmpty() && queue.last().kind == InputKind::Move) {
      auto &last = queue.last();
      last.first = static_cast<qint16>(std::clamp(last.first + event.first, -MaxMovement, MaxMovement));
      last.second = static_cast<qint16>(std::clamp(last.second + event.second, -MaxMovement, MaxMovement));
      return true;
    }
  }
  if (queue.size() >= MaxInputEvents)
    return false;
  queue.append(event);
  return true;
}

} // namespace deskflow::enhanced
