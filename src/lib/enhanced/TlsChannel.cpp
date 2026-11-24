/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TlsChannel.h"

#include "net/SecureUtils.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>

namespace deskflow::enhanced {
namespace {

constexpr int SocketBufferSize = 128 * 1024;
constexpr int MaxPemSize = 64 * 1024;
constexpr int MaxWaitSlice = 100;

using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

QByteArray readPem(const QString &path)
{
  QFile file(path);
  const QFileInfo info(path);
  if (!info.isFile() || info.isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > MaxPemSize)
    throw Error(QCoreApplication::translate("Enhanced", "Could not read the enhanced identity."));
  const auto pem = file.read(MaxPemSize + 1);
  if (file.error() != QFileDevice::NoError || pem.size() > MaxPemSize)
    throw Error(QCoreApplication::translate("Enhanced", "Could not read the enhanced identity."));
  return pem;
}

Cert readCertificate(const QByteArray &pem)
{
  const Bio input(BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size())), BIO_free);
  return {input ? PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr) : nullptr, X509_free};
}

int noPassword(char *, int, int, void *)
{
  return 0;
}

int waitSlice(const QDeadlineTimer &deadline, const std::atomic_bool &cancelled)
{
  checkCancelled(cancelled);
  if (deadline.hasExpired())
    throw Error(QCoreApplication::translate("Enhanced", "The enhanced connection timed out."));
  return deadline.isForever() ? MaxWaitSlice
                              : static_cast<int>(std::clamp<qint64>(deadline.remainingTime(), 1, MaxWaitSlice));
}

int verifyPeer(int valid, X509_STORE_CTX *store)
{
  auto *ssl = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
  const auto *pin = ssl ? static_cast<const QByteArray *>(SSL_get_app_data(ssl)) : nullptr;
  auto *cert = X509_STORE_CTX_get_current_cert(store);
  if (!pin || !cert || X509_STORE_CTX_get_error_depth(store) != 0)
    return 0;

  const auto error = X509_STORE_CTX_get_error(store);
  if (!valid && error != X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT)
    return 0;

  std::array<unsigned char, 32> digest{};
  unsigned int size = 0;
  if (X509_digest(cert, EVP_sha256(), digest.data(), &size) != 1 || size != digest.size() ||
      CRYPTO_memcmp(digest.data(), pin->constData(), digest.size()) != 0 ||
      X509_cmp_current_time(X509_get0_notBefore(cert)) >= 0 || X509_cmp_current_time(X509_get0_notAfter(cert)) <= 0) {
    return 0;
  }

  X509_STORE_CTX_set_error(store, X509_V_OK);
  return 1;
}

void configureSocket(QTcpSocket &socket)
{
  socket.setReadBufferSize(SocketBufferSize);
  socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
}

} // namespace

PeerIdentity loadIdentity(const QString &path, bool create)
{
  if (create) {
    const QDir directory = QFileInfo(path).absoluteDir();
    if (!directory.exists() && !directory.mkpath(QStringLiteral(".")))
      throw Error(QCoreApplication::translate("Enhanced", "Could not create the enhanced identity directory."));

    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(500))
      throw Error(QCoreApplication::translate("Enhanced", "The enhanced identity is in use. Try again."));
    if (!QFileInfo::exists(path)) {
      try {
        generatePemSelfSignedCert(path);
      } catch (const std::exception &) {
        throw Error(QCoreApplication::translate("Enhanced", "Could not create the enhanced identity."));
      }
    }
  }

  const auto certificate = readCertificate(readPem(path));
  if (!certificate)
    throw Error(QCoreApplication::translate("Enhanced", "The enhanced identity does not contain a certificate."));
  return {path, sslCertFingerprint(certificate.get(), QCryptographicHash::Sha256).data};
}

