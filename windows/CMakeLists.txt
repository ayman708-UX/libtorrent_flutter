# Windows CMakeLists.txt — uses prebuilt lib if available, otherwise builds from source.
cmake_minimum_required(VERSION 3.14)

set(PROJECT_NAME "libtorrent_flutter")
project(${PROJECT_NAME} LANGUAGES CXX)

# Check for prebuilt library first
set(PREBUILT_DLL "${CMAKE_CURRENT_SOURCE_DIR}/../prebuilt/windows/x64/libtorrent_flutter.dll")

if(EXISTS "${PREBUILT_DLL}")
  message(STATUS "Using prebuilt Windows native library")
  set(libtorrent_flutter_bundled_libraries
    "${PREBUILT_DLL}"
    PARENT_SCOPE
  )
  # Also bundle any dependency DLLs in the prebuilt dir
  file(GLOB PREBUILT_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../prebuilt/windows/x64/*.dll")
  set(libtorrent_flutter_bundled_libraries
    ${PREBUILT_DEPS}
    PARENT_SCOPE
  )
else()
  message(STATUS "Building from source (no prebuilt found at ${PREBUILT_DLL})")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_BINARY_DIR}/shared")
  set(libtorrent_flutter_bundled_libraries
    $<TARGET_FILE:libtorrent_flutter>
    PARENT_SCOPE
  )
endif()
