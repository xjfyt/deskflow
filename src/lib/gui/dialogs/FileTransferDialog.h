/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "enhanced/FileTransfer.h"

#include <QDialog>

#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMimeData;
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
  void dragMoveEvent(QDragMoveEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;
  void reject() override;

private:
  enum class ArmedAction
  {
    None,
    Clipboard,
    FileDrop
  };
  enum class SendMethod
  {
    Files,
    CutFiles,
    FileDrop
  };

  void addPaths(const QStringList &paths);
  void receive();
  void discardResume();
  void sendSelection();
  void armSend(ArmedAction action);
  void clipboardChanged();
  void start(const QStringList &sources, const QString &destination, SendMethod method = SendMethod::Files);
  bool acceptsFileDrop(const QDropEvent *event) const;
  void showReceived(enhanced::ReceivedFiles received);
  std::unique_ptr<QMimeData> receivedMime(const QStringList &paths);
  void copyReceived();
  void refreshActions();
  QString receiveDirectory() const;
  enhanced::TransferOptions options() const;

  enhanced::PeerIdentity m_identity;
  enhanced::ReceivedFiles m_received;
  QString m_receiveDirectory;
  QLineEdit *m_address = nullptr;
  QLineEdit *m_peer = nullptr;
  QSpinBox *m_controlPort = nullptr;
  QSpinBox *m_dataPort = nullptr;
  QWidget *m_settings = nullptr;
  QWidget *m_selectionActions = nullptr;
  QLabel *m_sourceHeading = nullptr;
  QListWidget *m_selection = nullptr;
  QWidget *m_receivedPanel = nullptr;
  QListWidget *m_receivedItems = nullptr;
  QCheckBox *m_autoPublish = nullptr;
  QCheckBox *m_allowFileDrop = nullptr;
  QCheckBox *m_resume = nullptr;
  QPushButton *m_discardResume = nullptr;
  QPushButton *m_send = nullptr;
  QPushButton *m_receive = nullptr;
  QPushButton *m_arm = nullptr;
  QPushButton *m_armDrop = nullptr;
  QPushButton *m_cancel = nullptr;
  QPushButton *m_copy = nullptr;
  QPushButton *m_openFolder = nullptr;
  QLabel *m_status = nullptr;
  QLabel *m_resumeOffset = nullptr;
  QProgressBar *m_progress = nullptr;
  QThread *m_worker = nullptr;
  QTimer *m_armTimer = nullptr;
  std::atomic_bool m_cancelled = false;
  quint64 m_clipboardRevision = 0;
  ArmedAction m_armed = ArmedAction::None;
};

} // namespace deskflow::gui
