set(_sublift_app_sources bridge.hpp)
if(SUBLIFT_ENABLE_OPENCV)
  list(APPEND _sublift_app_sources bridge.cpp)
else()
  list(APPEND _sublift_app_sources bridge_no_opencv.cpp)
endif()

add_library(sublift_application STATIC ${_sublift_app_sources})
add_library(sublift::application ALIAS sublift_application)

target_include_directories(sublift_application PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src/worker
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_application PUBLIC
  sublift_pipeline
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
