# SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
# SPDX-License-Identifier: MIT

add_library(enhanced STATIC
  FileManifest.cpp
  FileManifest.h
  Protocol.cpp
  Protocol.h
  TlsChannel.cpp
  TlsChannel.h
)
target_link_libraries(enhanced PUBLIC common Qt6::Network PRIVATE net)
target_compile_definitions(enhanced INTERFACE DESKFLOW_ENHANCED)
