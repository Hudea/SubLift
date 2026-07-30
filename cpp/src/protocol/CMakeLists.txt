add_library(sublift_protocol STATIC
  framing.cpp
  protocol.cpp
  ${PROJECT_SOURCE_DIR}/include/sublift/protocol/framing.hpp
  ${PROJECT_SOURCE_DIR}/include/sublift/protocol/protocol.hpp
)
add_library(sublift::protocol ALIAS sublift_protocol)

target_include_directories(sublift_protocol PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(sublift_protocol PUBLIC
  sublift_core
)
target_link_libraries(sublift_protocol PRIVATE nlohmann_json::nlohmann_json)
