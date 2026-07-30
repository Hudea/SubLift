set(_sublift_core_srcs
  version.cpp
  hash.cpp
  image.cpp
  models.cpp
  timeline.cpp
  dedupe.cpp
  line_select.cpp
)
if(SUBLIFT_ENABLE_OPENCV)
  list(APPEND _sublift_core_srcs signature.cpp changepoint.cpp pipeline.cpp)
endif()

add_library(sublift_core STATIC ${_sublift_core_srcs})
add_library(sublift::core ALIAS sublift_core)

target_include_directories(sublift_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(SUBLIFT_ENABLE_OPENCV)
  # PRIVATE: OpenCV is an impl detail of signature.cpp only. It must not
  # propagate to core's dependents (cli/worker) - no cv::Mat at the public
  # API boundary (architecture.md §3).
  target_link_libraries(sublift_core PRIVATE ${OpenCV_LIBS})
  target_include_directories(sublift_core PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()

# Core PUBLIC surface stays pure C++ types; JSON stays on test_support /
# future worker only.
