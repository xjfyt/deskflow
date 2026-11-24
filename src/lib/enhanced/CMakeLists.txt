# SPDX-FileCopyrightText: (C) 2026 Deskflow Enhanced contributors
# SPDX-License-Identifier: MIT

add_library(enhanced STATIC
  FileManifest.cpp
  FileManifest.h
  FileTransfer.cpp
  FileTransfer.h
  InputHub.cpp
  InputHub.h
  InputLease.cpp
  InputLease.h
  InputRouter.cpp
  InputRouter.h
  InputSession.cpp
  InputSession.h
  Protocol.cpp
  Protocol.h
  TlsChannel.cpp
  TlsChannel.h
)
target_link_libraries(enhanced PUBLIC common Qt6::Network PRIVATE net)
target_compile_definitions(enhanced INTERFACE DESKFLOW_ENHANCED)

if(WIN32)
  target_sources(enhanced PRIVATE windows/WindowsInput.cpp windows/WindowsInput.h)
  target_link_libraries(enhanced PRIVATE user32)
elseif(APPLE)
  find_library(enhanced_CoreFoundation CoreFoundation REQUIRED)
  target_sources(enhanced PRIVATE macos/MacInput.cpp macos/MacInput.h)
  target_link_libraries(enhanced PRIVATE ${lib_ApplicationServices} ${lib_Carbon} ${enhanced_CoreFoundation})
  target_compile_definitions(enhanced INTERFACE DESKFLOW_ENHANCED_MACOS)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND BUILD_X11_SUPPORT AND HAVE_XI2 AND HAVE_Xi AND HAVE_Xtst)
  target_sources(enhanced PRIVATE x11/X11Input.cpp x11/X11Input.h)
  target_link_libraries(enhanced PRIVATE X11::X11 X11::Xi X11::Xtst Qt6::DBus)
  target_compile_definitions(enhanced INTERFACE DESKFLOW_ENHANCED_X11)
endif()
