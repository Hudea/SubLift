if(NOT OpenCV_FOUND)
  find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
endif()

add_executable(worker_unit_test
  worker_unit_test.cpp
)

target_link_libraries(worker_unit_test
  PRIVATE
    Catch2::Catch2WithMain
    sublift_worker_lib
    sublift_adapter_mock
)

add_test(NAME worker_unit_test COMMAND worker_unit_test)
set_tests_properties(worker_unit_test PROPERTIES
  LABELS "unit"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(worker_adapter_test
  worker_adapter_test.cpp
)

target_include_directories(worker_adapter_test
  PRIVATE
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(worker_adapter_test
  PRIVATE
    Catch2::Catch2WithMain
    sublift_adapter_ffmpeg
    nlohmann_json::nlohmann_json
    ${OpenCV_LIBS}
)

target_compile_definitions(worker_adapter_test
  PRIVATE
    SUBLIFT_WORKER_EXECUTABLE="$<TARGET_FILE:sublift_worker>"
)

add_test(NAME worker_adapter_test COMMAND worker_adapter_test)
set_tests_properties(worker_adapter_test PROPERTIES
  LABELS "adapter"
  TIMEOUT 60
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
