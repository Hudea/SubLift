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
