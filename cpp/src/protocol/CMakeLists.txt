add_library(sublift_protocol STATIC
  framing.cpp
  framing.hpp
  protocol.cpp
  protocol.hpp
)
add_library(sublift::protocol ALIAS sublift_protocol)

target_include_directories(sublift_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_protocol PUBLIC
  sublift_core
  nlohmann_json::nlohmann_json
)
