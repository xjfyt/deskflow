/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "Protocol.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QtEndian>

namespace deskflow::enhanced {
namespace {

constexpr quint32 Magic = 0x44464558; // DFEX
constexpr quint16 Major = 1;
constexpr quint16 Minor = 0;

QString invalidFrame()
{
  return QCoreApplication::translate("Enhanced", "Invalid enhanced protocol frame.");
}

} // namespace

QByteArray encodeHeader(Message type, qsizetype size, quint64 sequence)
{
  if (size < 0 || size > MaxPayload || sequence == 0)
    throw Error(invalidFrame());

  QByteArray bytes(HeaderSize, Qt::Uninitialized);
  qToBigEndian(Magic, bytes.data());
  qToBigEndian(Major, bytes.data() + 4);
  qToBigEndian(Minor, bytes.data() + 6);
  qToBigEndian(static_cast<quint16>(type), bytes.data() + 8);
  qToBigEndian(quint16{0}, bytes.data() + 10);
  qToBigEndian(static_cast<quint32>(size), bytes.data() + 12);
  qToBigEndian(sequence, bytes.data() + 16);
  return bytes;
}

Header decodeHeader(QByteArrayView bytes, quint64 sequence)
{
  if (bytes.size() != HeaderSize || qFromBigEndian<quint32>(bytes.data()) != Magic ||
      qFromBigEndian<quint16>(bytes.data() + 4) != Major) {
    throw Error(QCoreApplication::translate("Enhanced", "Unsupported enhanced protocol version."));
  }

  const auto type = qFromBigEndian<quint16>(bytes.data() + 8);
  const auto size = qFromBigEndian<quint32>(bytes.data() + 12);
  if (qFromBigEndian<quint16>(bytes.data() + 10) != 0 || size > MaxPayload || sequence == 0 ||
      qFromBigEndian<quint64>(bytes.data() + 16) != sequence || type < static_cast<quint16>(Message::Hello) ||
      type > static_cast<quint16>(Message::Committed)) {
    throw Error(invalidFrame());
  }

  return {static_cast<Message>(type), size};
}

QByteArray fileCapability()
{
  return QByteArray(1, '\x01');
}

QByteArray parseFingerprint(QString text)
{
  static const QRegularExpression hex(QStringLiteral("\\A[0-9a-fA-F]{64}\\z"));
  text.remove(u':');
  text = text.trimmed();
  return hex.match(text).hasMatch() ? QByteArray::fromHex(text.toLatin1()) : QByteArray{};
}

} // namespace deskflow::enhanced
