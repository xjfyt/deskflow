/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "client/ServerProxyFileTransfer.h"

#include "client/Client.h"
#include "deskflow/FileTransferProtocol.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"

#include <cstring>

ServerProxyFileTransfer::ServerProxyFileTransfer(Client *client, deskflow::IStream *stream, IEventQueue *events)
    : ServerProxy1_8(client, stream, events),
      m_fileClient(client),
      m_transfer(events, stream, [client](const QStringList &paths, std::optional<uint32_t> expectedSequence) {
        return client->publishTransferredFiles(paths, expectedSequence);
      })
{
  m_transfer.setReadyCallback([this] { requestFiles(); });
}

void ServerProxyFileTransfer::onOptionsChanged(const OptionsList &options)
{
  bool advertised = false;
  for (size_t i = 0; i + 1 < options.size(); i += 2) {
    if (options[i] == kOptionFileCopy && options[i + 1] == kFileCopyCapability) {
      advertised = true;
    }
  }
  if (!advertised || !m_fileClient->fileClipboardEnabled() || !deskflow::FileTransferBridge::enabled()) {
    onOptionsReset();
    return;
  }
  m_advertised = true;
  requestFiles();
}

void ServerProxyFileTransfer::requestFiles()
{
  if (!m_transfer.available()) {
    m_recovering = m_recovering || m_requested || m_negotiated;
    m_requested = false;
    m_negotiated = false;
    return;
  }
  if (m_advertised && m_transfer.available() && !m_requested) {
    m_requested = true;
    ProtocolUtil::writef(getStream(), kMsgCFileCopyHello);
  }
}

void ServerProxyFileTransfer::onOptionsReset()
{
  m_recovering = m_recovering || m_requested || m_negotiated;
  m_advertised = false;
  m_requested = false;
  m_negotiated = false;
  m_transfer.setNegotiated(false);
}

ServerProxy::ConnectionResult ServerProxyFileTransfer::parseMessage(const uint8_t *code)
{
  if (std::memcmp(code, kMsgCFileCopyAck, 4) == 0) {
    if (getCurrentMessageSize() != 4)
      throw BadClientException();
    // Configuration updates can leave more than one acknowledgement in flight.
    if (m_negotiated)
      return ConnectionResult::Okay;
    // Pipe failure may be observed here before its queued worker wake is
    // dispatched. A valid in-flight ACK must not tear down keyboard sharing.
    if (m_requested && !m_transfer.available()) {
      requestFiles();
      return ConnectionResult::Okay;
    }
    // Capability withdrawal can race an acknowledgement already on the wire.
    if (!m_requested && (m_everNegotiated || m_recovering))
      return ConnectionResult::Okay;
    if (!m_requested) {
      throw BadClientException();
    }
    m_negotiated = true;
    m_everNegotiated = true;
    m_recovering = false;
    m_transfer.setNegotiated(true);
    return ConnectionResult::Okay;
  }
  if (std::memcmp(code, kMsgDFileCopy, 4) == 0) {
    if (!m_negotiated && !m_everNegotiated && !m_recovering) {
      throw BadClientException();
    }
    const auto payload = deskflow::filetransfer::readFrame(getStream(), getCurrentMessageSize());
    if (m_negotiated)
      m_transfer.receive(payload);
    return ConnectionResult::Okay;
  }
  return ServerProxy1_8::parseMessage(code);
}

bool ServerProxyFileTransfer::onGrabClipboard(ClipboardID id)
{
  if (id == kClipboardClipboard) {
    m_transfer.sourceClipboardChanged();
  }
  return ServerProxy1_8::onGrabClipboard(id);
}

void ServerProxyFileTransfer::offerFiles()
{
  if (m_negotiated)
    m_transfer.offerClipboardFiles();
}

void ServerProxyFileTransfer::onRemoteClipboardChanged(ClipboardID id)
{
  if (id == kClipboardClipboard) {
    m_transfer.sourceClipboardChanged();
  }
}

bool ServerProxyFileTransfer::canTransferFiles() const
{
  return m_negotiated && m_transfer.available();
}
