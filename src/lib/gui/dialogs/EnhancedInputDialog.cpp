/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EnhancedInputDialog.h"

#include "common/Constants.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>

#include <windows.h>

namespace deskflow::gui {

EnhancedInputDialog::EnhancedInputDialog(QWidget *parent) : QDialog(parent)
{
  setWindowTitle(tr("Enhanced Input (Windows Preview)"));
  auto *layout = new QVBoxLayout(this);
  m_settings = new QWidget(this);
  auto *form = new QFormLayout(m_settings);
  auto *fingerprint = new QLineEdit(m_settings);
  fingerprint->setReadOnly(true);
  m_peer = new QLineEdit(m_settings);
  m_peer->setMaxLength(128);
  m_role = new QComboBox(m_settings);
  m_role->addItem(tr("Listen"), QStringLiteral("listen"));
  m_role->addItem(tr("Connect"), QStringLiteral("connect"));
  m_address = new QLineEdit(m_settings);
  m_address->setMaxLength(128);
  m_edge = new QComboBox(m_settings);
  m_edge->addItem(tr("Right"), QStringLiteral("right"));
  m_edge->addItem(tr("Left"), QStringLiteral("left"));
  m_port = new QSpinBox(m_settings);
  m_port->setRange(1, 65535);
  m_port->setValue(24803);
  m_remember = new QCheckBox(tr("Remember this verified peer"), m_settings);
  form->addRow(tr("Your fingerprint"), fingerprint);
  form->addRow(tr("Verified peer fingerprint"), m_peer);
  form->addRow(tr("Connection role"), m_role);
  form->addRow(tr("IP address"), m_address);
  form->addRow(tr("Input port"), m_port);
  form->addRow(tr("Edge facing peer"), m_edge);
  form->addRow(m_remember);
  layout->addWidget(m_settings);

  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  m_status->setTextFormat(Qt::PlainText);
  layout->addWidget(m_status);
  layout->addWidget(new QLabel(tr("Emergency stop  Ctrl+Alt+Shift+F12"), this));

  auto *buttons = new QHBoxLayout;
  buttons->addStretch();
  m_start = new QPushButton(tr("Start"), this);
  m_stop = new QPushButton(tr("Stop"), this);
  auto *close = new QPushButton(tr("Close"), this);
  buttons->addWidget(m_start);
  buttons->addWidget(m_stop);
  buttons->addWidget(close);
  layout->addLayout(buttons);

  const auto updateAddressHint = [this] {
    const auto listening = m_role->currentData() == QStringLiteral("listen");
    m_address->setPlaceholderText(listening ? tr("This computer's local IP") : tr("Listening computer's IP"));
  };
  connect(m_role, &QComboBox::currentIndexChanged, this, updateAddressHint);
  updateAddressHint();
  connect(m_start, &QPushButton::clicked, this, &EnhancedInputDialog::start);
  connect(m_stop, &QPushButton::clicked, this, &EnhancedInputDialog::stop);
  connect(close, &QPushButton::clicked, this, &EnhancedInputDialog::reject);
  connect(m_remember, &QCheckBox::toggled, this, [this](bool checked) {
    if (!checked)
      forgetPeer();
  });

  m_process.setProcessChannelMode(QProcess::MergedChannels);
  m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
    arguments->flags |= CREATE_NO_WINDOW;
  });
  connect(&m_process, &QProcess::readyReadStandardOutput, this, &EnhancedInputDialog::readOutput);
  connect(&m_process, &QProcess::stateChanged, this, [this] {
    refresh();
    Q_EMIT activeChanged();
  });
  connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      m_status->setText(tr("Could not start the enhanced input process. %1").arg(m_process.errorString()));
      m_stopping = false;
      refresh();
      if (m_closeAfterStop)
        QDialog::reject();
    }
  });
  connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
    readOutput();
    if (!m_output.isEmpty())
      setChildState(m_output.trimmed());
    if (status == QProcess::NormalExit && code == 0) {
      m_status->setText(
          m_desktopInterrupted ? tr("Windows switched desktops. Start enhanced input again when ready.")
                               : tr("Enhanced input stopped.")
      );
    } else if (!m_lastDiagnostic.isEmpty()) {
      m_status->setText(tr("Enhanced input ended. %1").arg(m_lastDiagnostic));
    } else {
      m_status->setText(tr("Enhanced input ended unexpectedly."));
    }
    m_stopping = false;
    refresh();
    if (m_closeAfterStop)
      QDialog::reject();
  });

  try {
    const auto profile = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (profile.isEmpty())
      throw enhanced::Error(tr("Could not find the local Deskflow data folder."));
    const QDir directory(QDir(profile).filePath(QStringLiteral("enhanced")));
    m_identity = enhanced::loadIdentity(directory.filePath(QStringLiteral("identity.pem")), true);
    m_profilePath = directory.filePath(QStringLiteral("input-peer.ini"));
    fingerprint->setText(QString::fromLatin1(m_identity.fingerprint.toHex(':').toUpper()));
    fingerprint->setCursorPosition(0);
    loadPeer();
  } catch (const std::exception &error) {
    m_status->setText(QString::fromUtf8(error.what()));
  }
  refresh();
}

