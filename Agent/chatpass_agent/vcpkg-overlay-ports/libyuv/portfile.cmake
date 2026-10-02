vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://chromium.googlesource.com/libyuv/libyuv
    REF d98915a654d3564e4802a0004add46221c4e4348
    PATCHES
        cmake.diff
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        tools BUILD_TOOLS
)

vcpkg_cmake_get_vars(cmake_vars_file)
include("${cmake_vars_file}")

set(HI5_LIBYUV_BUILD_OPTIONS)
if(VCPKG_DETECTED_CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND NOT VCPKG_TARGET_IS_UWP)
    # libyuv disables its SSE/AVX acceleration when compiled by MSVC.
    # Keep Hi5Central binaries on MSVC /MT, but compile this ABI-compatible
    # static library with clang-cl so runtime CPU dispatch remains accelerated.
    message(STATUS "Hi5Central: building libyuv with clang-cl SIMD support")
    set(VCPKG_POLICY_SKIP_ARCHITECTURE_CHECK enabled)

    find_program(HI5_CLANG_CL
        NAMES clang-cl.exe clang-cl
        HINTS "$ENV{LLVMInstallDir}/x64/bin" "$ENV{LLVMInstallDir}/bin"
        PATHS "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin" "$ENV{VCINSTALLDIR}/Tools/Llvm/bin"
              "C:/Program Files/LLVM/bin"
    )
    if(NOT HI5_CLANG_CL)
        # Local developer machines may not expose LLVM on PATH. Allow vcpkg's
        # normal acquisition fallback there; CI preflights clang-cl explicitly.
        vcpkg_find_acquire_program(CLANG)
        if(CLANG MATCHES "-NOTFOUND")
            message(FATAL_ERROR "clang-cl is required for accelerated Windows libyuv")
        endif()
        get_filename_component(CLANG_DIR "${CLANG}" DIRECTORY)
        set(HI5_CLANG_CL "${CLANG_DIR}/clang-cl.exe")
    else()
        get_filename_component(CLANG_DIR "${HI5_CLANG_CL}" DIRECTORY)
    endif()

    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
        set(CLANG_TARGET "x86_64-pc-windows-msvc")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
        set(CLANG_TARGET "i686-pc-windows-msvc")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        set(CLANG_TARGET "aarch64-pc-windows-msvc")
    else()
        message(FATAL_ERROR "Unsupported libyuv target architecture: ${VCPKG_TARGET_ARCHITECTURE}")
    endif()

    string(APPEND VCPKG_DETECTED_CMAKE_CXX_FLAGS " --target=${CLANG_TARGET}")
    string(APPEND VCPKG_DETECTED_CMAKE_C_FLAGS " --target=${CLANG_TARGET}")
    list(APPEND HI5_LIBYUV_BUILD_OPTIONS
        "-DCMAKE_CXX_COMPILER=${CLANG_DIR}/clang-cl.exe"
        "-DCMAKE_C_COMPILER=${CLANG_DIR}/clang-cl.exe"
        "-DCMAKE_CXX_FLAGS=${VCPKG_DETECTED_CMAKE_CXX_FLAGS}"
        "-DCMAKE_C_FLAGS=${VCPKG_DETECTED_CMAKE_C_FLAGS}"
    )
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    DISABLE_PARALLEL_CONFIGURE
    OPTIONS
        ${FEATURE_OPTIONS}
        ${HI5_LIBYUV_BUILD_OPTIONS}
    OPTIONS_DEBUG
        -DBUILD_TOOLS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup()
if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES yuvconvert yuvconstants AUTO_CLEAN)
endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libyuv/basic_types.h" "defined(LIBYUV_USING_SHARED_LIBRARY)" "1")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/libyuv-config.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
