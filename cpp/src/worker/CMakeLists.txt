add_executable(sublift_worker main.cpp)

# ffmpeg PUBLIC-links core; avoid duplicate static archives on the link line.
target_link_libraries(sublift_worker PRIVATE sublift_ffmpeg)

if(SUBLIFT_ENABLE_VISION)
  target_link_libraries(sublift_worker PRIVATE sublift_vision_macos)
endif()
