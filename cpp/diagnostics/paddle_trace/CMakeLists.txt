if(SUBLIFT_ENABLE_PADDLE)
  add_executable(sublift_paddle_trace paddle_trace_main.cpp)
  target_link_libraries(sublift_paddle_trace PRIVATE
    sublift_paddle
    sublift_test_support
    nlohmann_json::nlohmann_json
  )
  if(COMMAND sublift_use_bundled_onnxruntime)
    sublift_use_bundled_onnxruntime(sublift_paddle_trace)
  endif()
endif()
