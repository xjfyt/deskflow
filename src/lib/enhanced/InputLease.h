/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QList>

namespace deskflow::enhanced {

enum class InputSide : quint8
{
  None,
  Listener,
  Connector
};

enum class InputKind : quint8
{
  Move = 1,
  Wheel,
  Button,
  Key
};

struct InputEvent
{
  InputKind kind;
  qint16 first;
  qint16 second;
};

inline constexpr qsizetype MaxInputEvents = 64;
inline constexpr qsizetype InputHeaderSize = 7;
inline constexpr qsizetype MaxInputPacket = InputHeaderSize + 5 * MaxInputEvents;

struct InputPoll
{
  quint32 epoch = 0;
  quint16 verticalPosition = 0;
  bool request = false;
  bool release = false;
  QList<InputEvent> events;
  bool ready = false;

  QByteArray encode() const;
  static InputPoll decode(QByteArrayView data);
};

struct InputState
{
  InputSide owner = InputSide::None;
  quint32 epoch = 0;
  quint16 verticalPosition = 0;
  QList<InputEvent> events;
  bool active = false;

  QByteArray encode() const;
  static InputState decode(QByteArrayView data);
};

void advanceLease(InputState &state, const InputPoll &listener, const InputPoll &connector);
void validateLeaseTransition(const InputState &previous, const InputState &next, const InputPoll &sent);
bool queueInputEvent(QList<InputEvent> &queue, InputEvent event);

} // namespace deskflow::enhanced
