# Dependency pins. OpenCV and ffmpeg remain system deps (not FetchContent).
#
# nlohmann/json: always (core).
# Catch2 v3: only when SUBLIFT_BUILD_TESTS is ON (set before include).

include(FetchContent)

set(SUBLIFT_CATCH2_TAG "v3.7.1" CACHE STRING "Catch2 git tag")
set(SUBLIFT_NLOHMANN_JSON_TAG "v3.11.3" CACHE STRING "nlohmann/json git tag")

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        ${SUBLIFT_NLOHMANN_JSON_TAG}
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(nlohmann_json)

if(SUBLIFT_BUILD_TESTS)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        ${SUBLIFT_CATCH2_TAG}
    GIT_SHALLOW    TRUE
  )
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(Catch2)
  # catch_discover_tests helpers (Catch2 v3)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()
