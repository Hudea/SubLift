set(_sublift_test_srcs
  smoke_test.cpp
  image_test.cpp
  models_test.cpp
  config_test.cpp
  sha256_test.cpp
  timeline_test.cpp
  dedupe_test.cpp
  line_select_test.cpp
  parity/config_parity_test.cpp
  parity/timeline_parity_test.cpp
  parity/dedupe_parity_test.cpp
  parity/line_select_parity_test.cpp
)
if(SUBLIFT_ENABLE_OPENCV)
  list(APPEND _sublift_test_srcs
    signature_test.cpp
    changepoint_test.cpp
    parity/signature_parity_test.cpp
    parity/changepoint_parity_test.cpp
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
sublift_stringify_path(_tl_golden_esc "${_tl_golden}")
sublift_stringify_path(_dd_golden_esc "${_dd_golden}")
sublift_stringify_path(_ls_golden_esc "${_ls_golden}")

target_compile_definitions(sublift_tests PRIVATE
  "SUBLIFT_PARITY_GOLDEN_CONFIG=\"${_config_golden_esc}\""
  "SUBLIFT_PARITY_GOLDEN_TIMELINE=\"${_tl_golden_esc}\""
  "SUBLIFT_PARITY_GOLDEN_DEDUPE=\"${_dd_golden_esc}\""
  "SUBLIFT_PARITY_GOLDEN_LINE_SELECT=\"${_ls_golden_esc}\""
)

if(SUBLIFT_ENABLE_OPENCV)
  set(_sig_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/signature/signature.v1.json")
  set(_sig_fixtures "${PROJECT_SOURCE_DIR}/../benchmark/parity/fixtures/signature")
  set(_cp_golden "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/events/changepoint.v1.json")
  set(_cp_fixtures "${PROJECT_SOURCE_DIR}/../benchmark/parity/fixtures/changepoint")
  sublift_stringify_path(_sig_golden_esc "${_sig_golden}")
  sublift_stringify_path(_sig_fixtures_esc "${_sig_fixtures}")
  sublift_stringify_path(_cp_golden_esc "${_cp_golden}")
  sublift_stringify_path(_cp_fixtures_esc "${_cp_fixtures}")
  target_compile_definitions(sublift_tests PRIVATE
    "SUBLIFT_PARITY_GOLDEN_SIGNATURE=\"${_sig_golden_esc}\""
    "SUBLIFT_PARITY_FIXTURE_SIGNATURE=\"${_sig_fixtures_esc}\""
    "SUBLIFT_PARITY_GOLDEN_CHANGEPOINT=\"${_cp_golden_esc}\""
    "SUBLIFT_PARITY_FIXTURE_CHANGEPOINT=\"${_cp_fixtures_esc}\""
  )
endif()

target_link_libraries(sublift_tests
  PRIVATE
    sublift_test_support
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(sublift_tests)
