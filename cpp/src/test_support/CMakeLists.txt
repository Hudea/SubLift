add_library(sublift_test_support STATIC
  test_support.cpp
  parity_config.cpp
)
add_library(sublift::test_support ALIAS sublift_test_support)

target_link_libraries(sublift_test_support
  PUBLIC
    sublift_core
    nlohmann_json::nlohmann_json
)
