cmake_minimum_required(VERSION 3.21)

project(hi5central_portable_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HI5CENTRAL_AGENT_VERSION "0.3.0-alpha" CACHE STRING "Hi5Central portable Agent version")

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
if(APPLE)
  # The macOS Remote Helper must be self-contained. libdatachannel otherwise
  # resolves Homebrew's dylibs, which are not present on customer Macs.
  set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "" FORCE)
  find_package(OpenSSL REQUIRED)
endif()

include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)

set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v12.0.1
)
FetchContent_MakeAvailable(nlohmann_json ixwebsocket)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  libdatachannel
  GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
  GIT_TAG v0.24.5
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libdatachannel)

set(HI5_REMOTE_SOURCES)
set(HI5_REMOTE_LIBRARIES)

if(APPLE)
  enable_language(OBJCXX)
  list(APPEND HI5_REMOTE_SOURCES
    src/remote_webrtc_parent_macos.cpp
  )
elseif(UNIX)
  pkg_check_modules(VPX REQUIRED IMPORTED_TARGET vpx)
  pkg_check_modules(HI5_X11 REQUIRED IMPORTED_TARGET x11 xext xtst)
  list(APPEND HI5_REMOTE_SOURCES
    src/remote_vp8_encoder.cpp
    src/remote_webrtc.cpp
    src/remote_platform_linux.cpp
  )
  list(APPEND HI5_REMOTE_LIBRARIES
    LibDataChannel::LibDataChannelStatic
    PkgConfig::VPX
    PkgConfig::HI5_X11
  )
else()
  message(FATAL_ERROR "The portable Agent target currently supports macOS and Linux only.")
endif()

add_executable(Hi5CentralAgent
  src/main.cpp
  src/http_client.cpp
  src/platform_info.cpp
  src/enhanced_inventory.cpp
  src/agent_websocket.cpp
  src/job_executor.cpp
  src/execution_context.cpp
  src/portable_live_tools.cpp
  src/software_inventory.cpp
  src/software_actions.cpp
  ${HI5_REMOTE_SOURCES}
)

target_include_directories(Hi5CentralAgent PRIVATE src)
target_compile_definitions(Hi5CentralAgent PRIVATE
  HI5CENTRAL_AGENT_VERSION="${HI5CENTRAL_AGENT_VERSION}"
)
target_link_libraries(Hi5CentralAgent PRIVATE
  CURL::libcurl
  nlohmann_json::nlohmann_json
  ixwebsocket::ixwebsocket
  Threads::Threads
  ${HI5_REMOTE_LIBRARIES}
)

if(UNIX)
  find_library(HI5_UTIL_LIBRARY util)
  if(HI5_UTIL_LIBRARY)
    target_link_libraries(Hi5CentralAgent PRIVATE ${HI5_UTIL_LIBRARY})
  endif()
endif()

if(APPLE)
  target_compile_definitions(Hi5CentralAgent PRIVATE HI5_PLATFORM_MACOS=1)
  target_link_libraries(Hi5CentralAgent PRIVATE
    "-framework ApplicationServices"
    "-framework Cocoa"
    "-framework CoreGraphics"
  )

  add_executable(Hi5CentralRemoteHelper
    src/remote_helper_main_macos.mm
    src/remote_h264_vt_encoder.mm
    src/remote_webrtc.cpp
    src/remote_platform_macos.mm
  )
  target_include_directories(Hi5CentralRemoteHelper PRIVATE src)
  target_compile_definitions(Hi5CentralRemoteHelper PRIVATE
    HI5_PLATFORM_MACOS=1
    HI5CENTRAL_AGENT_VERSION="${HI5CENTRAL_AGENT_VERSION}"
  )
  set_source_files_properties(
    src/remote_helper_main_macos.mm
    src/remote_h264_vt_encoder.mm
    src/remote_platform_macos.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
  )
  target_link_libraries(Hi5CentralRemoteHelper PRIVATE
    nlohmann_json::nlohmann_json
    LibDataChannel::LibDataChannelStatic
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    "-framework ApplicationServices"
    "-framework Cocoa"
    "-framework CoreGraphics"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework ScreenCaptureKit"
    "-framework VideoToolbox"
  )
elseif(UNIX)
  target_compile_definitions(Hi5CentralAgent PRIVATE HI5_PLATFORM_LINUX=1)
endif()

install(TARGETS Hi5CentralAgent RUNTIME DESTINATION bin)
if(APPLE)
  install(TARGETS Hi5CentralRemoteHelper RUNTIME DESTINATION bin)
endif()