add_library(sublift_paddle_geometry STATIC paddle_geometry.cpp)
add_library(sublift::paddle_geometry ALIAS sublift_paddle_geometry)

target_include_directories(sublift_paddle_geometry
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_paddle_geometry PUBLIC sublift_core)

add_library(sublift_paddle STATIC
  paddle_ocr.cpp
  paddle_models.cpp
  ppocr_det_preprocess.cpp
  ppocr_db_postprocess.cpp
  ppocr_crop_cls.cpp
)
add_library(sublift::paddle ALIAS sublift_paddle)

target_include_directories(sublift_paddle
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_paddle
  PUBLIC
    sublift_core
    sublift_paddle_geometry
)

if(SUBLIFT_ENABLE_OPENCV AND OpenCV_FOUND)
  target_compile_definitions(sublift_paddle PRIVATE SUBLIFT_HAS_OPENCV=1)
  target_link_libraries(sublift_paddle PRIVATE ${OpenCV_LIBS})
  target_include_directories(sublift_paddle PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()

if(SUBLIFT_ENABLE_PADDLE AND ONNXRuntime_FOUND)
  if(NOT SUBLIFT_ENABLE_OPENCV OR NOT OpenCV_FOUND)
    message(FATAL_ERROR
      "SUBLIFT_ENABLE_PADDLE=ON requires OpenCV for exact PP-OCR Det/crop parity")
  endif()
  target_compile_definitions(sublift_paddle PUBLIC SUBLIFT_HAS_PADDLE=1)
  target_link_libraries(sublift_paddle PRIVATE ONNXRuntime::ONNXRuntime)
endif()

# Explicit diagnostic utility for Phase 6.8 stage parity. It is not part of
# the product CLI/Worker path and only captures tensors when invoked directly.
add_executable(sublift_paddle_trace paddle_trace_main.cpp)
target_link_libraries(sublift_paddle_trace PRIVATE
  sublift_paddle
  sublift_test_support
  nlohmann_json::nlohmann_json
)
if(COMMAND sublift_use_bundled_onnxruntime)
  sublift_use_bundled_onnxruntime(sublift_paddle_trace)
endif()
