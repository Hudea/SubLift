add_library(sublift_core STATIC
  version.cpp
  image.cpp
  models.cpp
)
add_library(sublift::core ALIAS sublift_core)

target_include_directories(sublift_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Core is pure C++ types; JSON stays on test_support / future worker only.
