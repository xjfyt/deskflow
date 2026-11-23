/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransferDialog.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace deskflow::gui {
namespace {

constexpr auto ReceivedMime = "application/x-deskflow-received-files";

QStringList filePaths(const QMimeData *data)
{
  QStringList paths;
  if (!data)
    return paths;
  for (const auto &url : data->urls()) {
    if (!url.isLocalFile() || url.toLocalFile().isEmpty())
      return {};
    paths.append(url.toLocalFile());
  }
  return paths;
}

} // namespace

FileTransferDialog::FileTransferDialog(QWidget *parent) : QDialog(parent)
{
  setWindowTitle(tr("Enhanced File Transfer"));
  setAcceptDrops(true);

  auto *layout = new QVBoxLayout(this);
  m_settings = new QWidget(this);
  auto *settings = new QFormLayout(m_settings);
  auto *fingerprint = new QLineEdit;
  fingerprint->setReadOnly(true);
  m_address = new QLineEdit;
  m_address->setPlaceholderText(tr("Sender: receiver IP. Receiver: your local IP."));
  m_peer = new QLineEdit;
  m_peer->setMaxLength(128);
  auto *ports = new QHBoxLayout;
  m_controlPort = new QSpinBox;
  m_controlPort->setRange(1, 65535);
  m_controlPort->setValue(enhanced::ControlPort);
  m_controlPort->setAccessibleName(tr("Control port"));
  ports->addWidget(m_controlPort);
  m_dataPort = new QSpinBox;
  m_dataPort->setRange(1, 65535);
  m_dataPort->setValue(enhanced::DataPort);
  m_dataPort->setAccessibleName(tr("Data port"));
  ports->addWidget(m_dataPort);
  settings->addRow(tr("Your fingerprint"), fingerprint);
  settings->addRow(tr("Verified peer fingerprint"), m_peer);
  settings->addRow(tr("IP address"), m_address);
  settings->addRow(tr("Control and data ports"), ports);
  layout->addWidget(m_settings);

  layout->addWidget(new QLabel(tr("Files and folders to send")));
  m_selection = new QListWidget;
  m_selection->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_selection->setDragDropMode(QAbstractItemView::NoDragDrop);
  m_selection->setAcceptDrops(false);
  layout->addWidget(m_selection);

  auto *selectionActions = new QHBoxLayout;
  auto *addFiles = new QPushButton(tr("Add files…"));
  auto *addFolder = new QPushButton(tr("Add folder…"));
  auto *addClipboard = new QPushButton(tr("Add copied files"));
  auto *remove = new QPushButton(tr("Remove"));
  selectionActions->addWidget(addFiles);
  selectionActions->addWidget(addFolder);
  selectionActions->addWidget(addClipboard);
  selectionActions->addWidget(remove);
  layout->addLayout(selectionActions);

  auto *actions = new QHBoxLayout;
  m_send = new QPushButton(tr("Send selection"));
  m_arm = new QPushButton(tr("Send next file copy"));
  m_receive = new QPushButton(tr("Receive…"));
  m_cancel = new QPushButton(tr("Cancel"));
  actions->addWidget(m_send);
  actions->addWidget(m_arm);
  actions->addWidget(m_receive);
  actions->addWidget(m_cancel);
  layout->addLayout(actions);

  m_autoPublish = new QCheckBox(tr("Put received files on the clipboard if it stays unchanged"));
  layout->addWidget(m_autoPublish);
  m_progress = new QProgressBar;
  m_progress->setRange(0, 100);
  layout->addWidget(m_progress);
  m_status = new QLabel;
  m_status->setWordWrap(true);
  m_status->setTextFormat(Qt::PlainText);
  layout->addWidget(m_status);

  auto *resultActions = new QHBoxLayout;
  m_copy = new QPushButton(tr("Copy received files"));
  m_openFolder = new QPushButton(tr("Open received folder"));
  resultActions->addWidget(m_copy);
  resultActions->addWidget(m_openFolder);
  layout->addLayout(resultActions);

  connect(addFiles, &QPushButton::clicked, this, [this] {
    addPaths(QFileDialog::getOpenFileNames(this, tr("Select files to send")));
  });
  connect(addFolder, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("Select a folder to send"));
    if (!path.isEmpty())
      addPaths({path});
  });
  connect(addClipboard, &QPushButton::clicked, this, [this] {
    const auto paths = filePaths(QGuiApplication::clipboard()->mimeData());
    if (paths.isEmpty())
      m_status->setText(tr("The clipboard has no local file selection."));
    else
      addPaths(paths);
  });
  connect(remove, &QPushButton::clicked, this, [this] {
    qDeleteAll(m_selection->selectedItems());
    refreshActions();
  });
  connect(m_send, &QPushButton::clicked, this, &FileTransferDialog::sendSelection);
  connect(m_arm, &QPushButton::clicked, this, &FileTransferDialog::armClipboard);
  connect(m_receive, &QPushButton::clicked, this, &FileTransferDialog::receive);
  connect(m_cancel, &QPushButton::clicked, this, &FileTransferDialog::reject);
  connect(m_copy, &QPushButton::clicked, this, &FileTransferDialog::copyReceived);
  connect(m_openFolder, &QPushButton::clicked, this, [this] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_received.directory));
  });
  connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &FileTransferDialog::clipboardChanged);
  m_armTimer = new QTimer(this);
  m_armTimer->setSingleShot(true);
  connect(m_armTimer, &QTimer::timeout, this, [this] {
    m_armed = false;
    m_status->setText(tr("Clipboard capture expired."));
    refreshActions();
  });

  try {
    const QDir profile(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
    m_identity = enhanced::loadIdentity(profile.filePath(QStringLiteral("enhanced/identity.pem")), true);
    fingerprint->setText(QString::fromLatin1(m_identity.fingerprint.toHex(':').toUpper()));
    fingerprint->setCursorPosition(0);
    m_status->setText(tr("Verify the two fingerprints on both computers. Start Receive before sending."));
  } catch (const std::exception &error) {
    m_status->setText(QString::fromUtf8(error.what()));
  }
  refreshActions();
}

