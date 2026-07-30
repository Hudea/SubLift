add_library(sublift_ffmpeg STATIC
  pure_contracts.cpp
  process_utils.cpp
  resolve_bin.cpp
  probe.cpp
  ffmpeg_extractor.cpp
  plan_frame_io.cpp
)
add_library(sublift::ffmpeg ALIAS sublift_ffmpeg)

target_link_libraries(sublift_ffmpeg
  PUBLIC
    sublift_core
    nlohmann_json::nlohmann_json
  PRIVATE
    sublift_models
)
