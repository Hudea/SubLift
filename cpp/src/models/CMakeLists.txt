add_library(sublift_models STATIC model_bundle.cpp)
add_library(sublift::models ALIAS sublift_models)

target_include_directories(sublift_models
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_models PUBLIC sublift_core)
