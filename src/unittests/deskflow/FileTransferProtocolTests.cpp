/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransferProtocolTests.h"
#include "deskflow/Clipboard.h"
#include "deskflow/FileTransferProtocol.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"

#include <algorithm>
#include <cstring>

namespace {

class FrameStream : public deskflow::IStream
{
public:
  explicit FrameStream(QByteArray bytes) : m_bytes(std::move(bytes))
  {
  }
  void close() override
  {
  }
  uint32_t read(void *buffer, uint32_t size) override
  {
    ++readCalls;
    size = std::min(size, getSize());
    if (buffer != nullptr)
      std::memcpy(buffer, m_bytes.constData() + m_offset, size);
    m_offset += size;
    return size;
  }
  void write(const void *bytes, uint32_t size) override
  {
    m_bytes.append(static_cast<const char *>(bytes), size);
  }
  void flush() override
  {
  }
  void shutdownInput() override
  {
  }
  void shutdownOutput() override
  {
  }
  void *getEventTarget() const override
  {
    return nullptr;
  }
  bool isReady() const override
  {
    return getSize() > 0;
  }
  uint32_t getSize() const override
  {
    return static_cast<uint32_t>(m_bytes.size()) - m_offset;
  }
  int readCalls = 0;

private:
  QByteArray m_bytes;
  uint32_t m_offset = 0;
};

QByteArray frame(uint32_t declaredSize, const QByteArray &payload)
{
  QByteArray bytes;
  bytes.append(static_cast<char>(declaredSize >> 24));
  bytes.append(static_cast<char>(declaredSize >> 16));
  bytes.append(static_cast<char>(declaredSize >> 8));
  bytes.append(static_cast<char>(declaredSize));
  bytes.append(payload);
  return bytes;
}

} // namespace

void FileTransferProtocolTests::readsBoundedFrame()
{
  const QByteArray payload(128 * 1024, 'x');
  FrameStream stream(frame(static_cast<uint32_t>(payload.size()), payload));
  QCOMPARE(deskflow::filetransfer::readFrame(&stream, stream.getSize() + 4), payload);
  QCOMPARE(stream.getSize(), uint32_t(0));
  QCOMPARE(stream.readCalls, 2);
}

void FileTransferProtocolTests::readsFrameWrittenByProtocol()
{
  FrameStream stream(QByteArray{});
  const std::string payload("\x00\x7f\xff", 3);
  ProtocolUtil::writef(&stream, kMsgDFileCopy, &payload);
  const auto packetSize = stream.getSize();
  QByteArray code(4, Qt::Uninitialized);
  QCOMPARE(stream.read(code.data(), 4), uint32_t(4));
  QCOMPARE(code, QByteArray("DFCD"));
  QCOMPARE(deskflow::filetransfer::readFrame(&stream, packetSize), QByteArray::fromStdString(payload));
}

void FileTransferProtocolTests::rejectsMalformedFrames_data()
{
  QTest::addColumn<QByteArray>("bytes");
  QTest::newRow("short-prefix") << QByteArray(3, '\0');
  QTest::newRow("empty-payload") << frame(0, {});
  QTest::newRow("oversized-length") << frame(0xffffffff, "x");
  QTest::newRow("truncated-payload") << frame(4, "abc");
  QTest::newRow("trailing-data") << frame(2, "abc");
}

void FileTransferProtocolTests::rejectsMalformedFrames()
{
  QFETCH(QByteArray, bytes);
  FrameStream stream(bytes);
  QVERIFY_EXCEPTION_THROWN(deskflow::filetransfer::readFrame(&stream, stream.getSize() + 4), BadClientException);
  QVERIFY(stream.readCalls <= 1);
}

void FileTransferProtocolTests::rejectsOversizedPacketBeforeReading()
{
  FrameStream stream(QByteArray(128 * 1024 + 5, 'x'));
  QVERIFY_EXCEPTION_THROWN(deskflow::filetransfer::readFrame(&stream, stream.getSize() + 4), BadClientException);
  QCOMPARE(stream.readCalls, 0);
}

void FileTransferProtocolTests::doesNotConsumeNextPacketAfterEmptyFrame()
{
  // PacketStreamFilter has already advanced to the next packet when a
  // malformed DFCD packet contains only its four-byte code.
  FrameStream nextPacket(frame(3, "abc"));
  QVERIFY_EXCEPTION_THROWN(deskflow::filetransfer::readFrame(&nextPacket, 4), BadClientException);
  QCOMPARE(nextPacket.readCalls, 0);
  QCOMPARE(nextPacket.getSize(), uint32_t(7));
}

void FileTransferProtocolTests::fileReferencesStayOutOfLegacyClipboard()
{
  Clipboard clipboard;
  QVERIFY(clipboard.open(0));
  clipboard.add(IClipboard::Format::Text, "ordinary text");
  clipboard.add(IClipboard::Format::Files, "C:\\private-source.txt");
  clipboard.close();
  const auto wireData = clipboard.marshall();
  QVERIFY(wireData.find("private-source") == std::string::npos);

  clipboard.unmarshall(wireData, 0);
  QVERIFY(clipboard.open(0));
  QCOMPARE(clipboard.get(IClipboard::Format::Text), std::string("ordinary text"));
  QVERIFY(!clipboard.has(IClipboard::Format::Files));
  clipboard.close();

  const auto injected = frame(1, frame(static_cast<uint32_t>(IClipboard::Format::Files), frame(1, "x")));
  clipboard.unmarshall(injected.toStdString(), 0);
  QVERIFY(clipboard.open(0));
  QVERIFY(!clipboard.has(IClipboard::Format::Files));
  clipboard.close();
}

QTEST_GUILESS_MAIN(FileTransferProtocolTests)
