/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "enhanced/FileTransfer.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QThread;
class QTimer;

namespace deskflow::gui {

class FileTransferDialog : public QDialog
{
  Q_OBJECT

public:
  explicit FileTransferDialog(QWidget *parent);
  ~FileTransferDialog() override;

protected:
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  void reject() override;

private:
  void addPaths(const QStringList &paths);
  void receive();
  void sendSelection();
  void armClipboard();
  void clipboardChanged();
  void start(const QStringList &sources, const QString &destination);
  void copyReceived();
  void refreshActions();
  enhanced::TransferOptions options() const;

  enhanced::PeerIdentity m_identity;
  enhanced::ReceivedFiles m_received;
  QLineEdit *m_address = nullptr;
  QLineEdit *m_peer = nullptr;
  QSpinBox *m_controlPort = nullptr;
  QSpinBox *m_dataPort = nullptr;
  QWidget *m_settings = nullptr;
  QListWidget *m_selection = nullptr;
  QCheckBox *m_autoPublish = nullptr;
  QPushButton *m_send = nullptr;
  QPushButton *m_receive = nullptr;
  QPushButton *m_arm = nullptr;
  QPushButton *m_cancel = nullptr;
  QPushButton *m_copy = nullptr;
  QPushButton *m_openFolder = nullptr;
  QLabel *m_status = nullptr;
  QProgressBar *m_progress = nullptr;
  QThread *m_worker = nullptr;
  QTimer *m_armTimer = nullptr;
  std::atomic_bool m_cancelled = false;
  quint64 m_clipboardRevision = 0;
  bool m_armed = false;
};

} // namespace deskflow::gui