FileTransferDialog::~FileTransferDialog()
{
  if (m_worker) {
    m_cancelled.store(true, std::memory_order_relaxed);
    m_worker->wait();
  }
}

void FileTransferDialog::dragEnterEvent(QDragEnterEvent *event)
{
  if (!m_worker && !m_armed && event->possibleActions().testFlag(Qt::CopyAction) &&
      !filePaths(event->mimeData()).isEmpty()) {
    event->setDropAction(Qt::CopyAction);
    event->accept();
  }
}

void FileTransferDialog::dropEvent(QDropEvent *event)
{
  if (m_worker || m_armed || !event->possibleActions().testFlag(Qt::CopyAction))
    return;
  const auto paths = filePaths(event->mimeData());
  if (!paths.isEmpty()) {
    addPaths(paths);
    event->setDropAction(Qt::CopyAction);
    event->accept();
  }
}

void FileTransferDialog::reject()
{
  if (m_worker) {
    m_cancelled.store(true, std::memory_order_relaxed);
    m_status->setText(tr("Cancelling…"));
  } else if (m_armed) {
    m_armTimer->stop();
    m_armed = false;
    m_status->setText(tr("Clipboard capture cancelled."));
    refreshActions();
  } else {
    QDialog::reject();
  }
}

void FileTransferDialog::addPaths(const QStringList &paths)
{
  if (m_worker || m_armed)
    return;
  for (const auto &path : paths) {
    if (m_selection->findItems(path, Qt::MatchExactly).isEmpty())
      m_selection->addItem(path);
  }
  refreshActions();
}

void FileTransferDialog::receive()
{
  const auto directory = QFileDialog::getExistingDirectory(
      this, tr("Choose where to save received files"),
      QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
  );
  if (!directory.isEmpty())
    start({}, directory);
}

void FileTransferDialog::sendSelection()
{
  QStringList paths;
  for (int i = 0; i < m_selection->count(); ++i)
    paths.append(m_selection->item(i)->text());
  start(paths, {});
}

void FileTransferDialog::armClipboard()
{
  try {
    options();
  } catch (const enhanced::Error &error) {
    m_status->setText(error.message);
    return;
  }
  m_armed = true;
  m_status->setText(tr("Copy a file or folder in your file manager within 90 seconds."));
  refreshActions();
  m_armTimer->start(90000);
}

void FileTransferDialog::clipboardChanged()
{
  ++m_clipboardRevision;
  if (!m_armed)
    return;
  const auto *data = QGuiApplication::clipboard()->mimeData();
  if (!data || data->hasFormat(QString::fromLatin1(ReceivedMime)))
    return;
  const auto paths = filePaths(data);
  if (paths.isEmpty())
    return;

  m_armTimer->stop();
  m_armed = false;
  start(paths, {});
}

enhanced::TransferOptions FileTransferDialog::options() const
{
  const auto pin = enhanced::parseFingerprint(m_peer->text());
  const QHostAddress ip(m_address->text().trimmed());
  if (pin.isEmpty() || pin == m_identity.fingerprint || ip.isNull() || m_controlPort->value() == m_dataPort->value())
    throw enhanced::Error(tr("Enter a verified peer fingerprint, an IP address and two distinct ports."));
  return {m_identity, pin, ip, static_cast<quint16>(m_controlPort->value()), static_cast<quint16>(m_dataPort->value())};
}

