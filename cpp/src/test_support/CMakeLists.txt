add_library(sublift_test_support STATIC
  test_support.cpp
  parity_config.cpp
  signature_golden.cpp
  changepoint_golden.cpp
  pipeline_golden.cpp
  extractor_golden.cpp
  vision_golden.cpp
  sha256.cpp
  unicode_nfc.cpp
)
add_library(sublift::test_support ALIAS sublift_test_support)

target_link_libraries(sublift_test_support
  PUBLIC
    sublift_pipeline
    sublift_core
    nlohmann_json::nlohmann_json
)

if(APPLE)
  target_link_libraries(sublift_test_support PRIVATE "-framework CoreFoundation")
endif()
