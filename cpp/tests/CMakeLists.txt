add_executable(sublift_tests
  smoke_test.cpp
  image_test.cpp
  models_test.cpp
  config_test.cpp
  parity/config_parity_test.cpp
)

# Path to frozen config golden (repo root is parent of cpp/).
set(SUBLIFT_PARITY_GOLDEN_CONFIG
    "${PROJECT_SOURCE_DIR}/../benchmark/parity/goldens/config/default_config.v1.json")
# Escape for stringify in C++
file(TO_CMAKE_PATH "${SUBLIFT_PARITY_GOLDEN_CONFIG}" _parity_golden_cmake)
string(REPLACE "\\" "\\\\" _parity_golden_esc "${_parity_golden_cmake}")

target_compile_definitions(sublift_tests PRIVATE
  "SUBLIFT_PARITY_GOLDEN_CONFIG=\"${_parity_golden_esc}\""
)

target_link_libraries(sublift_tests
  PRIVATE
    sublift_test_support
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(sublift_tests)
