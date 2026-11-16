cmake_minimum_required(VERSION 3.20)
project(Hi5CentralStreamingLab LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/EHsc /W4)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
find_package(LibDataChannel CONFIG REQUIRED)
find_path(VPX_INCLUDE_DIR vpx/vpx_encoder.h REQUIRED)
find_library(VPX_LIBRARY NAMES vpx libvpx REQUIRED)
find_path(LIBYUV_INCLUDE_DIR libyuv/convert.h REQUIRED)
find_library(LIBYUV_LIBRARY NAMES yuv libyuv REQUIRED)
add_executable(Hi5StreamingLab native/main.cpp)
target_include_directories(Hi5StreamingLab PRIVATE ${VPX_INCLUDE_DIR} ${LIBYUV_INCLUDE_DIR})
target_compile_definitions(Hi5StreamingLab PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 RTC_ENABLE_MEDIA=1)
target_link_libraries(Hi5StreamingLab PRIVATE
  LibDataChannel::LibDataChannel
  ${VPX_LIBRARY} ${LIBYUV_LIBRARY}
  ws2_32 user32 gdi32 bcrypt
)
set_target_properties(Hi5StreamingLab PROPERTIES
  OUTPUT_NAME "Hi5StreamingLab"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
)
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${cfg}" ucfg)
  set_target_properties(Hi5StreamingLab PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${ucfg} "${CMAKE_BINARY_DIR}/dist")
endforeach()
