cmake_minimum_required(VERSION 3.20)
project(gunbros_arm64_bridge LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BRIDGE_DEBUG "Enable bridge debug logging" OFF)
if(BRIDGE_DEBUG)
    add_compile_definitions(BRIDGE_DEBUG=1)
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(GUNBROS_COMMON_SOURCES
    src/common/sha256.c
)

# Common sources shared across bridge and tests
set(GUNBROS_FS_SOURCES
    src/filesystem/path_mapper.c
    src/filesystem/file_handles.c
    src/filesystem/filesystem_logger.c
    src/filesystem/file_operations.c
    src/filesystem/directory_operations.c
)

set(GUNBROS_PLATFORM_SOURCES
    src/platform/pcm_ring.c
    src/platform/jni_env_helper.c
    src/platform/android_window.c
    src/platform/android_audio.c
    src/platform/network_time.c
    src/platform/ngs_ext.c
)

set(GUNBROS_COMPAT_SOURCES
    src/compatibility/game_patches.c
    src/compatibility/game_hooks.c
    src/compatibility/jni_virtual_machine.c
    src/compatibility/jni_methods.c
    src/compatibility/jni_audio.c
    src/compatibility/jni_textbox.c
)

set(GUNBROS_HLE_SOURCES
    src/hle/libc_string.c
    src/hle/libc_time.c
    src/hle/libc_stdio.c
    src/hle/net_stubs.c
    src/hle/gles1.c
    src/hle/hle_table.c
)

set(GUNBROS_EMULATOR_SOURCES
    src/emulator/guest_memory.c
    src/emulator/elf_loader.c
    src/emulator/dynarmic_backend.cpp
    ${GUNBROS_HLE_SOURCES}
)

set(GUNBROS_BRIDGE_SOURCES
    src/bridge/bridge_core.c
    src/bridge/jni_exports.c
)

