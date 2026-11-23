/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "TlsChannel.h"

#include <QStringList>

#include <functional>

namespace deskflow::enhanced {

struct TransferOptions
{
  PeerIdentity identity;
  QByteArray peerFingerprint;
  QHostAddress address;
  quint16 controlPort = ControlPort;
  quint16 dataPort = DataPort;
};

using TransferProgress = std::function<void(quint64 done, quint64 total)>;

struct ReceivedFiles
{
  QString directory;
  QStringList selection;
};

// Both entry points block and belong on a worker thread. Callbacks run on that same thread.
void sendFiles(
    const TransferOptions &options, const QStringList &paths, const std::atomic_bool &cancelled,
    const TransferProgress &progress = {}
);
ReceivedFiles receiveFiles(
    const TransferOptions &options, const QString &directory, const std::atomic_bool &cancelled,
    const TransferProgress &progress = {}, const std::function<void()> &listening = {}
);

} // namespace deskflow::enhanced