EnhancedInputDialog::~EnhancedInputDialog()
{
  if (isActive()) {
    disconnect(&m_process, nullptr, this, nullptr);
    stop();
    m_process.waitForFinished(3000);
  }
}

bool EnhancedInputDialog::isActive() const
{
  return m_process.state() != QProcess::NotRunning;
}

void EnhancedInputDialog::setClassicStopped(bool stopped)
{
  m_classicStopped = stopped;
  if (!m_identity.fingerprint.isEmpty() && !isActive())
    m_status->setText(
        stopped ? tr("Verify both fingerprints and start the listening computer first.")
                : tr("Stop the classic Deskflow core before starting enhanced input.")
    );
  refresh();
}

void EnhancedInputDialog::start()
{
  if (!m_classicStopped || isActive() || m_identity.fingerprint.isEmpty())
    return;
  const auto fingerprint = enhanced::parseFingerprint(m_peer->text());
  const QHostAddress address(m_address->text().trimmed());
  if (fingerprint.isEmpty() || fingerprint == m_identity.fingerprint || address.isNull() || address.isMulticast() ||
      address == QHostAddress::Any || address == QHostAddress::AnyIPv4 || address == QHostAddress::AnyIPv6 ||
      address == QHostAddress::Broadcast) {
    m_status->setText(tr("Enter the other computer's verified fingerprint and a specific IP address."));
    return;
  }

  m_stopping = false;
  m_desktopInterrupted = false;
  m_closeAfterStop = false;
  m_output.clear();
  m_lastDiagnostic.clear();
  m_status->setText(tr("Starting enhanced input…"));
  m_process.start(
      QDir(QCoreApplication::applicationDirPath()).filePath(QString::fromLatin1(kCoreBinName)),
      {QStringLiteral("--enhanced-control"), m_role->currentData().toString(), QStringLiteral("--gui-session"),
       QStringLiteral("--certificate"), m_identity.pemPath, QStringLiteral("--peer"),
       QString::fromLatin1(fingerprint.toHex()), QStringLiteral("--address"), address.toString(),
       QStringLiteral("--port"), QString::number(m_port->value()), QStringLiteral("--edge"),
       m_edge->currentData().toString()}
  );
}

void EnhancedInputDialog::stop()
{
  if (!isActive() || m_stopping)
    return;
  m_stopping = true;
  m_status->setText(tr("Stopping enhanced input…"));
  m_process.write("Q", 1);
  m_process.closeWriteChannel();
  refresh();
  QTimer::singleShot(2000, this, [this, pid = m_process.processId()] {
    if (isActive() && m_stopping && m_process.processId() == pid)
      m_status->setText(tr("Waiting for input release. Press Ctrl+Alt+Shift+F12 on either computer to end its session.")
      );
  });
}

