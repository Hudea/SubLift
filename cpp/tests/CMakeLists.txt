# Test Support Library (Golden Loaders & Parity Helpers)
add_library(test_support INTERFACE)
target_include_directories(test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(test_support INTERFACE nlohmann_json::nlohmann_json)

# =============================================================================
# Tier: unit (Fast in-memory & mock unit tests, no real FFmpeg/ORT/video)
# =============================================================================
add_executable(core_test
  image_test.cpp
  models_test.cpp
)
target_link_libraries(core_test PRIVATE Catch2::Catch2WithMain sublift_ports sublift_core)
add_test(NAME core_test COMMAND core_test)
set_tests_properties(core_test PROPERTIES
  LABELS "unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(features_test
  features_test.cpp
  adversarial_stress_test.cpp
)
target_link_libraries(features_test PRIVATE Catch2::Catch2WithMain sublift_features sublift_core)
target_compile_definitions(features_test PRIVATE SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME features_test COMMAND features_test)
set_tests_properties(features_test PROPERTIES
  LABELS "unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(timing_test
  timing_test.cpp
  timing_adversarial_stress_test.cpp
  timeline_adversarial_stress_test.cpp
)
target_link_libraries(timing_test PRIVATE Catch2::Catch2WithMain sublift_timing sublift_features sublift_core)
target_compile_definitions(timing_test PRIVATE SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME timing_test COMMAND timing_test)
set_tests_properties(timing_test PROPERTIES
  LABELS "unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(line_select_test
  line_select_test.cpp
)
target_link_libraries(line_select_test PRIVATE Catch2::Catch2WithMain sublift_text sublift_core)
add_test(NAME line_select_test COMMAND line_select_test)
set_tests_properties(line_select_test PROPERTIES
  LABELS "unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(dedupe_test
  dedupe_test.cpp
  text_adversarial_test.cpp
)
target_link_libraries(dedupe_test PRIVATE Catch2::Catch2WithMain sublift_text sublift_core)
target_compile_definitions(dedupe_test PRIVATE SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME dedupe_test COMMAND dedupe_test)
set_tests_properties(dedupe_test PROPERTIES
  LABELS "unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(region_cluster_test
  region_cluster_test.cpp
  region_cluster_adversarial_test.cpp
)
target_link_libraries(region_cluster_test PRIVATE Catch2::Catch2WithMain sublift_text sublift_core)
add_test(NAME region_cluster_test COMMAND region_cluster_test)
set_tests_properties(region_cluster_test PROPERTIES
  LABELS "unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(mock_adapters_test
  mock_adapters_test.cpp
)
target_link_libraries(mock_adapters_test PRIVATE Catch2::Catch2WithMain sublift_adapter_mock sublift_ports sublift_core)
add_test(NAME mock_adapters_test COMMAND mock_adapters_test)
set_tests_properties(mock_adapters_test PROPERTIES
  LABELS "unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(engine_test
  engine_test.cpp
  auto_region_test.cpp
  adversarial/engine_adversarial_test.cpp
)
target_link_libraries(engine_test PRIVATE Catch2::Catch2WithMain sublift_engine sublift_adapter_mock sublift_timing sublift_text sublift_core)
target_compile_definitions(engine_test PRIVATE SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME engine_test COMMAND engine_test)
set_tests_properties(engine_test PROPERTIES
  LABELS "unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(pipeline_integration_test
  integration/pipeline_integration_test.cpp
)
target_link_libraries(pipeline_integration_test PRIVATE Catch2::Catch2WithMain sublift_engine sublift_adapter_mock sublift_timing sublift_text sublift_core)
add_test(NAME pipeline_integration_test COMMAND pipeline_integration_test)
set_tests_properties(pipeline_integration_test PROPERTIES
  LABELS "unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

if (TARGET sublift_adapter_ffmpeg)
  set(FFMPEG_TEST_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/cpp/adapters/ffmpeg/src")

  add_executable(ffmpeg_unit_test
    extractor_pure_test.cpp
    roi_conversion_test.cpp
  )
  target_include_directories(ffmpeg_unit_test PRIVATE ${FFMPEG_TEST_INCLUDE_DIR})
  target_link_libraries(ffmpeg_unit_test PRIVATE Catch2::Catch2WithMain sublift_adapter_ffmpeg test_support)
  target_compile_definitions(ffmpeg_unit_test PRIVATE
    SUBLIFT_CONTRACT_ROI="${CMAKE_SOURCE_DIR}/benchmark/parity/fixtures/roi/roi-cases.json"
  )
  add_test(NAME ffmpeg_unit_test COMMAND ffmpeg_unit_test)
  set_tests_properties(ffmpeg_unit_test PROPERTIES
    LABELS "unit"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
endif()

if (TARGET sublift_adapter_paddle)
  set(PADDLE_TEST_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/cpp/adapters/paddle/src")

  add_executable(paddle_unit_test
    ocr_execution_test.cpp
    paddle_crop_cls_test.cpp
    paddle_ctc_test.cpp
    paddle_db_test.cpp
    paddle_det_preprocess_test.cpp
    paddle_geometry_test.cpp
    paddle_models_test.cpp
  )
  target_include_directories(paddle_unit_test PRIVATE ${PADDLE_TEST_INCLUDE_DIR} ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(paddle_unit_test PRIVATE Catch2::Catch2WithMain sublift_adapter_paddle ${OpenCV_LIBS} test_support)
  add_test(NAME paddle_unit_test COMMAND paddle_unit_test)
  set_tests_properties(paddle_unit_test PROPERTIES
    LABELS "unit"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
endif()

if (TARGET sublift_adapter_apple_vision)
  add_executable(apple_vision_unit_test
    apple_vision_geometry_test.cpp
  )
  target_link_libraries(apple_vision_unit_test PRIVATE
    Catch2::Catch2WithMain
    sublift_adapter_apple_vision
  )
  add_test(NAME apple_vision_unit_test COMMAND apple_vision_unit_test)
  set_tests_properties(apple_vision_unit_test PROPERTIES
    LABELS "unit"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
endif()

# =============================================================================
# Tier: parity (Golden JSON contract parity tests)
# =============================================================================
add_executable(signature_parity_test signature_parity_test.cpp)
target_link_libraries(signature_parity_test PRIVATE Catch2::Catch2WithMain sublift_features test_support)
target_compile_definitions(signature_parity_test PRIVATE SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME signature_parity_test COMMAND signature_parity_test)
set_tests_properties(signature_parity_test PROPERTIES
  LABELS "parity"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(changepoint_parity_test changepoint_parity_test.cpp)
target_link_libraries(changepoint_parity_test PRIVATE Catch2::Catch2WithMain sublift_timing test_support)
target_compile_definitions(changepoint_parity_test PRIVATE SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME changepoint_parity_test COMMAND changepoint_parity_test)
set_tests_properties(changepoint_parity_test PROPERTIES
  LABELS "parity"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(timeline_parity_test timeline_parity_test.cpp)
target_link_libraries(timeline_parity_test PRIVATE Catch2::Catch2WithMain sublift_timing test_support)
target_compile_definitions(timeline_parity_test PRIVATE SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME timeline_parity_test COMMAND timeline_parity_test)
set_tests_properties(timeline_parity_test PROPERTIES
  LABELS "parity"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(line_select_parity_test line_select_parity_test.cpp)
target_link_libraries(line_select_parity_test PRIVATE Catch2::Catch2WithMain sublift_text test_support)
target_compile_definitions(line_select_parity_test PRIVATE SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME line_select_parity_test COMMAND line_select_parity_test)
set_tests_properties(line_select_parity_test PROPERTIES
  LABELS "parity"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(dedupe_parity_test dedupe_parity_test.cpp)
target_link_libraries(dedupe_parity_test PRIVATE Catch2::Catch2WithMain sublift_text test_support)
target_compile_definitions(dedupe_parity_test PRIVATE SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME dedupe_parity_test COMMAND dedupe_parity_test)
set_tests_properties(dedupe_parity_test PROPERTIES
  LABELS "parity"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(pipeline_parity_test parity/pipeline_parity_test.cpp)
target_link_libraries(pipeline_parity_test PRIVATE Catch2::Catch2WithMain sublift_engine sublift_adapter_mock sublift_timing sublift_text sublift_core test_support)
target_compile_definitions(pipeline_parity_test PRIVATE SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME pipeline_parity_test COMMAND pipeline_parity_test)
set_tests_properties(pipeline_parity_test PROPERTIES
  LABELS "parity"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# =============================================================================
# Tier: parity;adapter & adapter (Real FFmpeg / ONNX Runtime adapter tests)
# =============================================================================
if (TARGET sublift_adapter_ffmpeg)
  add_executable(extractor_parity_test extractor_parity_test.cpp)
  target_include_directories(extractor_parity_test PRIVATE ${FFMPEG_TEST_INCLUDE_DIR})
  target_link_libraries(extractor_parity_test PRIVATE Catch2::Catch2WithMain sublift_adapter_ffmpeg test_support)
  target_compile_definitions(extractor_parity_test PRIVATE
    SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    SUBLIFT_PARITY_GOLDEN_EXTRACTOR="${CMAKE_SOURCE_DIR}/benchmark/parity/goldens/extractor/extractor.v1.json"
  )
  add_test(NAME extractor_parity_test COMMAND extractor_parity_test)
  set_tests_properties(extractor_parity_test PROPERTIES
    LABELS "parity;adapter"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )

  add_executable(ffmpeg_adapter_test
    extractor_probe_test.cpp
    extractor_roi_test.cpp
    extractor_full_test.cpp
    media_info_test.cpp
  )
  target_include_directories(ffmpeg_adapter_test PRIVATE ${FFMPEG_TEST_INCLUDE_DIR})
  target_link_libraries(ffmpeg_adapter_test PRIVATE Catch2::Catch2WithMain sublift_adapter_ffmpeg test_support)
  add_test(NAME ffmpeg_adapter_test COMMAND ffmpeg_adapter_test)
  set_tests_properties(ffmpeg_adapter_test PROPERTIES
    LABELS "adapter"
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
endif()

if (TARGET sublift_adapter_paddle)
  add_executable(paddle_parity_test parity/paddle_parity_test.cpp)
  target_include_directories(paddle_parity_test PRIVATE ${PADDLE_TEST_INCLUDE_DIR} ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(paddle_parity_test PRIVATE Catch2::Catch2WithMain sublift_adapter_paddle ${OpenCV_LIBS} test_support)
  target_compile_definitions(paddle_parity_test PRIVATE
    SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    SUBLIFT_PARITY_PADDLE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/benchmark/parity/fixtures/paddle"
    SUBLIFT_PARITY_PADDLE_MANIFEST="${CMAKE_SOURCE_DIR}/benchmark/parity/fixtures/paddle/manifest.json"
    SUBLIFT_PARITY_GOLDEN_PADDLE="${CMAKE_SOURCE_DIR}/benchmark/parity/goldens/paddle/paddle.v1.json"
  )
  add_test(NAME paddle_parity_test COMMAND paddle_parity_test)
  set_tests_properties(paddle_parity_test PROPERTIES
    LABELS "parity;adapter"
    TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )

  add_executable(paddle_adapter_test
    paddle_probe_cache_test.cpp
    resource_locator_test.cpp
  )
  target_include_directories(paddle_adapter_test PRIVATE ${PADDLE_TEST_INCLUDE_DIR})
  target_link_libraries(paddle_adapter_test PRIVATE Catch2::Catch2WithMain sublift_adapter_paddle test_support)
  target_compile_definitions(paddle_adapter_test PRIVATE SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME paddle_adapter_test COMMAND paddle_adapter_test)
  set_tests_properties(paddle_adapter_test PROPERTIES
    LABELS "adapter"
    TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
endif()

if (TARGET sublift_adapter_apple_vision AND SUBLIFT_ENABLE_VISION)
  add_executable(apple_vision_adapter_test
    apple_vision_ocr_test.mm
  )
  target_compile_options(apple_vision_adapter_test PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_include_directories(apple_vision_adapter_test PRIVATE
    ${OpenCV_INCLUDE_DIRS}
  )
  target_link_libraries(apple_vision_adapter_test PRIVATE
    Catch2::Catch2WithMain
    sublift_adapter_apple_vision
    ${OpenCV_LIBS}
    "-framework CoreText"
    "-framework CoreGraphics"
    "-framework Foundation"
    test_support
  )
  target_compile_definitions(apple_vision_adapter_test PRIVATE
    SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    SUBLIFT_PARITY_PADDLE_MANIFEST="${CMAKE_SOURCE_DIR}/benchmark/parity/fixtures/paddle/manifest.json"
  )
  add_test(NAME apple_vision_adapter_test COMMAND apple_vision_adapter_test)
  set_tests_properties(apple_vision_adapter_test PROPERTIES
    LABELS "adapter"
    TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
endif()

# =============================================================================
# Tier: e2e (Short-clip real video + FFmpeg + Paddle end-to-end smoke test)
# =============================================================================
if (TARGET sublift_adapter_ffmpeg AND TARGET sublift_adapter_paddle)
  add_executable(pipeline_e2e_smoke_test integration/pipeline_e2e_smoke_test.cpp)
  target_include_directories(pipeline_e2e_smoke_test PRIVATE ${FFMPEG_TEST_INCLUDE_DIR})
  target_link_libraries(pipeline_e2e_smoke_test PRIVATE
    Catch2::Catch2WithMain
    sublift_engine
    sublift_adapter_ffmpeg
    sublift_adapter_paddle
    sublift_adapter_mock
    sublift_text
    sublift_ports
    sublift_core
    test_support
  )
  target_compile_definitions(pipeline_e2e_smoke_test PRIVATE
    SUBLIFT_PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    SUBLIFT_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
  )
  add_test(NAME pipeline_e2e_smoke_test COMMAND pipeline_e2e_smoke_test)
  set_tests_properties(pipeline_e2e_smoke_test PROPERTIES
    LABELS "e2e"
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
endif()
