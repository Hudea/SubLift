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
# Soft default ON: prefer signature pipeline when OpenCV is installed.
# Missing OpenCV auto-disables (does not hard-fail configure). Force hard fail
# with -DSUBLIFT_REQUIRE_OPENCV=ON (dev machines with expected brew opencv@4).
option(SUBLIFT_ENABLE_OPENCV
  "Build signature pipeline when OpenCV is available (feat-06101)" ON)
option(SUBLIFT_REQUIRE_OPENCV
  "Fail configure if OpenCV is missing (implies enabling signature)" OFF)

# OpenCV: private impl detail of the signature pipeline (feat-06101). Linked
# PRIVATE into sublift_core only — no cv::Mat on the public API
# (architecture.md §3). Soft discovery keeps models/image builds working
# without a system OpenCV install (architecture.md §2.1 optional OpenCV).
if(SUBLIFT_REQUIRE_OPENCV)
  set(SUBLIFT_ENABLE_OPENCV ON)
endif()

if(SUBLIFT_ENABLE_OPENCV)
  # Homebrew opencv@4 is keg-only; help find_package locate it.
  if(APPLE)
    execute_process(
      COMMAND brew --prefix opencv@4
      OUTPUT_VARIABLE _brew_opencv_prefix
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _brew_opencv_rc)
    if(_brew_opencv_rc EQUAL 0 AND _brew_opencv_prefix)
      list(APPEND CMAKE_PREFIX_PATH "${_brew_opencv_prefix}")
    endif()
  endif()
  if(SUBLIFT_REQUIRE_OPENCV)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc)
  else()
    find_package(OpenCV QUIET COMPONENTS core imgproc)
  endif()
  if(OpenCV_FOUND)
    message(STATUS "OpenCV ${OpenCV_VERSION} enabled (signature pipeline)")
  else()
    set(SUBLIFT_ENABLE_OPENCV OFF)
    message(WARNING
      "OpenCV not found; signature pipeline disabled "
      "(models/image still build). Install: brew install opencv@4 "
      "(then reconfigure). Hard-require with -DSUBLIFT_REQUIRE_OPENCV=ON.")
  endif()
else()
  message(STATUS "OpenCV disabled (signature excluded; models/image still build)")
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
add_subdirectory(src/paddle)


if(SUBLIFT_ENABLE_VISION)
  if(NOT APPLE)
    message(FATAL_ERROR "SUBLIFT_ENABLE_VISION requires macOS (AppleClang + OBJCXX)")
  endif()
  enable_language(OBJCXX)
endif()

add_subdirectory(src/vision_macos)

if(SUBLIFT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
