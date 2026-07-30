set(_sublift_pipeline_srcs
  ${PROJECT_SOURCE_DIR}/include/sublift/pipeline/signature.hpp
  ${PROJECT_SOURCE_DIR}/include/sublift/pipeline/changepoint.hpp
  ${PROJECT_SOURCE_DIR}/include/sublift/pipeline/pipeline.hpp
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
