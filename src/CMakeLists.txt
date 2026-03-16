# src/CMakeLists.txt — Shared build for torrent_bridge native library
# Called from windows/, linux/, android/, and macos/ platform dirs.

cmake_minimum_required(VERSION 3.14)
project(libtorrent_flutter_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(libtorrent_flutter SHARED
    torrent_bridge.cpp
    torrent_bridge.h
)

target_compile_definitions(libtorrent_flutter PRIVATE
    TORRENT_BRIDGE_EXPORTS
    TORRENT_NO_DEPRECATE
)

# ── Platform-specific linking ──────────────────────────────────────────────────

if(WIN32)
    target_compile_definitions(libtorrent_flutter PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601
    )

    if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain")
    endif()
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
    # macOS: Use pkg-config (Homebrew) with explicit Boost include
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBTORRENT REQUIRED libtorrent-rasterbar)
        target_include_directories(libtorrent_flutter PRIVATE ${LIBTORRENT_INCLUDE_DIRS})
        target_link_directories(libtorrent_flutter PRIVATE ${LIBTORRENT_LIBRARY_DIRS})
        target_link_libraries(libtorrent_flutter PRIVATE ${LIBTORRENT_LIBRARIES})

        # Boost headers are needed but pkg-config for libtorrent may not include them
        find_package(Boost QUIET)
        if(Boost_FOUND)
            target_include_directories(libtorrent_flutter PRIVATE ${Boost_INCLUDE_DIRS})
        else()
            # Homebrew fallback: Apple Silicon + Intel
            foreach(_dir "/opt/homebrew/include" "/usr/local/include")
                if(EXISTS "${_dir}/boost/config.hpp")
                    target_include_directories(libtorrent_flutter PRIVATE "${_dir}")
                    message(STATUS "Found Boost headers at ${_dir}")
                    break()
                endif()
            endforeach()
        endif()
    else()
        find_package(LibtorrentRasterbar REQUIRED)
        target_link_libraries(libtorrent_flutter PRIVATE LibtorrentRasterbar::torrent-rasterbar)
    endif()

else()
    # Linux: Use pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBTORRENT REQUIRED libtorrent-rasterbar)
    target_include_directories(libtorrent_flutter PRIVATE ${LIBTORRENT_INCLUDE_DIRS})
    target_link_directories(libtorrent_flutter PRIVATE ${LIBTORRENT_LIBRARY_DIRS})
    target_link_libraries(libtorrent_flutter PRIVATE ${LIBTORRENT_LIBRARIES})
endif()

# ── Visibility ─────────────────────────────────────────────────────────────────
set_target_properties(libtorrent_flutter PROPERTIES
    C_VISIBILITY_PRESET   hidden
    CXX_VISIBILITY_PRESET hidden
)

if(NOT WIN32)
    set_target_properties(libtorrent_flutter PROPERTIES
        PREFIX ""
        OUTPUT_NAME "libtorrent_flutter"
    )
endif()
