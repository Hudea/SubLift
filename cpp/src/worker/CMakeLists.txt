set(_sublift_worker_runtime_sources
  engine_factory.cpp
  engine_factory.hpp
  path_media_services.cpp
  path_media_services.hpp
  detector_factory.cpp
  detector_factory.hpp
  connection.cpp
  connection.hpp
)

# Composition-root helpers for sublift_worker (OCR factory + path media + UDS session).
# Historically named sublift_ipc; kept as an alias for tests/docs during migration.
add_library(sublift_worker_runtime STATIC ${_sublift_worker_runtime_sources})
add_library(sublift::worker_runtime ALIAS sublift_worker_runtime)
add_library(sublift_ipc ALIAS sublift_worker_runtime)

target_include_directories(sublift_worker_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_worker_runtime PUBLIC
  sublift_application
  sublift_protocol
)

target_link_libraries(sublift_worker_runtime PRIVATE
  sublift_paddle
  sublift_vision_macos
  sublift_ffmpeg
)

add_executable(sublift_worker main.cpp)

target_link_libraries(sublift_worker PRIVATE
  sublift_application
  sublift_worker_runtime
)
if(COMMAND sublift_use_bundled_onnxruntime)
  sublift_use_bundled_onnxruntime(sublift_worker)
endif()
