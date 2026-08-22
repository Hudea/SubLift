add_library(sublift_models STATIC
  model_bundle.cpp
  resource_locator.cpp
  native_manifest.cpp
  resource_installer.cpp
)
add_library(sublift::models ALIAS sublift_models)

target_include_directories(sublift_models
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_models PUBLIC sublift_core)
target_link_libraries(sublift_models PRIVATE nlohmann_json::nlohmann_json)

set(_sublift_native_manifest "${PROJECT_SOURCE_DIR}/../resources/manifest.json")
file(TO_CMAKE_PATH "${_sublift_native_manifest}" _sublift_native_manifest)
string(REPLACE "\\" "\\\\" _sublift_native_manifest_esc "${_sublift_native_manifest}")
target_compile_definitions(sublift_models PUBLIC
  "SUBLIFT_NATIVE_MANIFEST_PATH=\"${_sublift_native_manifest_esc}\""
)

set(_sublift_manifest_share "${CMAKE_BINARY_DIR}/share/sublift")
file(MAKE_DIRECTORY "${_sublift_manifest_share}")
configure_file(
  "${PROJECT_SOURCE_DIR}/../resources/manifest.json"
  "${_sublift_manifest_share}/manifest.json"
  COPYONLY
)
