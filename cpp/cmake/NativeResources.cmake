# Native resource helpers: reject Python-packaged ORT and pin SHA-256.

function(sublift_library_is_python_onnxruntime lib_path out_var)
  set(_is_python FALSE)
  if("${lib_path}" MATCHES "/\\.venv/" OR
     "${lib_path}" MATCHES "site-packages" OR
     "${lib_path}" MATCHES "/onnxruntime/capi/")
    set(_is_python TRUE)
  endif()
  set(${out_var} ${_is_python} PARENT_SCOPE)
endfunction()

function(sublift_ort_sha256_allowed lib_path manifest_path out_var)
  set(_allowed FALSE)
  if(NOT EXISTS "${lib_path}" OR NOT EXISTS "${manifest_path}")
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  # macOS 自带 shasum，多数 Linux 发行版只有 sha256sum；两者输出格式一致。
  find_program(SUBLIFT_SHA256_TOOL NAMES shasum sha256sum)
  if(NOT SUBLIFT_SHA256_TOOL)
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  if("${SUBLIFT_SHA256_TOOL}" MATCHES "shasum$")
    set(_sha_cmd "${SUBLIFT_SHA256_TOOL}" -a 256 "${lib_path}")
  else()
    set(_sha_cmd "${SUBLIFT_SHA256_TOOL}" "${lib_path}")
  endif()
  execute_process(
    COMMAND ${_sha_cmd}
    OUTPUT_VARIABLE _sha_line
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _sha_rc)
  if(NOT _sha_rc EQUAL 0)
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  string(REGEX REPLACE " .*$" "" _digest "${_sha_line}")
  file(READ "${manifest_path}" _manifest_json)
  string(JSON _art_len ERROR_VARIABLE _json_err LENGTH "${_manifest_json}" onnxruntime)
  if(_json_err)
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  math(EXPR _last "${_art_len} - 1")
  foreach(_i RANGE 0 ${_last})
    string(JSON _art_sha GET "${_manifest_json}" onnxruntime ${_i} sha256)
    if(_art_sha STREQUAL _digest)
      set(_allowed TRUE)
    endif()
  endforeach()
  set(${out_var} ${_allowed} PARENT_SCOPE)
endfunction()
