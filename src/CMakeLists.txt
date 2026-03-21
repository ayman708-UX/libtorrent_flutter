# src/CMakeLists.txt — Shared build for torrent_bridge native library

cmake_minimum_required(VERSION 3.14)
cmake_policy(SET CMP0091 NEW)

# vcpkg toolchain must be set before project()
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
endif()

project(libtorrent_flutter_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Help find_package locate vcpkg-installed packages
if(DEFINED ENV{VCPKG_ROOT})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{VCPKG_ROOT}/installed/x64-windows")
endif()

add_library(libtorrent_flutter SHARED
    torrent_bridge.cpp
    torrent_bridge.h
)

target_compile_definitions(libtorrent_flutter PRIVATE
    TORRENT_BRIDGE_EXPORTS
)

# TORRENT_NO_DEPRECATE changes ABI version (v2:: namespace).
# Only use it when building libtorrent from source (Android).
# System libraries (Homebrew, apt, vcpkg) use ABI v1 = no v2:: namespace.
if(ANDROID)
    target_compile_definitions(libtorrent_flutter PRIVATE TORRENT_NO_DEPRECATE)
endif()

# ── Platform-specific linking ──────────────────────────────────────────────────

if(WIN32)
    target_compile_definitions(libtorrent_flutter PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601
    )
    find_package(LibtorrentRasterbar REQUIRED)
    target_link_libraries(libtorrent_flutter PRIVATE
        LibtorrentRasterbar::torrent-rasterbar
        ws2_32
    )
    if(MSVC)
        target_compile_options(libtorrent_flutter PRIVATE /W3 /utf-8 /bigobj)
    endif()

elseif(ANDROID)
    set(PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../android/src/main/jniLibs/${ANDROID_ABI}")
    if(EXISTS "${PREBUILT_DIR}/libtorrent-rasterbar.a")
        add_library(libtorrent_static STATIC IMPORTED)
        set_target_properties(libtorrent_static PROPERTIES
            IMPORTED_LOCATION "${PREBUILT_DIR}/libtorrent-rasterbar.a"
        )
        set(BOOST_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../android/src/main/include")
        set(LIBTORRENT_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../android/src/main/include")
        target_include_directories(libtorrent_flutter PRIVATE ${LIBTORRENT_INCLUDE} ${BOOST_INCLUDE})
        target_link_libraries(libtorrent_flutter PRIVATE libtorrent_static log)
    else()
        message(WARNING "No prebuilt libtorrent for ${ANDROID_ABI}. See README.")
        target_link_libraries(libtorrent_flutter PRIVATE log)
    endif()

elseif(APPLE)
    # macOS: Prefer CMake config files (Homebrew ships LibtorrentRasterbarConfig.cmake)
    # Try Homebrew paths both ARM and Intel
    list(APPEND CMAKE_PREFIX_PATH
        "/opt/homebrew/opt/libtorrent-rasterbar"
        "/opt/homebrew"
        "/usr/local/opt/libtorrent-rasterbar"
        "/usr/local"
    )

    find_package(LibtorrentRasterbar QUIET)
    if(LibtorrentRasterbar_FOUND)
        message(STATUS "Found LibtorrentRasterbar via cmake config")
        target_link_libraries(libtorrent_flutter PRIVATE LibtorrentRasterbar::torrent-rasterbar)
    else()
        # Fallback: find the library manually
        message(STATUS "LibtorrentRasterbar cmake config not found, searching manually")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBTORRENT REQUIRED libtorrent-rasterbar)

        target_include_directories(libtorrent_flutter PRIVATE ${LIBTORRENT_INCLUDE_DIRS})

        # Use full library paths from pkg-config
        find_library(LT_FULL_LIB
            NAMES torrent-rasterbar
            PATHS ${LIBTORRENT_LIBRARY_DIRS}
            NO_DEFAULT_PATH
        )
        if(LT_FULL_LIB)
            target_link_libraries(libtorrent_flutter PRIVATE ${LT_FULL_LIB})
        else()
            target_link_directories(libtorrent_flutter PRIVATE ${LIBTORRENT_LIBRARY_DIRS})
            target_link_libraries(libtorrent_flutter PRIVATE ${LIBTORRENT_LIBRARIES})
        endif()
    endif()

    # Boost headers
    find_package(Boost QUIET)
    if(Boost_FOUND)
        target_include_directories(libtorrent_flutter PRIVATE ${Boost_INCLUDE_DIRS})
    else()
        foreach(_dir "/opt/homebrew/include" "/usr/local/include")
            if(EXISTS "${_dir}/boost/config.hpp")
                target_include_directories(libtorrent_flutter PRIVATE "${_dir}")
                break()
            endif()
        endforeach()
    endif()

    # OpenSSL headers (keg-only on Homebrew)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_include_directories(libtorrent_flutter PRIVATE ${OPENSSL_INCLUDE_DIR})
    else()
        foreach(_dir "/opt/homebrew/opt/openssl/include" "/usr/local/opt/openssl/include")
            if(EXISTS "${_dir}/openssl/opensslv.h")
                target_include_directories(libtorrent_flutter PRIVATE "${_dir}")
                break()
            endif()
        endforeach()
    endif()

else()
    # Linux: Use pkg-config IMPORTED_TARGET
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBTORRENT REQUIRED IMPORTED_TARGET libtorrent-rasterbar)
    target_link_libraries(libtorrent_flutter PRIVATE PkgConfig::LIBTORRENT)
endif()

# ── Visibility ─────────────────────────────────────────────────────────────────
set_target_properties(libtorrent_flutter PROPERTIES
    C_VISIBILITY_PRESET   hidden
    CXX_VISIBILITY_PRESET hidden
)

if(NOT WIN32)
    set_target_properties(libtorrent_flutter PROPERTIES
        PREFIX ""
        OUTPUT_NAME "liblibtorrent_flutter"
    )
endif()
