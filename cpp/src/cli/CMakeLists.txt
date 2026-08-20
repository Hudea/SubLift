add_library(sublift_cli_lib STATIC
  args.cpp
  srt.cpp
  runtime_guard.cpp
  worker_locator.cpp
)
add_library(sublift::cli ALIAS sublift_cli_lib)

target_include_directories(sublift_cli_lib PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# Thin product native CLI: talks to sublift_worker over UDS.
# Only links protocol and core. Does NOT link ORT, OpenCV, or Vision.
target_link_libraries(sublift_cli_lib PUBLIC sublift_protocol sublift_core)

add_executable(sublift_cli main.cpp)
target_link_libraries(sublift_cli PRIVATE sublift_cli_lib)

# Also install/alias as `sublift` next to sublift_worker for PATH-friendly product use.
add_custom_command(
  TARGET sublift_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sublift_cli>
          $<TARGET_FILE_DIR:sublift_cli>/sublift
  COMMENT "Copy sublift_cli → sublift (native product CLI)"
)
