/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransferDialog.h"

#include "enhanced/FileManifest.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
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

#include <functional>
#include <utility>

namespace deskflow::gui {
namespace {

constexpr QLatin1StringView ReceivedMime("application/x-deskflow-received-files");
constexpr QLatin1StringView GnomeCopiedFiles("x-special/gnome-copied-files");
constexpr QLatin1StringView KdeCutSelection("application/x-kde-cutselection");
#ifdef Q_OS_WIN
constexpr QLatin1StringView PreferredDropEffect("application/x-qt-windows-mime;value=\"Preferred DropEffect\"");
#endif

QStringList filePaths(const QList<QUrl> &urls)
{
  if (urls.size() > enhanced::MaxEntries)
    return {};
  QStringList paths;
  paths.reserve(urls.size());
  for (auto url : urls) {
    if (url.isRelative() && url.hasFragment() && url.path().isEmpty() && !url.hasQuery())
      continue; // text/uri-list can include comment lines starting with '#'.
    if (!url.isValid() || !url.isLocalFile() || url.hasQuery() || url.hasFragment() || !url.userInfo().isEmpty() ||
        url.port() != -1) {
      return {};
    }
    if (!url.host().isEmpty()) {
      if (url.host() != QStringLiteral("localhost"))
        return {};
      url.setHost({});
    }
    const auto path = QDir::fromNativeSeparators(url.toLocalFile());
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null) || path.startsWith(QStringLiteral("//")))
      return {};
    paths.append(path);
  }
  return paths;
}

struct ClipboardSelection
{
  QStringList paths;
  bool cut = false;
};

ClipboardSelection gnomeSelection(QByteArrayView payload)
{
  if (payload.size() > enhanced::MaxEntries * enhanced::MaxPathBytes)
    return {};
  const auto headerEnd = payload.indexOf('\n');
  if (headerEnd < 0)
    return {};
  const auto operation = payload.first(headerEnd).trimmed();
  if (operation != "copy" && operation != "cut")
    return {};

  QList<QUrl> urls;
  payload = payload.sliced(headerEnd + 1);
  while (!payload.isEmpty()) {
    const auto end = payload.indexOf('\n');
    auto line = end == -1 ? payload : payload.first(end);
    if (line.endsWith('\r'))
      line.chop(1);
    if (line.isEmpty() || line.startsWith('#') || line.contains('\0') || urls.size() >= enhanced::MaxEntries)
      return {};
    urls.append(QUrl::fromEncoded(line, QUrl::StrictMode));
    if (end == -1)
      break;
    payload = payload.sliced(end + 1);
  }
  return {filePaths(urls), operation == "cut"};
}

ClipboardSelection clipboardSelection(const QMimeData *data)
{
  if (!data)
    return {};
  if (data->hasFormat(GnomeCopiedFiles)) {
    const auto selection = gnomeSelection(data->data(GnomeCopiedFiles));
    if (!selection.paths.isEmpty())
      return selection;
  }

  auto paths = filePaths(data->urls());
  if (paths.isEmpty())
    return {};
  bool cut = data->data(KdeCutSelection).startsWith('1');
#ifdef Q_OS_WIN
  const auto dropEffect = data->data(PreferredDropEffect);
  if (dropEffect.size() == 4)
    cut = cut || (dropEffect.at(0) & 2) != 0; // DROPEFFECT_MOVE in the little-endian DWORD.
#endif
  return {std::move(paths), cut};
}

class LocalFileMimeData final : public QMimeData
{
protected:
  QVariant retrieveData(const QString &mime, QMetaType type) const override
  {
    // Qt otherwise synthesizes plain text from the URLs, exposing local receiving paths to text-only consumers.
    if (mime == QStringLiteral("text/plain"))
      return QStringLiteral("");
    return QMimeData::retrieveData(mime, type);
  }
};

bool isReceivedDirectory(const QFileInfo &directory)
{
  return directory.isDir() && !directory.isSymLink() && !directory.isJunction() &&
         directory.canonicalFilePath() == directory.absoluteFilePath();
}

