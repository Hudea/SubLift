include(FetchContent)

# Find or fetch Catch2 v3.7.1
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
  )
  FetchContent_MakeAvailable(Catch2)
endif()

# Find OpenCV (required for sublift_features private implementation and test fixture loading)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

# Find nlohmann_json (required for test support, ffmpeg adapter metadata probe and paddle manifest)
find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()
