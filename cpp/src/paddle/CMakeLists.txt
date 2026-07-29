add_library(sublift_paddle_geometry STATIC paddle_geometry.cpp)
add_library(sublift::paddle_geometry ALIAS sublift_paddle_geometry)

target_include_directories(sublift_paddle_geometry
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_paddle_geometry PUBLIC sublift_core)
