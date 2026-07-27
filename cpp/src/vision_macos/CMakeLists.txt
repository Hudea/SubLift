add_library(sublift_vision_macos STATIC vision_stub.mm)
add_library(sublift::vision_macos ALIAS sublift_vision_macos)

target_link_libraries(sublift_vision_macos PUBLIC sublift_core)

# Real Vision.framework linkage arrives with feat-064xx.
