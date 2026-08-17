# cpp/src/server/CMakeLists.txt

add_library(sublift_server_runtime STATIC
  routes.cpp
  http_server.cpp
  job_manager.cpp
  workspace_manager.cpp
)
add_library(sublift::server_runtime ALIAS sublift_server_runtime)

target_include_directories(sublift_server_runtime PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_server_runtime PUBLIC
  sublift_application
  sublift_worker_runtime
  sublift_protocol
  sublift_core
  sublift_models
  sublift_ffmpeg
  sublift_vision_macos
  sublift_paddle
  sublift_httplib
  nlohmann_json::nlohmann_json
  Threads::Threads
)

add_executable(sublift_server
  main.cpp
)

target_link_libraries(sublift_server PRIVATE
  sublift_server_runtime
)

if(COMMAND sublift_use_bundled_onnxruntime)
  sublift_use_bundled_onnxruntime(sublift_server)
endif()
