# cpp/third_party/CMakeLists.txt
find_package(Threads REQUIRED)

add_library(sublift_httplib INTERFACE)
add_library(sublift::httplib ALIAS sublift_httplib)

target_include_directories(sublift_httplib INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp-httplib>
  $<INSTALL_INTERFACE:include>
)

target_compile_definitions(sublift_httplib INTERFACE
  CPPHTTPLIB_NO_OPENSSL
  CPPHTTPLIB_NO_ZLIB
  CPPHTTPLIB_NO_BROTLI
)

target_link_libraries(sublift_httplib INTERFACE
  Threads::Threads
)
