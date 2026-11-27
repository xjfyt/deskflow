/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Log.h"

#include <QTest>

class FileTransferProtocolTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void readsBoundedFrame();
  void readsFrameWrittenByProtocol();
  void rejectsMalformedFrames_data();
  void rejectsMalformedFrames();
  void rejectsOversizedPacketBeforeReading();
  void doesNotConsumeNextPacketAfterEmptyFrame();
  void fileReferencesStayOutOfLegacyClipboard();

private:
  Log m_log;
};
