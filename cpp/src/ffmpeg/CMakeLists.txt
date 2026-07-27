add_library(sublift_ffmpeg STATIC ffmpeg_stub.cpp)
add_library(sublift::ffmpeg ALIAS sublift_ffmpeg)

target_link_libraries(sublift_ffmpeg PUBLIC sublift_core)
