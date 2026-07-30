add_executable(sublift_cli main.cpp)

# Thin product native CLI: talks to sublift_worker over UDS.
# Only links sublift_protocol and sublift_core. Does NOT link ORT, OpenCV, or Vision.
target_link_libraries(sublift_cli PRIVATE sublift_protocol sublift_core)

# Also install/alias as `sublift` next to sublift_worker for PATH-friendly product use.
add_custom_command(
  TARGET sublift_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sublift_cli>
          $<TARGET_FILE_DIR:sublift_cli>/sublift
  COMMENT "Copy sublift_cli → sublift (native product CLI)"
)