struct TlsChannel::State
{
  State(
      QTcpSocket &socket, const std::atomic_bool &cancelled, QByteArray peerPin, QDeadlineTimer lifetime,
      QTcpSocket *requiredTransport
  )
      : socket(socket),
        cancelled(cancelled),
        peerPin(std::move(peerPin)),
        lifetime(lifetime),
        requiredTransport(requiredTransport),
        context(SSL_CTX_new(TLS_method()), SSL_CTX_free),
        ssl(nullptr, SSL_free)
  {
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::readyRead, &eventLoop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::bytesWritten, &eventLoop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::disconnected, &eventLoop, &QEventLoop::quit);
    if (requiredTransport)
      QObject::connect(requiredTransport, &QTcpSocket::disconnected, &eventLoop, &QEventLoop::quit);
  }

  void checkRequiredTransport() const
  {
    if (requiredTransport && requiredTransport->state() != QAbstractSocket::ConnectedState)
      throw Error(QCoreApplication::translate("Enhanced", "The enhanced control connection closed."));
  }

  void waitForSocket(const QDeadlineTimer &deadline)
  {
    checkRequiredTransport();
    if (socket.state() != QAbstractSocket::ConnectedState)
      throw Error(QCoreApplication::translate("Enhanced", "The enhanced connection closed."));
    timer.start(waitSlice(deadline, cancelled));
    eventLoop.exec(QEventLoop::ExcludeUserInputEvents);
    timer.stop();
  }

  void flush(const QDeadlineTimer &deadline)
  {
    auto *output = SSL_get_wbio(ssl.get());
    std::array<char, 16 * 1024> buffer;
    while (BIO_ctrl_pending(output) > 0) {
      checkCancelled(cancelled);
      const auto size = BIO_read(output, buffer.data(), static_cast<int>(buffer.size()));
      if (size <= 0 || socket.write(buffer.data(), size) != size)
        throw Error(QCoreApplication::translate("Enhanced", "Could not write to the enhanced connection."));
    }
    if (socket.bytesToWrite() > 0)
      socket.flush();
    while (socket.bytesToWrite() > 0)
      waitForSocket(deadline);
  }

  void supplyInput(const QDeadlineTimer &deadline)
  {
    while (socket.bytesAvailable() == 0)
      waitForSocket(deadline);
    const auto data = socket.read(16 * 1024);
    if (data.isEmpty() ||
        BIO_write(SSL_get_rbio(ssl.get()), data.constData(), static_cast<int>(data.size())) != data.size())
      throw Error(QCoreApplication::translate("Enhanced", "Could not read from the enhanced connection."));
  }

  template <typename Operation> void perform(Operation operation, const QDeadlineTimer &deadline)
  {
    while (true) {
      checkRequiredTransport();
      waitSlice(deadline, cancelled);
      ERR_clear_error();
      const auto result = operation();
      const auto error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl.get(), result);
      flush(deadline);
      if (error == SSL_ERROR_NONE)
        return;
      if (error == SSL_ERROR_WANT_READ) {
        supplyInput(deadline);
      } else if (error == SSL_ERROR_ZERO_RETURN) {
        throw Error(QCoreApplication::translate("Enhanced", "The enhanced connection closed."));
      } else if (error != SSL_ERROR_WANT_WRITE) {
        throw Error(
            QCoreApplication::translate(
                "Enhanced", "Enhanced TLS connection failed. Check both fingerprints and certificate validity."
            )
        );
      }
    }
  }

  QByteArray read(qsizetype size, const QDeadlineTimer &deadline)
  {
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype offset = 0; offset < size;) {
      size_t count = 0;
      perform(
          [&] { return SSL_read_ex(ssl.get(), bytes.data() + offset, static_cast<size_t>(size - offset), &count); },
          deadline
      );
      offset += static_cast<qsizetype>(count);
    }
    return bytes;
  }

  void write(QByteArrayView data, const QDeadlineTimer &deadline)
  {
    while (!data.isEmpty()) {
      size_t count = 0;
      perform([&] { return SSL_write_ex(ssl.get(), data.data(), static_cast<size_t>(data.size()), &count); }, deadline);
      data = data.sliced(static_cast<qsizetype>(count));
    }
  }

  QTcpSocket &socket;
  const std::atomic_bool &cancelled;
  QByteArray peerPin;
  QDeadlineTimer lifetime;
  QTcpSocket *requiredTransport;
  QEventLoop eventLoop;
  QTimer timer;
  std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context;
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl;
};

TlsChannel::TlsChannel(
    QTcpSocket &socket, const PeerIdentity &identity, const QByteArray &pin, const std::atomic_bool &cancelled,
    bool server, QDeadlineTimer deadline, QDeadlineTimer lifetime, QTcpSocket *requiredTransport
)
    : m_state(std::make_unique<State>(socket, cancelled, pin, lifetime, requiredTransport))
{
  auto *context = m_state->context.get();
  if (pin.size() != TokenSize || !context || SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) != 1)
    throw Error(QCoreApplication::translate("Enhanced", "Could not configure enhanced TLS."));

  SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
  SSL_CTX_set_options(context, SSL_OP_NO_TICKET | SSL_OP_NO_RENEGOTIATION);
  SSL_CTX_set_num_tickets(context, 0);
  SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verifyPeer);
  X509_VERIFY_PARAM_set_flags(SSL_CTX_get0_param(context), X509_V_FLAG_CHECK_SS_SIGNATURE);

  const auto pem = readPem(identity.pemPath);
  const auto cert = readCertificate(pem);
  const Bio keyInput(BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size())), BIO_free);
  const Key key(
      keyInput ? PEM_read_bio_PrivateKey(keyInput.get(), nullptr, noPassword, nullptr) : nullptr, EVP_PKEY_free
  );
  if (!cert || !key || sslCertFingerprint(cert.get(), QCryptographicHash::Sha256).data != identity.fingerprint ||
      SSL_CTX_use_certificate(context, cert.get()) != 1 || SSL_CTX_use_PrivateKey(context, key.get()) != 1 ||
      SSL_CTX_check_private_key(context) != 1) {
    throw Error(
        QCoreApplication::translate("Enhanced", "Could not load the enhanced TLS certificate and private key.")
    );
  }

  m_state->ssl.reset(SSL_new(context));
  auto *ssl = m_state->ssl.get();
  Bio input(BIO_new(BIO_s_mem()), BIO_free);
  Bio output(BIO_new(BIO_s_mem()), BIO_free);
  if (!ssl || !input || !output || SSL_set_app_data(ssl, &m_state->peerPin) != 1)
    throw Error(QCoreApplication::translate("Enhanced", "Could not start enhanced TLS."));
  SSL_set_bio(ssl, input.release(), output.release());
  if (server)
    SSL_set_accept_state(ssl);
  else
    SSL_set_connect_state(ssl);

  try {
    m_state->perform([&] { return SSL_do_handshake(ssl); }, (std::min)(deadline, lifetime));
  } catch (const Error &error) {
    checkCancelled(cancelled);
    m_state->checkRequiredTransport();
    throw PeerHandshakeError(error.message);
  }
  const Cert peer(SSL_get1_peer_certificate(ssl), X509_free);
  if (!peer || SSL_get_verify_result(ssl) != X509_V_OK ||
      sslCertFingerprint(peer.get(), QCryptographicHash::Sha256).data != pin) {
    throw PeerHandshakeError(QCoreApplication::translate("Enhanced", "The enhanced peer fingerprint did not match."));
  }
}