void FileTransferDialog::start(const QStringList &sources, const QString &destination)
{
  if (m_worker)
    return;
  enhanced::TransferOptions transfer;
  try {
    transfer = options();
  } catch (const enhanced::Error &error) {
    m_status->setText(error.message);
    refreshActions();
    return;
  }

  const auto clipboardRevision = m_clipboardRevision;
  const bool autoPublish = m_autoPublish->isChecked();
  m_cancelled.store(false, std::memory_order_relaxed);
  m_progress->setRange(0, 0);
  m_status->setText(destination.isEmpty() ? tr("Preparing file selection…") : tr("Opening the receive port…"));

  m_worker = QThread::create([this, transfer, sources, destination, clipboardRevision, autoPublish] {
    QElapsedTimer timer;
    timer.start();
    const auto progress = [&](quint64 done, quint64 total) {
      if (done != 0 && done != total && timer.elapsed() < 100)
        return;
      timer.restart();
      const int percent = total == 0 ? 0 : static_cast<int>(100 * done / total);
      QMetaObject::invokeMethod(
          this,
          [this, percent] {
            m_progress->setRange(0, 100);
            m_progress->setValue(percent);
            m_status->setText(tr("Transferring and verifying files…"));
          },
          Qt::QueuedConnection
      );
    };

    try {
      enhanced::ReceivedFiles received;
      if (destination.isEmpty()) {
        enhanced::sendFiles(transfer, sources, m_cancelled, progress);
      } else {
        received = enhanced::receiveFiles(transfer, destination, m_cancelled, progress, [this] {
          QMetaObject::invokeMethod(
              this, [this] { m_status->setText(tr("Waiting for the verified peer to send files…")); },
              Qt::QueuedConnection
          );
        });
      }
      QMetaObject::invokeMethod(
          this,
          [this, received, clipboardRevision, autoPublish] {
            m_progress->setRange(0, 100);
            m_progress->setValue(100);
            if (received.directory.isEmpty()) {
              m_status->setText(tr("The receiver confirmed the transfer."));
            } else {
              m_received = received;
              if (autoPublish && m_clipboardRevision == clipboardRevision)
                copyReceived();
              else
                m_status->setText(tr("Verified files are ready in the received folder."));
            }
          },
          Qt::QueuedConnection
      );
    } catch (const std::exception &error) {
      QMetaObject::invokeMethod(
          this,
          [this, message = QString::fromUtf8(error.what())] {
            m_progress->setRange(0, 100);
            m_progress->setValue(0);
            m_status->setText(message);
          },
          Qt::QueuedConnection
      );
    }
  });
  m_worker->setParent(this);
  connect(m_worker, &QThread::finished, this, [this] {
    m_worker->deleteLater();
    m_worker = nullptr;
    refreshActions();
  });
  refreshActions();
  m_worker->start();
}

void FileTransferDialog::copyReceived()
{
  QList<QUrl> urls;
  QByteArray gnomeSelection("copy");
  for (const auto &path : m_received.selection) {
    const QFileInfo file(path);
    if (!file.exists() || file.isSymLink()) {
      m_status->setText(tr("A received file or folder is no longer available."));
      return;
    }
    const auto url = QUrl::fromLocalFile(path);
    urls.append(url);
    gnomeSelection += '\n' + url.toEncoded();
  }
  if (urls.isEmpty())
    return;
  auto *data = new QMimeData;
  data->setUrls(urls);
  data->setData(QString::fromLatin1(ReceivedMime), "1");
#ifdef Q_OS_WIN
  data->setData(
      QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\""), QByteArray("\x01\x00\x00\x00", 4)
  );
#endif
  data->setData(QStringLiteral("x-special/gnome-copied-files"), gnomeSelection);
  QGuiApplication::clipboard()->setMimeData(data);
  m_status->setText(tr("Files are on the clipboard. Paste in your file manager."));
}

void FileTransferDialog::refreshActions()
{
  const bool available = !m_worker && !m_armed;
  const bool configured = available && !m_identity.fingerprint.isEmpty();
  m_settings->setEnabled(available);
  m_selection->setEnabled(available);
  m_send->setEnabled(configured && m_selection->count() > 0);
  m_receive->setEnabled(configured);
  m_arm->setEnabled(configured);
  m_cancel->setEnabled(!available);
  m_copy->setEnabled(available && !m_received.selection.isEmpty());
  m_openFolder->setEnabled(available && !m_received.directory.isEmpty());
  m_autoPublish->setEnabled(available);
}

} // namespace deskflow::gui
