list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(APPLE)
  set(_sublift_vision_default ON)
else()
  set(_sublift_vision_default OFF)
endif()
option(SUBLIFT_ENABLE_VISION "Enable Apple Vision OCR adapter on macOS" ${_sublift_vision_default})

if(SUBLIFT_ENABLE_VISION)
  if(NOT APPLE)
    message(FATAL_ERROR "SUBLIFT_ENABLE_VISION requires macOS (AppleClang + OBJCXX)")
  endif()
  if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS deployment target")
  endif()
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
  set(CMAKE_OBJCXX_EXTENSIONS OFF)
  enable_language(OBJCXX)
endif()

include(cmake/Dependencies.cmake)

# -----------------------------------------------------------------------------
# 1. sublift_core
# -----------------------------------------------------------------------------
add_library(sublift_core STATIC
  src/core/geometry.cpp
  src/core/image_buffer.cpp
  src/core/image_view.cpp
  src/core/models.cpp
)

target_include_directories(sublift_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_library(sublift::core ALIAS sublift_core)

# -----------------------------------------------------------------------------
# 2. sublift_features
# -----------------------------------------------------------------------------
add_library(sublift_features STATIC
  src/features/adaptive_component_filter.cpp
  src/features/adaptive_line_cluster.cpp
  src/features/adaptive_presence_mask.cpp
  src/features/dhash.cpp
  src/features/geometry.cpp
  src/features/mask.cpp
  src/features/ssim.cpp
  src/features/visual_analysis.cpp
)

target_include_directories(sublift_features
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(sublift_features
  PUBLIC
    sublift_core
  PRIVATE
    ${OpenCV_LIBS}
)

add_library(sublift::features ALIAS sublift_features)

# -----------------------------------------------------------------------------
# 3. sublift_timing
# -----------------------------------------------------------------------------
add_library(sublift_timing STATIC
  src/timing/changepoint.cpp
  src/timing/frame_analysis.cpp
  src/timing/segment.cpp
  src/timing/timeline.cpp
)

target_include_directories(sublift_timing
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_timing
  PUBLIC
    sublift_core
    sublift_features
)

add_library(sublift::timing ALIAS sublift_timing)

# -----------------------------------------------------------------------------
# 4. sublift_text
# -----------------------------------------------------------------------------
add_library(sublift_text STATIC
  src/text/cleaning.cpp
  src/text/consensus.cpp
  src/text/dedupe.cpp
  src/text/line_select.cpp
  src/text/region_cluster.cpp
  src/text/srt.cpp
)

target_include_directories(sublift_text
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_text
  PUBLIC
    sublift_core
)

add_library(sublift::text ALIAS sublift_text)

# -----------------------------------------------------------------------------
# 5. sublift_ports
# -----------------------------------------------------------------------------
add_library(sublift_ports INTERFACE)

target_include_directories(sublift_ports
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_ports
  INTERFACE
    sublift_core
)

add_library(sublift::ports ALIAS sublift_ports)

# -----------------------------------------------------------------------------
# 6. sublift_engine
# -----------------------------------------------------------------------------
add_library(sublift_engine STATIC
  src/engine/analysis_executor.cpp
  src/engine/auto_region.cpp
  src/engine/config.cpp
  src/engine/observer.cpp
  src/engine/segment_recognizer.cpp
  src/engine/session.cpp
)

target_include_directories(sublift_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_engine
  PUBLIC
    sublift_ports
    sublift_core
    sublift_timing
    sublift_text
)

add_library(sublift::engine ALIAS sublift_engine)

# -----------------------------------------------------------------------------
# 7. sublift_adapter_mock & sublift_adapter_ffmpeg & sublift_adapter_paddle & sublift_adapter_apple_vision
# -----------------------------------------------------------------------------
add_subdirectory(adapters/mock)
add_subdirectory(adapters/ffmpeg)
add_subdirectory(adapters/paddle)
add_subdirectory(adapters/apple_vision)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
add_subdirectory(tests)
