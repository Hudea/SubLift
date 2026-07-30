set(_sublift_ipc_sources
  engine_factory.cpp
  engine_factory.hpp
  connection.cpp
  connection.hpp
)

add_library(sublift_ipc STATIC ${_sublift_ipc_sources})

target_include_directories(sublift_ipc PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_ipc PUBLIC
  sublift_application
  sublift_protocol
)

add_executable(sublift_worker main.cpp)

target_link_libraries(sublift_worker PRIVATE
  sublift_application
  sublift_ipc
)
if(COMMAND sublift_use_bundled_onnxruntime)
  sublift_use_bundled_onnxruntime(sublift_worker)
endif()
