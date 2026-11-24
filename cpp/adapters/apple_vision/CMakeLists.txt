set(_vision_srcs
  src/vision_geometry.cpp
)

if(SUBLIFT_ENABLE_VISION)
  list(APPEND _vision_srcs
    src/vision_availability.mm
    src/vision_ocr.mm
  )
else()
  list(APPEND _vision_srcs
    src/vision_stub.cpp
  )
endif()

add_library(sublift_adapter_apple_vision STATIC ${_vision_srcs})
add_library(sublift::adapter_apple_vision ALIAS sublift_adapter_apple_vision)

target_include_directories(sublift_adapter_apple_vision
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_link_libraries(sublift_adapter_apple_vision
  PUBLIC
    sublift_ports
    sublift_core
)

if(SUBLIFT_ENABLE_VISION)
  target_compile_definitions(sublift_adapter_apple_vision
    PUBLIC
      SUBLIFT_ENABLE_VISION=1
  )
  # ObjC++ TUs use ARC for Vision/Foundation objects; CG* refs remain CF-owned via RAII.
  target_compile_options(sublift_adapter_apple_vision
    PRIVATE
      $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sublift_adapter_apple_vision
    PRIVATE
      "-framework Vision"
      "-framework CoreGraphics"
      "-framework Foundation"
  )
else()
  target_compile_definitions(sublift_adapter_apple_vision
    PUBLIC
      SUBLIFT_ENABLE_VISION=0
  )
endif()
