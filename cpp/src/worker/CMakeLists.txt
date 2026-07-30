add_library(sublift_protocol STATIC
  framing.cpp
  framing.hpp
  protocol.cpp
  protocol.hpp
)

target_include_directories(sublift_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sublift_protocol PUBLIC
  sublift_core
  nlohmann_json::nlohmann_json
)

set(_sublift_app_sources bridge.hpp)
if(SUBLIFT_ENABLE_OPENCV)
  list(APPEND _sublift_app_sources bridge.cpp)
else()
  list(APPEND _sublift_app_sources bridge_no_opencv.cpp)
endif()

add_library(sublift_application STATIC ${_sublift_app_sources})

target_include_directories(sublift_application PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sublift_application PUBLIC
  sublift_protocol
  sublift_core
  sublift_ffmpeg
  sublift_vision_macos
  sublift_paddle
  nlohmann_json::nlohmann_json
)

if(SUBLIFT_ENABLE_OPENCV)
  target_compile_definitions(sublift_application PUBLIC SUBLIFT_HAS_OPENCV=1)
else()
  target_compile_definitions(sublift_application PUBLIC SUBLIFT_HAS_OPENCV=0)
endif()

if(APPLE)
  target_link_libraries(sublift_application PUBLIC
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework ImageIO"
  )
endif()

set(_sublift_ipc_sources
  engine_factory.cpp
  engine_factory.hpp
  connection.cpp
  connection.hpp
)

add_library(sublift_ipc STATIC ${_sublift_ipc_sources})

target_include_directories(sublift_ipc PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sublift_ipc PUBLIC
  sublift_application
  sublift_protocol
)

add_executable(sublift_worker main.cpp)

target_link_libraries(sublift_worker PRIVATE
  sublift_application
  sublift_ipc
)
if(COMMAND sublift_use_bundled_onnxruntime)
  sublift_use_bundled_onnxruntime(sublift_worker)
endif()
