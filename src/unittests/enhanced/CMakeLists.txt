# SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
# SPDX-License-Identifier: MIT

create_test(
  NAME EnhancedProtocolTests
  WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
  DEPENDS enhanced
  SOURCE EnhancedProtocolTests.cpp
)

create_test(
  NAME EnhancedFileTransferTests
  WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
  DEPENDS enhanced
  SOURCE EnhancedFileTransferTests.cpp
)