void EnhancedInputDialog::reject()
{
  if (isActive()) {
    m_closeAfterStop = true;
    stop();
  } else {
    QDialog::reject();
  }
}

void EnhancedInputDialog::refresh()
{
  m_settings->setEnabled(!isActive());
  m_start->setEnabled(m_classicStopped && !isActive() && !m_identity.fingerprint.isEmpty());
  m_stop->setEnabled(isActive() && !m_stopping);
}

void EnhancedInputDialog::readOutput()
{
  m_output += m_process.readAllStandardOutput();
  while (true) {
    const auto end = m_output.indexOf('\n');
    if (end < 0)
      break;
    setChildState(m_output.first(end).trimmed());
    m_output.remove(0, end + 1);
  }
  if (m_output.size() > 4096)
    m_output.clear();
}

void EnhancedInputDialog::setChildState(const QByteArray &line)
{
  if (!line.startsWith("DFEX_STATE ")) {
    if (!line.isEmpty())
      m_lastDiagnostic = QString::fromUtf8(line.first(512));
    return;
  }
  const auto state = line.sliced(11);
  if (state == "desktop") {
    m_desktopInterrupted = true;
    m_status->setText(tr("Windows switched desktops. Complete the prompt using the local mouse or keyboard."));
  } else if (m_stopping) {
    return;
  } else if (state == "waiting")
    m_status->setText(tr("Waiting for a verified peer."));
  else if (state == "paired" || state == "local")
    m_status->setText(tr("Paired. Input is local. Move through the selected edge to start."));
  else if (state == "sending")
    m_status->setText(tr("This computer is controlling the peer."));
  else if (state == "receiving")
    m_status->setText(tr("The peer is controlling this computer. Use local input to take over."));
  if (state == "paired" && m_remember->isChecked() && !savePeer())
    m_status->setText(tr("Paired. Could not save the verified peer."));
}

void EnhancedInputDialog::loadPeer()
{
  const QSettings settings(m_profilePath, QSettings::IniFormat);
  if (settings.value(QStringLiteral("identity")).toByteArray() != m_identity.fingerprint)
    return;
  const auto pin = settings.value(QStringLiteral("fingerprint")).toByteArray();
  if (pin.size() != enhanced::TokenSize || pin == m_identity.fingerprint)
    return;

  m_peer->setText(QString::fromLatin1(pin.toHex(':').toUpper()));
  m_address->setText(settings.value(QStringLiteral("address")).toString());
  m_port->setValue(settings.value(QStringLiteral("port"), 24803).toInt());
  m_role->setCurrentIndex(qMax(0, m_role->findData(settings.value(QStringLiteral("role")))));
  m_edge->setCurrentIndex(qMax(0, m_edge->findData(settings.value(QStringLiteral("edge")))));
  m_remember->setChecked(true);
}

void EnhancedInputDialog::forgetPeer()
{
  if (m_profilePath.isEmpty())
    return;
  QSettings settings(m_profilePath, QSettings::IniFormat);
  if (!settings.contains(QStringLiteral("fingerprint")))
    return;
  settings.clear();
  settings.sync();
  if (settings.status() == QSettings::NoError) {
    m_peer->clear();
    m_address->clear();
    m_status->setText(tr("Forgot the saved peer."));
  } else {
    m_remember->setChecked(true);
    m_status->setText(tr("Could not forget the saved peer."));
  }
}

bool EnhancedInputDialog::savePeer() const
{
  QSettings settings(m_profilePath, QSettings::IniFormat);
  settings.setValue(QStringLiteral("identity"), m_identity.fingerprint);
  settings.setValue(QStringLiteral("fingerprint"), enhanced::parseFingerprint(m_peer->text()));
  settings.setValue(QStringLiteral("address"), m_address->text().trimmed());
  settings.setValue(QStringLiteral("port"), m_port->value());
  settings.setValue(QStringLiteral("role"), m_role->currentData());
  settings.setValue(QStringLiteral("edge"), m_edge->currentData());
  settings.sync();
  return settings.status() == QSettings::NoError;
}

} // namespace deskflow::gui
