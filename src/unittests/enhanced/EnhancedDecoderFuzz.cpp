/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "enhanced/FileManifest.h"
#include "enhanced/InputLease.h"
#include "enhanced/Protocol.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *bytes, std::size_t size)
{
  using namespace deskflow::enhanced;
  if (size == 0 || size > MaxPayload + 1)
    return 0;

  const QByteArrayView input(reinterpret_cast<const char *>(bytes + 1), static_cast<qsizetype>(size - 1));
  try {
    switch (bytes[0] % 4) {
    case 0:
      decodeHeader(input, 1);
      break;
    case 1:
      FileManifest::decode(input);
      break;
    case 2:
      InputPoll::decode(input);
      break;
    case 3:
      InputState::decode(input);
      break;
    }
  } catch (const Error &) {
    // Rejected protocol messages are expected.
  }
  return 0;
}
