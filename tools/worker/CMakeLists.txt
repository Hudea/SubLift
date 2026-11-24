if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json 3.10 REQUIRED)
endif()

add_library(sublift_worker_lib STATIC
  image_io.cpp
  worker_app.cpp
)

target_include_directories(sublift_worker_lib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(sublift_worker_lib
  PUBLIC
    sublift_engine
    sublift_text
    sublift_timing
    sublift_ports
    sublift_core
    sublift_adapter_ffmpeg
    sublift_adapter_paddle
    sublift_adapter_apple_vision
    nlohmann_json::nlohmann_json
)

if(SUBLIFT_ENABLE_VISION)
  target_compile_definitions(sublift_worker_lib PUBLIC SUBLIFT_HAS_APPLE_VISION=1)
else()
  target_compile_definitions(sublift_worker_lib PUBLIC SUBLIFT_HAS_APPLE_VISION=0)
endif()

add_library(sublift::worker_lib ALIAS sublift_worker_lib)

add_executable(sublift_worker
  main.cpp
)

target_link_libraries(sublift_worker
  PRIVATE
    sublift_worker_lib
)

add_executable(sublift::worker ALIAS sublift_worker)

add_subdirectory(tests)
