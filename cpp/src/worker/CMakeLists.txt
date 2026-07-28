add_library(sublift_ipc STATIC
  framing.cpp
  framing.hpp
  protocol.cpp
  protocol.hpp
  engine_factory.cpp
  engine_factory.hpp
  bridge.cpp
  bridge.hpp
  connection.cpp
  connection.hpp
)

target_include_directories(sublift_ipc PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sublift_ipc PUBLIC
  sublift_core
  sublift_ffmpeg
  sublift_vision_macos
  nlohmann_json::nlohmann_json
)

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
