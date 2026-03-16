# Linux CMakeLists.txt — uses prebuilt lib if available, otherwise builds from source.
cmake_minimum_required(VERSION 3.14)

set(PROJECT_NAME "libtorrent_flutter")
project(${PROJECT_NAME} LANGUAGES CXX)

set(PREBUILT_SO "${CMAKE_CURRENT_SOURCE_DIR}/../prebuilt/linux/x64/liblibtorrent_flutter.so")

if(EXISTS "${PREBUILT_SO}")
  message(STATUS "Using prebuilt Linux native library")
  set(libtorrent_flutter_bundled_libraries
    "${PREBUILT_SO}"
    PARENT_SCOPE
  )
else()
  message(STATUS "Building from source (no prebuilt found at ${PREBUILT_SO})")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_BINARY_DIR}/shared")
  set(libtorrent_flutter_bundled_libraries
    $<TARGET_FILE:libtorrent_flutter>
    PARENT_SCOPE
  )
endif()
