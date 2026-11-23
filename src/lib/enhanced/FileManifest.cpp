/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileManifest.h"

#include "Protocol.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QtEndian>

namespace deskflow::enhanced {
namespace {

QString invalidManifest()
{
  return QCoreApplication::translate("Enhanced", "Invalid or oversized file manifest.");
}

bool isPortableComponent(const QString &component)
{
  static const QRegularExpression reserved(
      QStringLiteral("^(CON|PRN|AUX|NUL|CLOCK\\$|CONIN\\$|CONOUT\\$|(?:COM|LPT)[0-9\\x{00B9}\\x{00B2}\\x{00B3}])$"),
      QRegularExpression::CaseInsensitiveOption
  );
  constexpr QStringView forbidden = u"/\\:*?\"<>|";
  if (component.isEmpty() || component.endsWith(u'.') || component.endsWith(u' ') || component.toUtf8().size() > 255 ||
      reserved.match(component.section(u'.', 0, 0).trimmed()).hasMatch()) {
    return false;
  }
  for (const auto ch : component) {
    if (ch.category() == QChar::Other_Control || forbidden.contains(ch))
      return false;
  }
  return true;
}

} // namespace

quint64 FileManifest::validate() const
{
  if (entries.isEmpty() || entries.size() > MaxEntries)
    throw Error(invalidManifest());

  quint64 total = 0;
  QHash<QString, bool> exactPaths;
  QSet<QString> foldedPaths;
  for (const auto &entry : entries) {
    const auto utf8 = entry.path.toUtf8();
    const auto parts = entry.path.split(u'/');
    if (utf8.size() > MaxPathBytes || QString::fromUtf8(utf8) != entry.path || parts.size() > MaxDepth)
      throw Error(invalidManifest());
    for (const auto &component : parts) {
      if (!isPortableComponent(component))
        throw Error(invalidManifest());
    }

    const auto folded = entry.path.toCaseFolded().normalized(QString::NormalizationForm_C);
    const auto separator = entry.path.lastIndexOf(u'/');
    if (foldedPaths.contains(folded) || (separator >= 0 && !exactPaths.value(entry.path.first(separator), false)) ||
        (entry.directory && entry.size != 0) || entry.size > MaxTransferBytes - total) {
      throw Error(invalidManifest());
    }
    total += entry.size;
    exactPaths.insert(entry.path, entry.directory);
    foldedPaths.insert(folded);
  }

  return total;
}

QByteArray FileManifest::encode() const
{
  validate();
  QByteArray data;
  QDataStream stream(&data, QIODeviceBase::WriteOnly);
  stream << static_cast<quint32>(entries.size());
  for (const auto &entry : entries) {
    const auto path = entry.path.toUtf8();
    constexpr qsizetype entryHeaderSize = 11;
    if (data.size() + entryHeaderSize + path.size() > MaxPayload)
      throw Error(invalidManifest());
    stream << static_cast<quint8>(entry.directory) << entry.size << static_cast<quint16>(path.size());
    stream.writeRawData(path.constData(), static_cast<int>(path.size()));
  }
  if (stream.status() != QDataStream::Ok)
    throw Error(invalidManifest());
  return data;
}

FileManifest FileManifest::decode(QByteArrayView data)
{
  constexpr qsizetype entryHeaderSize = 11;
  if (data.size() < 4 || data.size() > MaxPayload)
    throw Error(invalidManifest());
  const auto count = qFromBigEndian<quint32>(data.data());
  data = data.sliced(4);
  if (count == 0 || count > MaxEntries || count > data.size() / (entryHeaderSize + 1))
    throw Error(invalidManifest());

  FileManifest manifest;
  manifest.entries.reserve(count);
  for (quint32 i = 0; i < count; ++i) {
    if (data.size() < entryHeaderSize)
      throw Error(invalidManifest());
    const auto type = static_cast<quint8>(data[0]);
    const auto size = qFromBigEndian<quint64>(data.data() + 1);
    const auto pathSize = qFromBigEndian<quint16>(data.data() + 9);
    data = data.sliced(entryHeaderSize);
    if (type > 1 || pathSize == 0 || pathSize > MaxPathBytes || pathSize > data.size())
      throw Error(invalidManifest());

    QStringDecoder utf8(
        QStringDecoder::Utf8, QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom
    );
    const QString path = utf8(data.first(pathSize));
    if (utf8.hasError())
      throw Error(invalidManifest());
    manifest.entries.append({path, type == 1, size});
    data = data.sliced(pathSize);
  }
  if (!data.isEmpty())
    throw Error(invalidManifest());
  manifest.validate();
  return manifest;
}

} // namespace deskflow::enhanced
