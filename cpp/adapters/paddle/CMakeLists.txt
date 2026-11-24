find_package(ONNXRuntime REQUIRED)

add_library(sublift_adapter_paddle STATIC
  src/hash.cpp
  src/model_bundle.cpp
  src/native_manifest.cpp
  src/ocr_execution.cpp
  src/paddle_geometry.cpp
  src/paddle_ocr.cpp
  src/ppocr_crop_cls.cpp
  src/ppocr_db_postprocess.cpp
  src/ppocr_det_preprocess.cpp
  src/resource_locator.cpp
)

set_source_files_properties(src/hash.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O2>"
)

target_include_directories(sublift_adapter_paddle
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    ${OpenCV_INCLUDE_DIRS}
    ${ONNXRuntime_INCLUDE_DIRS}
    ${ONNXRuntime_INCLUDE_DIRS}/onnxruntime
)

target_compile_definitions(sublift_adapter_paddle
  PUBLIC
    SUBLIFT_HAS_PADDLE=1
  PRIVATE
    SUBLIFT_HAS_OPENCV=1
    SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(sublift_adapter_paddle
  PUBLIC
    sublift_ports
    sublift_core
  PRIVATE
    ${OpenCV_LIBS}
    ONNXRuntime::ONNXRuntime
    nlohmann_json::nlohmann_json
)

add_library(sublift::adapter_paddle ALIAS sublift_adapter_paddle)
