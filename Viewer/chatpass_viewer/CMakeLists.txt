cmake_minimum_required(VERSION 3.22)

project(hi5central_viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG master
)

set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_AMALGAMATION OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(webview)

add_executable(hi5tech-viewer
    src/main.cpp
    src/app/viewer_app.cpp
    src/core/logger.cpp
    src/core/deep_link.cpp
)

target_include_directories(hi5tech-viewer PRIVATE
    src
)

target_link_libraries(hi5tech-viewer PRIVATE
    webview::core
)

if (WIN32)
    target_compile_definitions(hi5tech-viewer PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
endif()

add_custom_command(TARGET hi5tech-viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:hi5tech-viewer>\\web"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/web/index.html"
        "$<TARGET_FILE_DIR:hi5tech-viewer>\\web\\index.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/web/renderer.js"
        "$<TARGET_FILE_DIR:hi5tech-viewer>\\web\\renderer.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/web/file_browser.html"
        "$<TARGET_FILE_DIR:hi5tech-viewer>\\web\\file_browser.html"
)
# Hi5Central rebrand: keep internal target for compatibility, but output Hi5CentralViewer.exe
set_target_properties(hi5tech-viewer PROPERTIES
    OUTPUT_NAME "Hi5CentralViewer"
)