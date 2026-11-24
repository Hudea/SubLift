add_library(sublift_adapter_mock STATIC
  src/mock_frame_source.cpp
  src/mock_ocr.cpp
  src/mock_subtitle_sink.cpp
)

target_include_directories(sublift_adapter_mock
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_adapter_mock
  PUBLIC
    sublift_ports
    sublift_core
)

add_library(sublift::adapter_mock ALIAS sublift_adapter_mock)
