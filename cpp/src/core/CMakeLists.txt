set(_sublift_core_srcs
  version.cpp
  hash.cpp
  image.cpp
  models.cpp
  timeline.cpp
  dedupe.cpp
  line_select.cpp
)

add_library(sublift_core STATIC ${_sublift_core_srcs})
add_library(sublift::core ALIAS sublift_core)

target_include_directories(sublift_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

set(_sublift_pipeline_srcs
  ${PROJECT_SOURCE_DIR}/include/sublift/signature.hpp
  ${PROJECT_SOURCE_DIR}/include/sublift/changepoint.hpp
  ${PROJECT_SOURCE_DIR}/include/sublift/pipeline.hpp
)
if(SUBLIFT_ENABLE_OPENCV)
  list(APPEND _sublift_pipeline_srcs signature.cpp changepoint.cpp pipeline.cpp)
endif()

add_library(sublift_pipeline STATIC ${_sublift_pipeline_srcs})
add_library(sublift::pipeline ALIAS sublift_pipeline)

target_include_directories(sublift_pipeline
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_pipeline PUBLIC sublift_core)

if(SUBLIFT_ENABLE_OPENCV)
  target_link_libraries(sublift_pipeline PRIVATE ${OpenCV_LIBS})
  target_include_directories(sublift_pipeline PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
