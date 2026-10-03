cmake_minimum_required(VERSION 3.20)
project(hi5central_portable_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HI5CENTRAL_AGENT_VERSION "0.3.0-alpha" CACHE STRING "Hi5Central portable Agent version")

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

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
)

if(UNIX)
  find_library(HI5_UTIL_LIBRARY util)
  if(HI5_UTIL_LIBRARY)
    target_link_libraries(Hi5CentralAgent PRIVATE ${HI5_UTIL_LIBRARY})
  endif()
endif()

if(APPLE)
  target_compile_definitions(Hi5CentralAgent PRIVATE HI5_PLATFORM_MACOS=1)
elseif(UNIX)
  target_compile_definitions(Hi5CentralAgent PRIVATE HI5_PLATFORM_LINUX=1)
else()
  message(FATAL_ERROR "The portable Agent target currently supports macOS and Linux only.")
endif()

install(TARGETS Hi5CentralAgent RUNTIME DESTINATION bin)