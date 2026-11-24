cmake_minimum_required(VERSION 3.20)
project(SubLift VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

enable_testing()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_subdirectory(cpp)
add_subdirectory(tools)

# -----------------------------------------------------------------------------
# Layered Architecture Dependency Checker & Tests
# -----------------------------------------------------------------------------

add_custom_target(check_dependencies
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_dependencies.py
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Checking layered architecture dependency rules..."
)

add_test(NAME test_dependency_rules
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_dependencies.py
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(NAME test_dependency_negative_cases
  COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/scripts -p "test_*.py"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

set_tests_properties(test_dependency_rules test_dependency_negative_cases
  PROPERTIES LABELS "unit"
)