TlsChannel::~TlsChannel() = default;

void TlsChannel::send(Message type, QByteArrayView data, QDeadlineTimer deadline)
{
  const auto header = encodeHeader(type, data.size(), ++m_sendSequence);
  auto frame = header;
  frame.append(data.data(), data.size());
  m_state->write(frame, (std::min)(deadline, m_state->lifetime));
}

QByteArray TlsChannel::receive(Message type, qsizetype minimum, qsizetype maximum, QDeadlineTimer deadline)
{
  deadline = (std::min)(deadline, m_state->lifetime);
  const auto header = decodeHeader(m_state->read(HeaderSize, deadline), ++m_receiveSequence);
  if (header.type != type || header.size < minimum || header.size > maximum)
    throw Error(QCoreApplication::translate("Enhanced", "Unexpected enhanced protocol message."));
  return m_state->read(header.size, deadline);
}

void checkCancelled(const std::atomic_bool &cancelled)
{
  if (cancelled.load(std::memory_order_relaxed))
    throw Error(QCoreApplication::translate("Enhanced", "Transfer cancelled."));
}

void listen(QTcpServer &server, const QHostAddress &address, quint16 port)
{
  server.setProxy(QNetworkProxy::NoProxy);
  server.setListenBacklogSize(2);
  server.setMaxPendingConnections(1);
  if (address.isNull() || port == 0 || !server.listen(address, port))
    throw Error(
        QCoreApplication::translate("Enhanced", "Could not open enhanced port %1 on the selected address.").arg(port)
    );
}

std::unique_ptr<QTcpSocket> accept(
    QTcpServer &server, QDeadlineTimer deadline, const std::atomic_bool &cancelled, const QTcpSocket *requiredTransport
)
{
  QEventLoop eventLoop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
  QObject::connect(&server, &QTcpServer::newConnection, &eventLoop, &QEventLoop::quit);
  if (requiredTransport)
    QObject::connect(requiredTransport, &QTcpSocket::disconnected, &eventLoop, &QEventLoop::quit);

  while (!server.hasPendingConnections()) {
    if (requiredTransport && requiredTransport->state() != QAbstractSocket::ConnectedState)
      throw Error(QCoreApplication::translate("Enhanced", "The enhanced control connection closed."));
    timer.start(waitSlice(deadline, cancelled));
    eventLoop.exec(QEventLoop::ExcludeUserInputEvents);
    timer.stop();
  }
  waitSlice(deadline, cancelled);
  std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
  socket->setParent(nullptr);
  configureSocket(*socket);
  return socket;
}

void connectSocket(
    QTcpSocket &socket, const QHostAddress &address, quint16 port, QDeadlineTimer deadline,
    const std::atomic_bool &cancelled
)
{
  socket.setProxy(QNetworkProxy::NoProxy);
  socket.setReadBufferSize(SocketBufferSize);
  QEventLoop eventLoop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
  QObject::connect(&socket, &QTcpSocket::connected, &eventLoop, &QEventLoop::quit);
  QObject::connect(&socket, &QTcpSocket::errorOccurred, &eventLoop, &QEventLoop::quit);
  try {
    waitSlice(deadline, cancelled);
    socket.connectToHost(address, port);
    while (socket.state() == QAbstractSocket::ConnectingState || socket.state() == QAbstractSocket::HostLookupState) {
      timer.start(waitSlice(deadline, cancelled));
      eventLoop.exec(QEventLoop::ExcludeUserInputEvents);
      timer.stop();
    }
    checkCancelled(cancelled);
    if (socket.state() != QAbstractSocket::ConnectedState)
      throw Error(QCoreApplication::translate("Enhanced", "Could not connect to enhanced port %1.").arg(port));
  } catch (...) {
    socket.abort();
    throw;
  }
  configureSocket(socket);
}

} // namespace deskflow::enhanced
