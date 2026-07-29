# FindONNXRuntime.cmake
# --------------------
# Find ONNX Runtime (onnxruntime_cxx_api.h and libonnxruntime)
#
# Output variables:
#   ONNXRuntime_FOUND        - True if ONNX Runtime was found
#   ONNXRuntime_INCLUDE_DIRS - Include directories for ONNX Runtime
#   ONNXRuntime_LIBRARIES    - Libraries to link against
#   ONNXRuntime_VERSION      - Detected version string (optional)

if(APPLE)
  execute_process(
    COMMAND brew --prefix onnxruntime
    OUTPUT_VARIABLE _brew_ort_prefix
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _brew_ort_rc)
  if(_brew_ort_rc EQUAL 0 AND _brew_ort_prefix)
    list(APPEND _ort_search_paths "${_brew_ort_prefix}")
  endif()
endif()

find_path(ONNXRuntime_INCLUDE_DIR
  NAMES onnxruntime_cxx_api.h onnxruntime_c_api.h
  PATHS
    ${_ort_search_paths}
    ENV ONNXRUNTIME_ROOTDIR
    ENV ONNXRUNTIME_DIR
    /usr/local
    /usr
    /opt/homebrew
  PATH_SUFFIXES include include/onnxruntime
)

find_library(ONNXRuntime_LIBRARY
  NAMES onnxruntime
  PATHS
    ${_ort_search_paths}
    ENV ONNXRUNTIME_ROOTDIR
    ENV ONNXRUNTIME_DIR
    /usr/local
    /usr
    /opt/homebrew
  PATH_SUFFIXES lib lib64
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ONNXRuntime
  REQUIRED_VARS ONNXRuntime_LIBRARY ONNXRuntime_INCLUDE_DIR
)

if(ONNXRuntime_FOUND)
  set(ONNXRuntime_INCLUDE_DIRS ${ONNXRuntime_INCLUDE_DIR})
  set(ONNXRuntime_LIBRARIES ${ONNXRuntime_LIBRARY})

  if(NOT TARGET ONNXRuntime::ONNXRuntime)
    add_library(ONNXRuntime::ONNXRuntime UNKNOWN IMPORTED)
    set_target_properties(ONNXRuntime::ONNXRuntime PROPERTIES
      IMPORTED_LOCATION "${ONNXRuntime_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${ONNXRuntime_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(ONNXRuntime_INCLUDE_DIR ONNXRuntime_LIBRARY)
