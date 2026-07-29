set(_sublift_test_srcs
  smoke_test.cpp
  image_test.cpp
  models_test.cpp
  config_test.cpp
  sha256_test.cpp
  timeline_test.cpp
  dedupe_test.cpp
  line_select_test.cpp
  ocr_test.cpp
  extractor_pure_test.cpp
  extractor_probe_test.cpp
  extractor_full_test.cpp
  extractor_roi_test.cpp
  vision_geometry_test.cpp
  paddle_geometry_test.cpp
  paddle_stub_test.cpp
  paddle_ctc_test.cpp
  paddle_db_test.cpp
  paddle_models_test.cpp
  worker_framing_test.cpp

  worker_protocol_test.cpp
  worker_path_mode_test.cpp
  worker_cancel_test.cpp
  parity/config_parity_test.cpp
  parity/timeline_parity_test.cpp
  parity/dedupe_parity_test.cpp
  parity/line_select_parity_test.cpp
  parity/extractor_parity_test.cpp
  parity/vision_parity_test.cpp
  parity/paddle_parity_test.cpp
  parity/paddle_stages_parity_test.cpp
)
if(SUBLIFT_ENABLE_OPENCV)
  list(APPEND _sublift_test_srcs
    signature_test.cpp
    changepoint_test.cpp
    pipeline_feed_test.cpp
    pipeline_ocr_test.cpp
    pipeline_finalize_test.cpp
    parity/signature_parity_test.cpp
    parity/changepoint_parity_test.cpp
    parity/pipeline_parity_test.cpp
  )
endif()

if(SUBLIFT_ENABLE_VISION)
  list(APPEND _sublift_test_srcs
    vision_ocr_test.mm
  )
endif()

add_executable(sublift_tests ${_sublift_test_srcs})

# Helper: turn a repo path into an escaped C++ string literal.
function(sublift_stringify_path var path)
  file(TO_CMAKE_PATH "${path}" _cmake_path)
  string(REPLACE "\\" "\\\\" _esc "${_cmake_path}")
  set(${var} "${_esc}" PARENT_SCOPE)
endfunction()

# Path to frozen config golden (repo root is parent of cpp/).
set(_config_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/config/default_config.v1.json")
sublift_stringify_path(_config_golden_esc "${_config_golden}")

set(_tl_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/segments/timeline.v1.json")
set(_dd_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/entries/dedupe.v1.json")
set(_ls_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/line_select/line_select.v1.json")
set(_ext_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/extractor/extractor.v1.json")
set(_vis_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/vision/vision.v1.json")
set(_pad_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/paddle/paddle.v1.json")
set(_pad_stages_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/paddle/paddle_stages.v1.json")
sublift_stringify_path(_tl_golden_esc "${_tl_golden}")
sublift_stringify_path(_dd_golden_esc "${_dd_golden}")
sublift_stringify_path(_ls_golden_esc "${_ls_golden}")
sublift_stringify_path(_ext_golden_esc "${_ext_golden}")
sublift_stringify_path(_vis_golden_esc "${_vis_golden}")
sublift_stringify_path(_pad_golden_esc "${_pad_golden}")
sublift_stringify_path(_pad_stages_golden_esc "${_pad_stages_golden}")

target_compile_definitions(sublift_tests PRIVATE
  "SUBLIFT_PARITY_GOLDEN_CONFIG=\"${_config_golden_esc}\""
  "SUBLIFT_PARITY_GOLDEN_TIMELINE=\"${_tl_golden_esc}\""
  "SUBLIFT_PARITY_GOLDEN_DEDUPE=\"${_dd_golden_esc}\""
  "SUBLIFT_PARITY_GOLDEN_LINE_SELECT=\"${_ls_golden_esc}\""
  "SUBLIFT_PARITY_GOLDEN_EXTRACTOR=\"${_ext_golden_esc}\""
  "SUBLIFT_PARITY_GOLDEN_VISION=\"${_vis_golden_esc}\""
  "SUBLIFT_PARITY_GOLDEN_PADDLE=\"${_pad_golden_esc}\""
  "SUBLIFT_PARITY_GOLDEN_PADDLE_STAGES=\"${_pad_stages_golden_esc}\""
)

if(SUBLIFT_ENABLE_OPENCV)
  set(_sig_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/signature/signature.v1.json")
  set(_sig_fixtures "${PROJECT_SOURCE_DIR}/../benchmark/parity/fixtures/signature")
  set(_cp_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/events/changepoint.v1.json")
  set(_cp_fixtures "${PROJECT_SOURCE_DIR}/../benchmark/parity/fixtures/changepoint")
  set(_pl_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/pipeline/pipeline.v1.json")
  sublift_stringify_path(_sig_golden_esc "${_sig_golden}")
  sublift_stringify_path(_sig_fixtures_esc "${_sig_fixtures}")
  sublift_stringify_path(_cp_golden_esc "${_cp_golden}")
  sublift_stringify_path(_cp_fixtures_esc "${_cp_fixtures}")
  sublift_stringify_path(_pl_golden_esc "${_pl_golden}")
  target_compile_definitions(sublift_tests PRIVATE
    "SUBLIFT_PARITY_GOLDEN_SIGNATURE=\"${_sig_golden_esc}\""
    "SUBLIFT_PARITY_FIXTURE_SIGNATURE=\"${_sig_fixtures_esc}\""
    "SUBLIFT_PARITY_GOLDEN_CHANGEPOINT=\"${_cp_golden_esc}\""
    "SUBLIFT_PARITY_FIXTURE_CHANGEPOINT=\"${_cp_fixtures_esc}\""
    "SUBLIFT_PARITY_GOLDEN_PIPELINE=\"${_pl_golden_esc}\""
  )
endif()

target_link_libraries(sublift_tests
  PRIVATE
    sublift_test_support
    sublift_ipc
    sublift_ffmpeg
    sublift_vision_macos
    sublift_paddle_geometry
    sublift_paddle
    Catch2::Catch2WithMain
)

if(SUBLIFT_ENABLE_VISION)
  target_link_libraries(sublift_tests PRIVATE "-framework CoreText")
  # Match sublift_vision_macos: ARC for ObjC++ test TUs (vision_ocr_test.mm).
  target_compile_options(sublift_tests PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
endif()

include(Catch)
catch_discover_tests(sublift_tests)
