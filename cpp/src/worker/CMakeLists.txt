set(_sublift_ipc_sources
  framing.cpp
  framing.hpp
  protocol.cpp
  protocol.hpp
  engine_factory.cpp
  engine_factory.hpp
  bridge.hpp
  connection.cpp
  connection.hpp
)

# The Worker is a product pipeline only when the OpenCV-backed signature
# stages are present.  Keep a small protocol-only implementation available
# without it so that build diagnostics (including ASan) do not inherit the
# OpenCV/TBB runtime merely to execute `--version` or a handshake.
if(SUBLIFT_ENABLE_OPENCV)
  list(APPEND _sublift_ipc_sources bridge.cpp)
else()
  list(APPEND _sublift_ipc_sources bridge_no_opencv.cpp)
endif()

add_library(sublift_ipc STATIC ${_sublift_ipc_sources})

target_include_directories(sublift_ipc PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sublift_ipc PUBLIC
  sublift_core
  sublift_ffmpeg
  sublift_vision_macos
  sublift_paddle
  nlohmann_json::nlohmann_json
)

if(SUBLIFT_ENABLE_OPENCV)
  target_compile_definitions(sublift_ipc PUBLIC SUBLIFT_HAS_OPENCV=1)
else()
  target_compile_definitions(sublift_ipc PUBLIC SUBLIFT_HAS_OPENCV=0)
endif()

if(APPLE)
  target_link_libraries(sublift_ipc PUBLIC
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework ImageIO"
  )
endif()

add_executable(sublift_worker main.cpp)

target_link_libraries(sublift_worker PRIVATE
  sublift_ipc
)
if(COMMAND sublift_use_bundled_onnxruntime)
  sublift_use_bundled_onnxruntime(sublift_worker)
endif()
