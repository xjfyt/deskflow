/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QList>
#include <QStringList>

namespace deskflow::enhanced {

inline constexpr qsizetype MaxEntries = 4096;
inline constexpr qsizetype MaxPathBytes = 4096;
inline constexpr qsizetype MaxDepth = 48;
inline constexpr quint64 MaxTransferBytes = 20ULL * 1024 * 1024 * 1024;

struct FileEntry
{
  QString path;
  bool directory;
  quint64 size;
};

struct FileManifest
{
  QList<FileEntry> entries;

  quint64 validate() const;
  QByteArray encode() const;
  static FileManifest decode(QByteArrayView data);
};

} // namespace deskflow::enhanced
