cmake_minimum_required(VERSION 3.20)
project(sublift_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default static libs; hidden visibility (docs/cpp/architecture.md §2.1)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

option(SUBLIFT_ENABLE_VISION "Build sublift_vision_macos (ObjC++/Vision)" OFF)
option(SUBLIFT_SANITIZE "Enable ASan+UBSan (Debug configs)" OFF)
option(SUBLIFT_BUILD_TESTS "Build Catch2 tests and register CTest" ON)

find_package(OpenCV QUIET)
if(OpenCV_FOUND)
  message(STATUS "OpenCV found: ${OpenCV_VERSION} (not linked in feat-06002 stubs)")
else()
  message(STATUS "OpenCV not found (optional; OK for 06002)")
endif()

if(SUBLIFT_SANITIZE)
  add_compile_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)
  add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

add_subdirectory(src/core)
add_subdirectory(src/ffmpeg)
add_subdirectory(src/test_support)
add_subdirectory(src/worker)
add_subdirectory(src/cli)

if(SUBLIFT_ENABLE_VISION)
  if(NOT APPLE)
    message(FATAL_ERROR "SUBLIFT_ENABLE_VISION requires macOS (AppleClang + OBJCXX)")
  endif()
  enable_language(OBJCXX)
  add_subdirectory(src/vision_macos)
endif()

if(SUBLIFT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
