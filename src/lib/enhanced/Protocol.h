/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QString>

#include <stdexcept>

namespace deskflow::enhanced {

inline constexpr quint16 ControlPort = 24801;
inline constexpr quint16 DataPort = 24802;
inline constexpr qsizetype HeaderSize = 24;
inline constexpr qsizetype MaxPayload = 256 * 1024;
inline constexpr qsizetype ChunkSize = 64 * 1024;
inline constexpr qsizetype TokenSize = 32;

enum class Message : quint16
{
  Hello = 1,
  Offer,
  Accept,
  DataOpen,
  DataReady,
  Chunk,
  Digest,
  FileAck,
  Finish,
  Committed,
  InputPoll,
  InputState,
  ResumeIdentity,
  ResumeOffset,
  ResumeVerified
};

struct Header
{
  Message type;
  quint32 size;
};

class Error : public std::runtime_error
{
public:
  explicit Error(const QString &message) : std::runtime_error(message.toUtf8().constData()), message(message)
  {
  }
  const QString message;
};

QByteArray encodeHeader(Message type, qsizetype size, quint64 sequence);
Header decodeHeader(QByteArrayView bytes, quint64 sequence);
QByteArray fileCapability();
QByteArray parseFingerprint(QString text);

} // namespace deskflow::enhanced
