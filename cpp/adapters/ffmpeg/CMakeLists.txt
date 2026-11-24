add_library(sublift_adapter_ffmpeg STATIC
  src/ffmpeg_frame_source.cpp
  src/plan_frame_io.cpp
  src/probe.cpp
  src/process_utils.cpp
  src/pure_contracts.cpp
  src/resolve_bin.cpp
)

target_include_directories(sublift_adapter_ffmpeg
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_link_libraries(sublift_adapter_ffmpeg
  PUBLIC
    sublift_ports
    sublift_core
  PRIVATE
    nlohmann_json::nlohmann_json
)

add_library(sublift::adapter_ffmpeg ALIAS sublift_adapter_ffmpeg)
