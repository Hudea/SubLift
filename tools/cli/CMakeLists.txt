add_executable(sublift_cli
  main.cpp
)

target_link_libraries(sublift_cli
  PRIVATE
    sublift_engine
    sublift_adapter_ffmpeg
    sublift_adapter_paddle
    sublift_text
    sublift_timing
    sublift_core
    sublift_ports
)

add_executable(sublift::cli ALIAS sublift_cli)
