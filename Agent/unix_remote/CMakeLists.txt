cmake_minimum_required(VERSION 3.21)
project(hi5central_unix_remote LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(USE_NICE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  libdatachannel
  GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
  GIT_TAG v0.24.5
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(libdatachannel)

find_package(Threads REQUIRED)
find_package(X11 REQUIRED COMPONENTS X11 Xinerama Xtst)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VPX REQUIRED IMPORTED_TARGET vpx)

add_executable(Hi5CentralRemoteHost
  src/main.cpp
  src/unix_webrtc_sender.cpp
  src/x11_frame_source.cpp
  src/x11_input.cpp
  ../chatpass_agent/src/vp8_encoder.cpp
)

target_include_directories(Hi5CentralRemoteHost PRIVATE
  src
  ../chatpass_agent/src
)

target_link_libraries(Hi5CentralRemoteHost PRIVATE
  LibDataChannel::LibDataChannelStatic
  PkgConfig::VPX
  Threads::Threads
  X11::X11
  X11::Xinerama
  X11::Xtst
)

target_compile_definitions(Hi5CentralRemoteHost PRIVATE
  RTC_ENABLE_MEDIA=1
  HI5_UNIX_REMOTE=1
)

install(TARGETS Hi5CentralRemoteHost RUNTIME DESTINATION bin)
