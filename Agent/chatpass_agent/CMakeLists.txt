cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

project(native_vp8_stream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HI5_REQUIRE_STATIC_CRT "Require static MSVC runtime (/MT) for agent builds" ON)

if(MSVC AND HI5_REQUIRE_STATIC_CRT)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  message(STATUS "Hi5Tech Agent: forcing static MSVC runtime (/MT, /MTd)")
endif()

if(MSVC)
  add_compile_options(/EHsc)
endif()

function(hi5_use_static_msvc_runtime target_name)
  if(MSVC AND HI5_REQUIRE_STATIC_CRT)
    set_property(TARGET ${target_name} PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
  endif()
endfunction()

set(LIBDATACHANNEL_ROOT "C:/Users/Dan/Desktop/libdatachannel-install-static" CACHE PATH "Path to local static libdatachannel install")
option(HI5_FETCH_LIBDATACHANNEL "Fetch a pinned libdatachannel source tree for reproducible CI builds" OFF)
set(HI5_LIBDATACHANNEL_VERSION "v0.24.5" CACHE STRING "Pinned libdatachannel release used for reproducible builds")

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

find_path(VPX_INCLUDE_DIRS "vpx/vpx_encoder.h")
find_library(VPX_LIBRARIES NAMES vpx libvpx)
if(WIN32)
  find_path(OPUS_INCLUDE_DIR NAMES opus/opus.h)
  find_library(OPUS_LIBRARY NAMES opus)
endif()

if(NOT VPX_INCLUDE_DIRS OR NOT VPX_LIBRARIES)
  message(FATAL_ERROR "libvpx not found")
endif()
if(WIN32 AND (NOT OPUS_INCLUDE_DIR OR NOT OPUS_LIBRARY))
  message(FATAL_ERROR "libopus not found")
endif()

set(HI5_LIBDATACHANNEL_LINK_LIBS)
set(HI5_LIBDATACHANNEL_INCLUDE_DIRS)

if(HI5_FETCH_LIBDATACHANNEL)
  include(FetchContent)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
  set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
  set(NO_TESTS ON CACHE BOOL "" FORCE)
  set(USE_SYSTEM_JSON ON CACHE BOOL "" FORCE)
  set(USE_SYSTEM_SRTP OFF CACHE BOOL "" FORCE)
  set(USE_SYSTEM_JUICE OFF CACHE BOOL "" FORCE)
  set(USE_SYSTEM_USRSCTP OFF CACHE BOOL "" FORCE)
  set(USE_SYSTEM_PLOG OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(libdatachannel GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git GIT_TAG ${HI5_LIBDATACHANNEL_VERSION} GIT_SHALLOW TRUE GIT_SUBMODULES_RECURSE TRUE)
  FetchContent_MakeAvailable(libdatachannel)
  if(NOT TARGET LibDataChannel::LibDataChannelStatic)
    message(FATAL_ERROR "Fetched libdatachannel did not provide LibDataChannel::LibDataChannelStatic")
  endif()
  list(APPEND HI5_LIBDATACHANNEL_LINK_LIBS LibDataChannel::LibDataChannelStatic)
  message(STATUS "Hi5Central Agent: fetched libdatachannel ${HI5_LIBDATACHANNEL_VERSION}")
elseif(LIBDATACHANNEL_ROOT)
  set(LIBDATACHANNEL_INCLUDE_DIR "${LIBDATACHANNEL_ROOT}/include")
  set(LIBDATACHANNEL_LIB_DIR "${LIBDATACHANNEL_ROOT}/lib")
  set(LIBDATACHANNEL_LIB "${LIBDATACHANNEL_LIB_DIR}/datachannel.lib")
  set(LIBDATACHANNEL_USRSCTP_LIB "${LIBDATACHANNEL_LIB_DIR}/usrsctp.lib")
  set(LIBDATACHANNEL_SRTP2_LIB "${LIBDATACHANNEL_LIB_DIR}/srtp2.lib")
  set(LIBDATACHANNEL_JUICE_LIB "${LIBDATACHANNEL_LIB_DIR}/juice.lib")

  foreach(req LIBDATACHANNEL_INCLUDE_DIR LIBDATACHANNEL_LIB LIBDATACHANNEL_USRSCTP_LIB LIBDATACHANNEL_SRTP2_LIB LIBDATACHANNEL_JUICE_LIB)
    if(NOT EXISTS "${${req}}")
      message(FATAL_ERROR "Missing required libdatachannel component: ${req} -> ${${req}}")
    endif()
  endforeach()

  list(APPEND HI5_LIBDATACHANNEL_INCLUDE_DIRS "${LIBDATACHANNEL_INCLUDE_DIR}")
  list(APPEND HI5_LIBDATACHANNEL_LINK_LIBS "${LIBDATACHANNEL_LIB}" "${LIBDATACHANNEL_USRSCTP_LIB}" "${LIBDATACHANNEL_SRTP2_LIB}" "${LIBDATACHANNEL_JUICE_LIB}")
  message(STATUS "Hi5Central Agent: using local libdatachannel install at ${LIBDATACHANNEL_ROOT}")
else()
  message(FATAL_ERROR "Set LIBDATACHANNEL_ROOT to a local static install or enable HI5_FETCH_LIBDATACHANNEL=ON")
endif()

# WebView2 SDK lookup. Recommended quick setup:
#   nuget install Microsoft.Web.WebView2 -OutputDirectory external
# or set WEBVIEW2_SDK to the package directory containing build/native/include.
set(WEBVIEW2_SDK "$ENV{WEBVIEW2_SDK}" CACHE PATH "Path to Microsoft.Web.WebView2 NuGet package root")
if(WEBVIEW2_SDK AND EXISTS "${WEBVIEW2_SDK}/build/native/include/WebView2.h")
  set(WEBVIEW2_INCLUDE_DIR "${WEBVIEW2_SDK}/build/native/include")
  set(WEBVIEW2_LOADER_STATIC "${WEBVIEW2_SDK}/build/native/x64/WebView2LoaderStatic.lib")
else()
  file(GLOB WEBVIEW2_NUGET_DIRS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/external/Microsoft.Web.WebView2*"
    "${CMAKE_SOURCE_DIR}/packages/Microsoft.Web.WebView2*"
  )
  list(SORT WEBVIEW2_NUGET_DIRS)
  list(REVERSE WEBVIEW2_NUGET_DIRS)
  foreach(dir ${WEBVIEW2_NUGET_DIRS})
    if(EXISTS "${dir}/build/native/include/WebView2.h")
      set(WEBVIEW2_INCLUDE_DIR "${dir}/build/native/include")
      set(WEBVIEW2_LOADER_STATIC "${dir}/build/native/x64/WebView2LoaderStatic.lib")
      break()
    endif()
  endforeach()
endif()

if(NOT WEBVIEW2_INCLUDE_DIR OR NOT EXISTS "${WEBVIEW2_INCLUDE_DIR}/WebView2.h")
  message(FATAL_ERROR "WebView2 SDK not found. Run: nuget install Microsoft.Web.WebView2 -OutputDirectory external")
endif()

if(NOT WEBVIEW2_LOADER_STATIC OR NOT EXISTS "${WEBVIEW2_LOADER_STATIC}")
  message(FATAL_ERROR "WebView2LoaderStatic.lib not found. Check Microsoft.Web.WebView2 NuGet package build/native/x64 folder.")
endif()

set(HI5_PLATFORM_SOURCES)

if(WIN32)
  list(APPEND HI5_PLATFORM_SOURCES
    src/platform/windows/windows_platform.cpp
    src/platform/windows/windows_providers.cpp
    src/platform/windows/windows_service.cpp
    src/platform/windows/windows_input.cpp
    src/platform/windows/windows_screen.cpp
    src/platform/windows/windows_inventory.cpp
    src/platform/windows/windows_identity.cpp
    src/platform/windows/windows_codec.cpp	
    src/platform/windows/wasapi_loopback.cpp
  )
elseif(APPLE)
  list(APPEND HI5_PLATFORM_SOURCES
    src/platform/macos/macos_platform.cpp
    src/platform/macos/macos_providers.cpp
  )
elseif(UNIX)
  list(APPEND HI5_PLATFORM_SOURCES
    src/platform/linux/linux_platform.cpp
    src/platform/linux/linux_providers.cpp
  )
endif()

add_executable(native_vp8_stream
  src/main.cpp
  src/streamer_main.cpp
  src/backstage/backstage_host.cpp
  src/backstage/backstage_browser_host.cpp
  src/backstage/backstage_cef_browser.cpp
  src/signaling_client.cpp
  src/h264_mf_encoder.cpp
  src/vp8_encoder.cpp
  src/vp9_mf_encoder.cpp
  src/vp9_vpx_encoder.cpp
  src/av1_mf_encoder.cpp
  src/h265_mf_encoder.cpp
  src/webrtc_sender.cpp
  src/codec_policy.cpp
  src/agent_identity.cpp
  src/session/session_bridge.cpp
  src/ipc/session_launcher.cpp
  src/ipc/named_pipe.cpp
  src/ui/native_banner.cpp
  src/ui/native_chat_window.cpp
  src/ui/native_tray.cpp
  src/ui/agent_presence_controller.cpp
  src/ui/chat_controller.cpp
  src/ui_chat_overlay/chat_overlay_main.cpp
  ${HI5_PLATFORM_SOURCES}
)

hi5_use_static_msvc_runtime(native_vp8_stream)

target_include_directories(native_vp8_stream BEFORE PRIVATE
  ${HI5_LIBDATACHANNEL_INCLUDE_DIRS}
  ${WEBVIEW2_INCLUDE_DIR}
  src
  ${VPX_INCLUDE_DIRS}
  ${OPUS_INCLUDE_DIR}
)

option(HI5_ENABLE_CEF_BROWSER "Enable CEF off-screen browser for Backstage" OFF)
set(CEF_ROOT "$ENV{CEF_ROOT}" CACHE PATH "Path to CEF binary distribution root")
set(CEF_WRAPPER_LIB "$ENV{CEF_WRAPPER_LIB}" CACHE FILEPATH "Path to libcef_dll_wrapper.lib")

if(HI5_ENABLE_CEF_BROWSER)
  if(NOT CEF_ROOT OR NOT EXISTS "${CEF_ROOT}/include/cef_app.h")
    message(FATAL_ERROR "HI5_ENABLE_CEF_BROWSER=ON requires CEF_ROOT pointing at the CEF binary distribution root")
  endif()

  find_library(CEF_LIBCEF_LIB NAMES libcef PATHS "${CEF_ROOT}/Release" "${CEF_ROOT}/Debug" NO_DEFAULT_PATH)
  if(NOT CEF_LIBCEF_LIB)
    message(FATAL_ERROR "libcef.lib not found under ${CEF_ROOT}/Release or ${CEF_ROOT}/Debug")
  endif()

  if(NOT CEF_WRAPPER_LIB OR NOT EXISTS "${CEF_WRAPPER_LIB}")
    find_library(CEF_WRAPPER_LIB_FOUND NAMES libcef_dll_wrapper PATHS
      "${CEF_ROOT}/build/libcef_dll_wrapper/Release"
      "${CEF_ROOT}/build/libcef_dll_wrapper/Debug"
      "${CEF_ROOT}/libcef_dll_wrapper/Release"
      "${CEF_ROOT}/libcef_dll_wrapper/Debug"
      NO_DEFAULT_PATH)
    if(CEF_WRAPPER_LIB_FOUND)
      set(CEF_WRAPPER_LIB "${CEF_WRAPPER_LIB_FOUND}")
    endif()
  endif()

  if(NOT CEF_WRAPPER_LIB OR NOT EXISTS "${CEF_WRAPPER_LIB}")
    message(FATAL_ERROR "libcef_dll_wrapper.lib not found. Build CEF's libcef_dll_wrapper and set CEF_WRAPPER_LIB.")
  endif()

  target_include_directories(native_vp8_stream PRIVATE "${CEF_ROOT}")
  target_compile_definitions(native_vp8_stream PRIVATE HI5_ENABLE_CEF_BROWSER=1)
  target_link_libraries(native_vp8_stream PRIVATE "${CEF_WRAPPER_LIB}" "${CEF_LIBCEF_LIB}")
  message(STATUS "Backstage CEF browser enabled: CEF_ROOT=${CEF_ROOT}")
endif()

target_compile_definitions(native_vp8_stream PRIVATE
  RTC_ENABLE_MEDIA=1
  RTC_ENABLE_WEBSOCKET=1
  RTC_STATIC
  NOMINMAX
  _WIN32_WINNT=0x0A00
)

target_link_libraries(native_vp8_stream PRIVATE
  ${HI5_LIBDATACHANNEL_LINK_LIBS}
  ${WEBVIEW2_LOADER_STATIC}
  nlohmann_json::nlohmann_json
  Threads::Threads
  OpenSSL::SSL
  OpenSSL::Crypto
  ${VPX_LIBRARIES}
  ${OPUS_LIBRARY}
  d3d11
  dxgi
  windowscodecs
  mf
  mfplat
  mfuuid
  strmiids
  ws2_32
  crypt32
  winhttp
  urlmon
  bcrypt
  advapi32
  wtsapi32
  userenv
  user32
  gdi32
  iphlpapi
  psapi
  ole32
  oleaut32
  uuid
  shell32
  shlwapi
  dwmapi
  uxtheme
  d2d1
  dwrite
)

if(MSVC AND HI5_REQUIRE_STATIC_CRT)
  get_property(_runtime TARGET native_vp8_stream PROPERTY MSVC_RUNTIME_LIBRARY)
  message(STATUS "Hi5Tech Agent: native_vp8_stream MSVC_RUNTIME_LIBRARY=${_runtime}")
endif()

set_target_properties(native_vp8_stream PROPERTIES
    OUTPUT_NAME "Hi5CentralAgentService"
)

if(WIN32)
  add_executable(hi5central_user
    src/user_main.cpp
    src/ui/native_banner.cpp
    src/ui/native_chat_window.cpp
    src/ui/native_tray.cpp
  )
  hi5_use_static_msvc_runtime(hi5central_user)
  target_include_directories(hi5central_user PRIVATE src)
  target_compile_definitions(hi5central_user PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
  target_link_libraries(hi5central_user PRIVATE
    nlohmann_json::nlohmann_json Threads::Threads
    crypt32 advapi32 user32 gdi32 shell32 dwmapi d2d1 dwrite ole32
  )
  set_target_properties(hi5central_user PROPERTIES OUTPUT_NAME "Hi5CentralUser")

  add_executable(hi5central_remote_host
    src/remote_host_main.cpp
    src/streamer_main.cpp
    src/backstage/backstage_host.cpp
    src/backstage/backstage_browser_host.cpp
    src/backstage/backstage_cef_browser.cpp
    src/ipc/named_pipe.cpp
    src/ipc/session_launcher.cpp
    src/platform/windows/windows_input.cpp
    src/platform/windows/windows_screen.cpp
  )
  hi5_use_static_msvc_runtime(hi5central_remote_host)
  target_include_directories(hi5central_remote_host PRIVATE
    ${WEBVIEW2_INCLUDE_DIR}
    src
  )
  target_compile_definitions(hi5central_remote_host PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
  target_link_libraries(hi5central_remote_host PRIVATE
    ${WEBVIEW2_LOADER_STATIC}
    nlohmann_json::nlohmann_json Threads::Threads
    d3d11 dxgi windowscodecs advapi32 user32 gdi32 shell32 shlwapi
    ole32 oleaut32 uuid wtsapi32 userenv psapi dwmapi uxtheme
  )
  set_target_properties(hi5central_remote_host PROPERTIES OUTPUT_NAME "Hi5CentralRemoteHost")

  # Session-scoped media worker. Keep WebRTC/codecs/audio out of the long-lived
  # service and out of the multifunction capture/backstage RemoteHost binary.
  add_executable(hi5central_media_host
    src/media_host_entry.cpp
    src/media_host_main.cpp
    src/ipc/named_pipe.cpp
    src/webrtc_sender.cpp
    src/vp8_encoder.cpp
    src/vp9_mf_encoder.cpp
    src/vp9_vpx_encoder.cpp
    src/h264_mf_encoder.cpp
    src/h265_mf_encoder.cpp
    src/av1_mf_encoder.cpp
    src/platform/windows/windows_input.cpp
    src/platform/windows/windows_screen.cpp
    src/platform/windows/shared_gpu_frame_reader.cpp
    src/platform/windows/windows_codec.cpp
    src/platform/windows/wasapi_loopback.cpp
  )
  hi5_use_static_msvc_runtime(hi5central_media_host)
  target_include_directories(hi5central_media_host BEFORE PRIVATE
    ${HI5_LIBDATACHANNEL_INCLUDE_DIRS}
    ${VPX_INCLUDE_DIRS}
    ${OPUS_INCLUDE_DIR}
    src
  )
  target_compile_definitions(hi5central_media_host PRIVATE
    RTC_ENABLE_MEDIA=1 RTC_ENABLE_WEBSOCKET=1 RTC_STATIC
    NOMINMAX _WIN32_WINNT=0x0A00
  )
  target_link_libraries(hi5central_media_host PRIVATE
    ${HI5_LIBDATACHANNEL_LINK_LIBS}
    nlohmann_json::nlohmann_json Threads::Threads
    OpenSSL::SSL OpenSSL::Crypto ${VPX_LIBRARIES} ${OPUS_LIBRARY}
    d3d11 dxgi windowscodecs mf mfplat mfuuid strmiids
    ws2_32 crypt32 winhttp urlmon bcrypt advapi32 user32 gdi32 shell32 shlwapi
    ole32 oleaut32 uuid wtsapi32 userenv psapi dwmapi iphlpapi
  )
  set_target_properties(hi5central_media_host PROPERTIES OUTPUT_NAME "Hi5CentralMediaHost")

  # Short-lived patch worker. The long-lived Agent owns platform
  # authentication and hands PatchHost only device/job-scoped work.
  add_executable(hi5central_patch_host
    src/patching/patch_host_main.cpp
  )
  hi5_use_static_msvc_runtime(hi5central_patch_host)
  target_include_directories(hi5central_patch_host PRIVATE src)
  target_compile_definitions(hi5central_patch_host PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
  target_link_libraries(hi5central_patch_host PRIVATE
    nlohmann_json::nlohmann_json
    crypt32 bcrypt urlmon wintrust version
    advapi32 user32 shell32
  )
  set_target_properties(hi5central_patch_host PROPERTIES OUTPUT_NAME "Hi5CentralPatchHost")
endif()
