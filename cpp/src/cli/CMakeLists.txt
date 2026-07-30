add_executable(sublift_cli main.cpp)

# Product native CLI: talk to sublift_worker over UDS (framing/protocol in sublift_ipc).
# sublift_ipc already PUBLIC-links core/ffmpeg/vision — do not also link those here.
target_link_libraries(sublift_cli PRIVATE sublift_ipc)
if(COMMAND sublift_use_bundled_onnxruntime)
  sublift_use_bundled_onnxruntime(sublift_cli)
endif()

# Also install/alias as `sublift` next to sublift_worker for PATH-friendly product use.
add_custom_command(
  TARGET sublift_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sublift_cli>
          $<TARGET_FILE_DIR:sublift_cli>/sublift
  COMMENT "Copy sublift_cli → sublift (native product CLI)"
)
