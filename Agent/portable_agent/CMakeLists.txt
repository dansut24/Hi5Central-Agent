cmake_minimum_required(VERSION 3.20)
project(hi5central_portable_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HI5CENTRAL_AGENT_VERSION "0.3.0-alpha" CACHE STRING "Hi5Central portable Agent version")

find_package(CURL REQUIRED)

include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(Hi5CentralAgent
  src/main.cpp
  src/http_client.cpp
  src/platform_info.cpp
)

target_include_directories(Hi5CentralAgent PRIVATE src)

target_compile_definitions(Hi5CentralAgent PRIVATE
  HI5CENTRAL_AGENT_VERSION="${HI5CENTRAL_AGENT_VERSION}"
)

target_link_libraries(Hi5CentralAgent PRIVATE
  CURL::libcurl
  nlohmann_json::nlohmann_json
)

if(APPLE)
  target_compile_definitions(Hi5CentralAgent PRIVATE HI5_PLATFORM_MACOS=1)
elseif(UNIX)
  target_compile_definitions(Hi5CentralAgent PRIVATE HI5_PLATFORM_LINUX=1)
else()
  message(FATAL_ERROR "The portable Agent target currently supports macOS and Linux only.")
endif()

install(TARGETS Hi5CentralAgent RUNTIME DESTINATION bin)