# Target for Android Build
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android")
    # Load .env file if present
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.env")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/.env" ENV_LINES ENCODING UTF-8)
        foreach(line ${ENV_LINES})
            string(STRIP "${line}" line)
            if(line MATCHES "^([^#=]+)=(.*)$")
                set(var_name "${CMAKE_MATCH_1}")
                string(STRIP "${var_name}" var_name)
                set(var_val "${CMAKE_MATCH_2}")
                string(STRIP "${var_val}" var_val)
                string(REGEX REPLACE "^[\"'](.*)[\"']$" "\\1" var_val "${var_val}")
                if(var_val MATCHES "^\\./(.*)")
                    set(var_val "${CMAKE_CURRENT_SOURCE_DIR}/${CMAKE_MATCH_1}")
                endif()
                if(NOT DEFINED ENV{${var_name}} AND NOT DEFINED ${var_name})
                    set(ENV{${var_name}} "${var_val}")
                endif()
            endif()
        endforeach()
    endif()

    if(NOT DEFINED DYNARMIC_ROOT)
        if(DEFINED ENV{DYNARMIC_ROOT} AND EXISTS "$ENV{DYNARMIC_ROOT}")
            set(DYNARMIC_ROOT "$ENV{DYNARMIC_ROOT}")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dynarmic")
            set(DYNARMIC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dynarmic")
        else()
            message(FATAL_ERROR "DYNARMIC_ROOT not found. Please set DYNARMIC_ROOT in .env or via -DDYNARMIC_ROOT=/path/to/dynarmic")
        endif()
    endif()

    if(NOT DEFINED BOOST_ROOT)
        if(DEFINED ENV{BOOST_ROOT} AND EXISTS "$ENV{BOOST_ROOT}")
            set(BOOST_ROOT "$ENV{BOOST_ROOT}")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost/usr/include")
            set(BOOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost/usr/include")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost/include")
            set(BOOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost/include")
        elseif(EXISTS "/usr/include/boost")
            set(BOOST_ROOT "/usr/include")
        else()
            message(FATAL_ERROR "BOOST_ROOT not found. Please set BOOST_ROOT in .env or via -DBOOST_ROOT=/path/to/boost/include")
        endif()
    endif()

    include_directories(
        ${DYNARMIC_ROOT}/src
        ${DYNARMIC_ROOT}/externals/oaknut/include
        ${DYNARMIC_ROOT}/externals/fmt/include
        ${DYNARMIC_ROOT}/externals/mcl/include
        ${DYNARMIC_ROOT}/externals/robin-map/include
        SYSTEM ${BOOST_ROOT}
    )

    add_library(androidplatformjni SHARED
        ${GUNBROS_COMMON_SOURCES}
        ${GUNBROS_FS_SOURCES}
        ${GUNBROS_PLATFORM_SOURCES}
        ${GUNBROS_COMPAT_SOURCES}
        ${GUNBROS_EMULATOR_SOURCES}
        ${GUNBROS_BRIDGE_SOURCES}
    )

    set(DYNARMIC_BUILD_DIR "${DYNARMIC_ROOT}/build-arm64")
    target_link_libraries(androidplatformjni PRIVATE
        ${DYNARMIC_BUILD_DIR}/src/dynarmic/libdynarmic.a
        ${DYNARMIC_BUILD_DIR}/externals/fmt/libfmt.a
        ${DYNARMIC_BUILD_DIR}/externals/mcl/src/libmcl.a
        log
        aaudio
        EGL
        GLESv1_CM
        android
        m
    )

    set_target_properties(androidplatformjni PROPERTIES
        OUTPUT_NAME "androidplatformjni"
        LINK_FLAGS "-Wl,-soname,libandroidplatformjni.so -static-libstdc++"
    )
endif()

# Standalone Tests (Host or CTest)
option(BUILD_TESTING "Build GunBros automated test suite" ON)

if(BUILD_TESTING)
    enable_testing()

    add_library(gunbros_fs_core STATIC
        ${GUNBROS_FS_SOURCES}
    )
    target_include_directories(gunbros_fs_core PUBLIC include)

    add_executable(test_path_mapper tests/filesystem/test_path_mapper.c)
    target_link_libraries(test_path_mapper PRIVATE gunbros_fs_core)
    add_test(NAME test_path_mapper COMMAND test_path_mapper)

    add_executable(test_file_operations tests/filesystem/test_file_operations.c)
    target_link_libraries(test_file_operations PRIVATE gunbros_fs_core)
    add_test(NAME test_file_operations COMMAND test_file_operations)

    add_executable(test_host_io_persistence tests/filesystem/test_host_io_persistence.c)
    target_link_libraries(test_host_io_persistence PRIVATE gunbros_fs_core)
    add_test(NAME test_host_io_persistence COMMAND test_host_io_persistence)

    # Target alias for backwards compatibility
    add_custom_target(test_persistence DEPENDS test_host_io_persistence)

    add_executable(test_pcm_ring tests/platform/test_pcm_ring.c src/platform/pcm_ring.c)
    target_include_directories(test_pcm_ring PRIVATE include)
    add_test(NAME test_pcm_ring COMMAND test_pcm_ring)

    add_executable(test_guest_memory tests/emulator/test_guest_memory.c src/emulator/guest_memory.c)
    target_include_directories(test_guest_memory PRIVATE include)
    add_test(NAME test_guest_memory COMMAND test_guest_memory)

    add_executable(test_hle_libc
        tests/test_hle_libc.c
        src/hle/libc_string.c
        src/hle/libc_time.c
        src/hle/libc_stdio.c
        src/emulator/guest_memory.c
    )
    target_include_directories(test_hle_libc PRIVATE include src/hle)
    target_link_libraries(test_hle_libc PRIVATE gunbros_fs_core m)
    add_test(NAME test_hle_libc COMMAND test_hle_libc)

    add_executable(golden_save_runner
        tests/golden/golden_save_runner.c
        src/hle/libc_string.c
        src/hle/libc_time.c
        src/hle/libc_stdio.c
        src/emulator/guest_memory.c
    )
    target_include_directories(golden_save_runner PRIVATE include src/hle)
    target_link_libraries(golden_save_runner PRIVATE gunbros_fs_core m)
    add_test(NAME golden_save_runner COMMAND golden_save_runner)

    add_executable(test_sha256
        tests/common/test_sha256.c
        src/common/sha256.c
    )
    target_include_directories(test_sha256 PRIVATE include)
    add_test(NAME test_sha256 COMMAND test_sha256)

    add_executable(test_patch_equivalence
        tests/compatibility/test_patch_equivalence.c
        src/compatibility/game_patches.c
    )
    target_include_directories(test_patch_equivalence PRIVATE include)
    add_test(NAME test_patch_equivalence COMMAND test_patch_equivalence)

    add_executable(test_hle_resolution
        tests/hle/test_hle_resolution.c
        src/hle/hle_table.c
        src/hle/libc_string.c
        src/hle/libc_time.c
        src/hle/libc_stdio.c
        src/hle/net_stubs.c
        src/hle/gles1.c
        src/emulator/guest_memory.c
    )
    target_include_directories(test_hle_resolution PRIVATE include src/hle)
    target_link_libraries(test_hle_resolution PRIVATE gunbros_fs_core m)
    add_test(NAME test_hle_resolution COMMAND test_hle_resolution)
endif()
