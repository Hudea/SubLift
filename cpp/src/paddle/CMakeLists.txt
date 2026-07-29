add_library(sublift_paddle_geometry STATIC paddle_geometry.cpp)
add_library(sublift::paddle_geometry ALIAS sublift_paddle_geometry)

target_include_directories(sublift_paddle_geometry
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_paddle_geometry PUBLIC sublift_core)

add_library(sublift_paddle STATIC paddle_ocr.cpp)
add_library(sublift::paddle ALIAS sublift_paddle)

target_include_directories(sublift_paddle
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_paddle
  PUBLIC
    sublift_core
    sublift_paddle_geometry
)

if(SUBLIFT_ENABLE_PADDLE AND ONNXRuntime_FOUND)
  target_compile_definitions(sublift_paddle PUBLIC SUBLIFT_HAS_PADDLE=1)
  target_link_libraries(sublift_paddle PRIVATE ONNXRuntime::ONNXRuntime)
endif()
