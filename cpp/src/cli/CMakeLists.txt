add_executable(sublift_cli main.cpp)

# ffmpeg PUBLIC-links core; avoid duplicate static archives on the link line.
target_link_libraries(sublift_cli PRIVATE sublift_ffmpeg)