class ReceivedFilesList final : public QListWidget
{
public:
  explicit ReceivedFilesList(std::function<std::unique_ptr<QMimeData>(const QStringList &)> createPayload)
      : m_createPayload(std::move(createPayload))
  {
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragDropMode(QAbstractItemView::DragOnly);
    setDragEnabled(true);
  }

protected:
  void startDrag(Qt::DropActions) override
  {
    QStringList paths;
    for (const auto *item : selectedItems())
      paths.append(item->data(Qt::UserRole).toString());
    if (paths.isEmpty())
      return;
    auto payload = m_createPayload(paths);
    if (!payload)
      return;
    auto *drag = new QDrag(this);
    drag->setMimeData(payload.release());
    drag->exec(Qt::CopyAction, Qt::CopyAction);
  }

private:
  std::function<std::unique_ptr<QMimeData>(const QStringList &)> m_createPayload;
};

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
  m_address->setAcceptDrops(false);
  m_address->setPlaceholderText(tr("Sender: receiver IP. Receiver: your local IP."));
  m_peer = new QLineEdit;
  m_peer->setAcceptDrops(false);
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
  m_selection->setDragDropMode(QAbstractItemView::DropOnly);
  m_selection->viewport()->installEventFilter(this);
  layout->addWidget(m_selection);

  m_selectionActions = new QWidget;
  auto *selectionActions = new QHBoxLayout(m_selectionActions);
  selectionActions->setContentsMargins(0, 0, 0, 0);
  auto *addFiles = new QPushButton(tr("Add files…"));
  auto *addFolder = new QPushButton(tr("Add folder…"));
  auto *addClipboard = new QPushButton(tr("Add copied files"));
  auto *remove = new QPushButton(tr("Remove"));
  selectionActions->addWidget(addFiles);
  selectionActions->addWidget(addFolder);
  selectionActions->addWidget(addClipboard);
  selectionActions->addWidget(remove);
  layout->addWidget(m_selectionActions);

  auto *actions = new QHBoxLayout;
  m_send = new QPushButton(tr("Send listed files"));
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

  m_receivedPanel = new QWidget;
  auto *receivedLayout = new QVBoxLayout(m_receivedPanel);
  receivedLayout->setContentsMargins(0, 0, 0, 0);
  const auto receivedName = tr("Received files");
  auto *receivedLabel = new QLabel(receivedName);
  receivedLayout->addWidget(receivedLabel);
  auto *dragHint = new QLabel(tr("Drag received files to a folder in this computer's file manager."));
  dragHint->setWordWrap(true);
  receivedLayout->addWidget(dragHint);
  m_receivedItems = new ReceivedFilesList([this](const QStringList &paths) { return receivedMime(paths); });
  m_receivedItems->setAccessibleName(receivedName);
  m_receivedItems->setMaximumHeight(128);
  receivedLayout->addWidget(m_receivedItems);
  auto *resultActions = new QHBoxLayout;
  m_copy = new QPushButton(tr("Copy received files"));
  m_openFolder = new QPushButton(tr("Open received folder"));
  resultActions->addWidget(m_copy);
  resultActions->addWidget(m_openFolder);
  receivedLayout->addLayout(resultActions);
  layout->addWidget(m_receivedPanel);
  m_receivedPanel->hide();

  connect(addFiles, &QPushButton::clicked, this, [this] {
    addPaths(QFileDialog::getOpenFileNames(this, tr("Select files to send")));
  });
  connect(addFolder, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("Select a folder to send"));
    if (!path.isEmpty())
      addPaths({path});
  });
  connect(addClipboard, &QPushButton::clicked, this, [this] {
    const auto selection = clipboardSelection(QGuiApplication::clipboard()->mimeData());
    if (selection.paths.isEmpty()) {
      m_status->setText(tr("The clipboard has no local file selection."));
    } else {
      addPaths(selection.paths);
      if (selection.cut)
        m_status->setText(tr("Cut files were added as copies."));
    }
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
    if (!isReceivedDirectory(QFileInfo(m_received.directory)) ||
        !QDesktopServices::openUrl(QUrl::fromLocalFile(m_received.directory))) {
      m_status->setText(tr("Could not open the received folder."));
    }
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
  if (acceptsFileDrop(event)) {
    event->setDropAction(Qt::CopyAction);
    event->accept();
  }
}

void FileTransferDialog::dragMoveEvent(QDragMoveEvent *event)
{
  if (acceptsFileDrop(event)) {
    event->setDropAction(Qt::CopyAction);
    event->accept();
  }
}

void FileTransferDialog::dropEvent(QDropEvent *event)
{
  if (!acceptsFileDrop(event))
    return;
  const auto paths = filePaths(event->mimeData()->urls());
  if (!paths.isEmpty()) {
    addPaths(paths);
    event->setDropAction(Qt::CopyAction);
    event->accept();
  }
}

bool FileTransferDialog::acceptsFileDrop(const QDropEvent *event) const
{
  return !m_worker && !m_armed && event->source() != m_receivedItems &&
         event->possibleActions().testFlag(Qt::CopyAction) && event->mimeData()->hasUrls();
}

bool FileTransferDialog::eventFilter(QObject *watched, QEvent *event)
{
  if (m_selection && watched == m_selection->viewport()) {
    switch (event->type()) {
    case QEvent::DragEnter:
      dragEnterEvent(static_cast<QDragEnterEvent *>(event));
      return true;
    case QEvent::DragMove:
      dragMoveEvent(static_cast<QDragMoveEvent *>(event));
      return true;
    case QEvent::Drop:
      dropEvent(static_cast<QDropEvent *>(event));
      return true;
    default:
      break;
    }
  }
  return QDialog::eventFilter(watched, event);
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
  if (!data || data->hasFormat(ReceivedMime))
    return;
  const auto selection = clipboardSelection(data);
  if (selection.paths.isEmpty())
    return;

  m_armTimer->stop();
  m_armed = false;
  start(selection.paths, {}, selection.cut);
}

enhanced::TransferOptions FileTransferDialog::options() const
{
  const auto pin = enhanced::parseFingerprint(m_peer->text());
  const QHostAddress ip(m_address->text().trimmed());
  if (pin.isEmpty() || pin == m_identity.fingerprint || ip.isNull() || m_controlPort->value() == m_dataPort->value())
    throw enhanced::Error(tr("Enter a verified peer fingerprint, an IP address and two distinct ports."));
  return {m_identity, pin, ip, static_cast<quint16>(m_controlPort->value()), static_cast<quint16>(m_dataPort->value())};
}

void FileTransferDialog::start(const QStringList &sources, const QString &destination, bool copyCutFiles)
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
  const auto transferStatus =
      copyCutFiles ? tr("Sending cut files as copies…") : tr("Transferring and verifying files…");
  m_cancelled.store(false, std::memory_order_relaxed);
  m_progress->setRange(0, 0);
  if (destination.isEmpty())
    m_status->setText(copyCutFiles ? transferStatus : tr("Preparing file selection…"));
  else
    m_status->setText(tr("Opening the receive port…"));

  m_worker = QThread::create([this, transfer, sources, destination, clipboardRevision, autoPublish, transferStatus] {
    QElapsedTimer timer;
    timer.start();
    const auto progress = [&](quint64 done, quint64 total) {
      if (done != 0 && done != total && timer.elapsed() < 100)
        return;
      timer.restart();
      const int percent = total == 0 ? 0 : static_cast<int>(100 * done / total);
      QMetaObject::invokeMethod(
          this,
          [this, percent, transferStatus] {
            m_progress->setRange(0, 100);
            m_progress->setValue(percent);
            m_status->setText(transferStatus);
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
          [this, received = std::move(received), clipboardRevision, autoPublish]() mutable {
            m_progress->setRange(0, 100);
            m_progress->setValue(100);
            if (received.directory.isEmpty()) {
              m_status->setText(tr("The receiver confirmed the transfer."));
            } else {
              showReceived(std::move(received));
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

void FileTransferDialog::showReceived(enhanced::ReceivedFiles received)
{
  m_received = std::move(received);
  m_receivedItems->clear();
  for (const auto &path : m_received.selection) {
    auto *item = new QListWidgetItem(QFileInfo(path).fileName(), m_receivedItems);
    item->setData(Qt::UserRole, path);
  }
  m_receivedPanel->show();
}

std::unique_ptr<QMimeData> FileTransferDialog::receivedMime(const QStringList &paths)
{
  if (paths.isEmpty())
    return {};
  const QFileInfo batch(m_received.directory);
  if (!isReceivedDirectory(batch)) {
    m_status->setText(tr("A received file or folder is no longer available."));
    return {};
  }
  QList<QUrl> urls;
  urls.reserve(paths.size());
  for (const auto &path : paths) {
    const QFileInfo file(path);
    if ((!file.isFile() && !file.isDir()) || file.isSymLink() || file.isJunction() ||
        file.absolutePath() != batch.absoluteFilePath()) {
      m_status->setText(tr("A received file or folder is no longer available."));
      return {};
    }
    const auto url = QUrl::fromLocalFile(path);
    urls.append(url);
  }
  auto data = std::make_unique<LocalFileMimeData>();
  data->setUrls(urls);
#ifdef Q_OS_WIN
  data->setData(PreferredDropEffect, QByteArray("\x01\x00\x00\x00", 4));
#endif
  return data;
}

void FileTransferDialog::copyReceived()
{
  auto data = receivedMime(m_received.selection);
  if (!data)
    return;
  QByteArray gnomeFiles("copy");
  for (const auto &url : data->urls())
    gnomeFiles += '\n' + url.toEncoded();
  data->setData(GnomeCopiedFiles, gnomeFiles);
  data->setData(KdeCutSelection, "0");
  data->setData(ReceivedMime, "1");
  QGuiApplication::clipboard()->setMimeData(data.release());
  m_status->setText(tr("Files are on the clipboard. Paste in your file manager."));
}

void FileTransferDialog::refreshActions()
{
  const bool available = !m_worker && !m_armed;
  const bool configured = available && !m_identity.fingerprint.isEmpty();
  m_settings->setEnabled(available);
  m_selectionActions->setEnabled(available);
  m_selection->setEnabled(available);
  m_receivedItems->setEnabled(available);
  m_send->setEnabled(configured && m_selection->count() > 0);
  m_receive->setEnabled(configured);
  m_arm->setEnabled(configured);
  m_cancel->setEnabled(!available);
  m_copy->setEnabled(available && !m_received.selection.isEmpty());
  m_openFolder->setEnabled(available && !m_received.directory.isEmpty());
  m_autoPublish->setEnabled(available);
}

} // namespace deskflow::gui
