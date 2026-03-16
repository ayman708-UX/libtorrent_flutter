# src/CMakeLists.txt — Shared build for torrent_bridge native library
# Called from windows/, linux/, android/, and macos/ platform dirs.

cmake_minimum_required(VERSION 3.14)

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
    # Windows: Use vcpkg or manually installed libtorrent
    target_compile_definitions(libtorrent_flutter PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0601
    )

    # Try vcpkg first
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
    # Android: Link against prebuilt libtorrent static libs
    # Users must place .a files in android/src/main/jniLibs/<abi>/
    set(PREBUILT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../android/src/main/jniLibs/${ANDROID_ABI}")

    if(EXISTS "${PREBUILT_DIR}/libtorrent-rasterbar.a")
        # Use prebuilt static library
        add_library(libtorrent_static STATIC IMPORTED)
        set_target_properties(libtorrent_static PROPERTIES
            IMPORTED_LOCATION "${PREBUILT_DIR}/libtorrent-rasterbar.a"
        )
        # Boost headers should be in android/src/main/include/
        set(BOOST_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../android/src/main/include")
        set(LIBTORRENT_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../android/src/main/include")

        target_include_directories(libtorrent_flutter PRIVATE
            ${LIBTORRENT_INCLUDE}
            ${BOOST_INCLUDE}
        )
        target_link_libraries(libtorrent_flutter PRIVATE
            libtorrent_static
            log
        )
    else()
        message(WARNING "Prebuilt libtorrent not found for ${ANDROID_ABI} at ${PREBUILT_DIR}. "
                "Build will fail at link time. See README for build instructions.")
        target_link_libraries(libtorrent_flutter PRIVATE log)
    endif()

elseif(APPLE)
    # macOS: Use Homebrew or system-installed libtorrent
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBTORRENT REQUIRED libtorrent-rasterbar)
        target_include_directories(libtorrent_flutter PRIVATE ${LIBTORRENT_INCLUDE_DIRS})
        target_link_directories(libtorrent_flutter PRIVATE ${LIBTORRENT_LIBRARY_DIRS})
        target_link_libraries(libtorrent_flutter PRIVATE ${LIBTORRENT_LIBRARIES})
    else()
        # Fallback: try find_package
        find_package(LibtorrentRasterbar REQUIRED)
        target_link_libraries(libtorrent_flutter PRIVATE
            LibtorrentRasterbar::torrent-rasterbar
        )
    endif()

else()
    # Linux: Use pkg-config (system-installed libtorrent-rasterbar-dev)
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

# On non-Windows, ensure the output is named correctly
if(NOT WIN32)
    set_target_properties(libtorrent_flutter PROPERTIES
        PREFIX ""
        OUTPUT_NAME "libtorrent_flutter"
    )
endif()
