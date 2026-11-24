/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "enhanced/TlsChannel.h"

#include <QDialog>
#include <QProcess>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace deskflow::gui {

class EnhancedInputDialog : public QDialog
{
  Q_OBJECT

public:
  explicit EnhancedInputDialog(QWidget *parent);
  ~EnhancedInputDialog() override;

  bool isActive() const;
  void setClassicStopped(bool stopped);
  void stop();

Q_SIGNALS:
  void activeChanged();

protected:
  void reject() override;

private:
  void start();
  void refresh();
  void readOutput();
  void setChildState(const QByteArray &line);
  void loadPeer();
  void forgetPeer();
  bool savePeer() const;

  QProcess m_process;
  enhanced::PeerIdentity m_identity;
  QByteArray m_output;
  QString m_lastDiagnostic;
  QString m_profilePath;
  QWidget *m_settings = nullptr;
  QComboBox *m_role = nullptr;
  QComboBox *m_edge = nullptr;
  QLineEdit *m_address = nullptr;
  QLineEdit *m_peer = nullptr;
  QSpinBox *m_port = nullptr;
  QCheckBox *m_remember = nullptr;
  QLabel *m_status = nullptr;
  QPushButton *m_start = nullptr;
  QPushButton *m_stop = nullptr;
  bool m_classicStopped = false;
  bool m_stopping = false;
  bool m_closeAfterStop = false;
};

} // namespace deskflow::gui
