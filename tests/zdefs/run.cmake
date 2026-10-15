# Configures and builds tests/zdefs, whose shared library calls an undefined
# function. Passes only when configure succeeds and the link fails with an
# undefined-symbol error.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -DWORK_DIR=<dir> -DCXX=<compiler>
#              -DGENERATOR=<generator> -P run.cmake

if(APPLE)
  message(STATUS "z_defs: skipped on Apple, ld64 rejects undefined dylib symbols")
  return()
endif()

foreach(_var SOURCE_DIR WORK_DIR CXX GENERATOR)
  if(NOT DEFINED ${_var})
    message(FATAL_ERROR "run.cmake requires -D${_var}=...")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/zdefs" -B "${WORK_DIR}" -G
          "${GENERATOR}" -DCMAKE_CXX_COMPILER=${CXX}
          -DZDEFS_ROOT=${SOURCE_DIR}
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "z_defs: configure failed:\n${_out}\n${_err}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${WORK_DIR}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)
if(_rc EQUAL 0)
  message(FATAL_ERROR "z_defs: the build succeeded, -z defs is not applied")
endif()
if(NOT "${_out}\n${_err}" MATCHES "undefined (reference|symbol)")
  message(FATAL_ERROR "z_defs: the build failed without an undefined-symbol error:\n${_out}\n${_err}")
endif()
message(STATUS "z_defs: the link failed with an undefined-symbol error")
