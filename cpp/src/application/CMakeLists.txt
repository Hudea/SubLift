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
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# Application depends on pipeline + protocol + ports (headers). Concrete FFmpeg /
# OCR adapters are injected by the Worker composition root.
target_link_libraries(sublift_application PUBLIC
  sublift_pipeline
  sublift_protocol
  sublift_core
)

if(SUBLIFT_ENABLE_OPENCV)
  target_compile_definitions(sublift_application PUBLIC SUBLIFT_HAS_OPENCV=1)
  # bridge.cpp 的 Frame-mode JPEG 解码在非 Apple 平台走 OpenCV（cv::imdecode）。
  find_package(OpenCV QUIET COMPONENTS core imgproc imgcodecs)
  if(OpenCV_FOUND)
    target_link_libraries(sublift_application PRIVATE ${OpenCV_LIBS})
    target_include_directories(sublift_application PRIVATE ${OpenCV_INCLUDE_DIRS})
  endif()
else()
  target_compile_definitions(sublift_application PUBLIC SUBLIFT_HAS_OPENCV=0)
endif()

# Frame-mode JPEG decode (IPC payload) uses ImageIO on Apple — PRIVATE only.
if(APPLE)
  target_link_libraries(sublift_application PRIVATE
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework ImageIO"
  )
endif()
