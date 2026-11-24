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

option(ENABLE_ENHANCED_FUZZ "Build the enhanced wire decoder libFuzzer target" OFF)
if(ENABLE_ENHANCED_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$" OR MSVC)
    message(FATAL_ERROR "ENABLE_ENHANCED_FUZZ requires Clang and a libFuzzer runtime")
  endif()
  set(enhanced_dir ${CMAKE_SOURCE_DIR}/src/lib/enhanced)
  add_executable(EnhancedDecoderFuzz
    EnhancedDecoderFuzz.cpp
    ${enhanced_dir}/FileManifest.cpp
    ${enhanced_dir}/InputLease.cpp
    ${enhanced_dir}/InputRouter.cpp
    ${enhanced_dir}/Protocol.cpp
  )
  target_link_libraries(EnhancedDecoderFuzz PRIVATE common)
  target_compile_options(EnhancedDecoderFuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(EnhancedDecoderFuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
