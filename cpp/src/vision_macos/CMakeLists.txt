set(_vision_srcs vision_geometry.cpp)

if(SUBLIFT_ENABLE_VISION)
  list(APPEND _vision_srcs vision_availability.mm vision_ocr.mm)
else()
  list(APPEND _vision_srcs vision_stub.cpp)
endif()

add_library(sublift_vision_macos STATIC ${_vision_srcs})
add_library(sublift::vision_macos ALIAS sublift_vision_macos)

target_link_libraries(sublift_vision_macos PUBLIC sublift_core)

if(SUBLIFT_ENABLE_VISION)
  target_compile_definitions(sublift_vision_macos PUBLIC SUBLIFT_ENABLE_VISION=1)
  # ObjC++ TUs: ARC for Vision/Foundation objects. CG* refs stay CF-owned via unique_ptr.
  target_compile_options(sublift_vision_macos PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sublift_vision_macos PRIVATE
    "-framework Vision"
    "-framework CoreGraphics"
    "-framework Foundation"
  )
else()
  target_compile_definitions(sublift_vision_macos PUBLIC SUBLIFT_ENABLE_VISION=0)
endif()
