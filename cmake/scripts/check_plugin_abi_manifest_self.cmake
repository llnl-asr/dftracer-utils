# SPDX-License-Identifier: MIT
# Runs the plugin ABI manifest generator on the small fixture header in
# tests/cmake/plugin_abi_manifest/, compiles what it writes, runs it and
# compares against the recorded lines there. A parser that drops a field, a
# union arm, an enumerator, a constant or a function pointer fails here.
#
# Usage: cmake -DPYTHON=<python3> -DC_COMPILER=<cc> -DSOURCE_DIR=<repo>
#              -DWORK_DIR=<dir> -P check_plugin_abi_manifest_self.cmake

if(NOT DEFINED PYTHON OR NOT DEFINED C_COMPILER OR NOT DEFINED SOURCE_DIR OR NOT DEFINED WORK_DIR)
  message(
    FATAL_ERROR
      "check_plugin_abi_manifest_self.cmake requires -DPYTHON=... -DC_COMPILER=... -DSOURCE_DIR=... -DWORK_DIR=..."
  )
endif()

set(fixture ${SOURCE_DIR}/tests/cmake/plugin_abi_manifest)
set(fixture_header dftracer/utils/plugin_abi_manifest_fixture.h)
set(expected_file ${fixture}/expected_manifest.txt)
file(MAKE_DIRECTORY ${WORK_DIR})
set(generated ${WORK_DIR}/plugin_abi_manifest_self.c)
set(binary ${WORK_DIR}/plugin_abi_manifest_self)

execute_process(
  COMMAND ${PYTHON} ${SOURCE_DIR}/scripts/plugin_abi_manifest.py --root
          ${fixture} --header ${fixture_header} --out ${generated}
  RESULT_VARIABLE generate_result
  OUTPUT_VARIABLE generate_output
  ERROR_VARIABLE generate_output)
if(NOT generate_result EQUAL 0)
  message(FATAL_ERROR "plugin_abi_manifest_self: the generator failed:\n${generate_output}")
endif()

execute_process(
  COMMAND ${C_COMPILER} -I ${fixture}/include -o ${binary} ${generated}
  RESULT_VARIABLE compile_result
  OUTPUT_VARIABLE compile_output
  ERROR_VARIABLE compile_output)
if(NOT compile_result EQUAL 0)
  message(FATAL_ERROR "plugin_abi_manifest_self: compiling ${generated} failed:\n${compile_output}")
endif()

execute_process(COMMAND ${binary}
                RESULT_VARIABLE run_result OUTPUT_VARIABLE actual
                ERROR_VARIABLE run_output)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "plugin_abi_manifest_self: ${binary} failed:\n${run_output}")
endif()

file(READ ${expected_file} expected)
if(actual STREQUAL expected)
  message(STATUS "plugin_abi_manifest_self: ok")
else()
  set(actual_file ${WORK_DIR}/plugin_abi_manifest_self.actual.txt)
  file(WRITE ${actual_file} "${actual}")
  find_program(DFTRACER_UTILS_DIFF diff)
  if(DFTRACER_UTILS_DIFF)
    execute_process(COMMAND ${DFTRACER_UTILS_DIFF} -u ${expected_file}
                            ${actual_file} OUTPUT_VARIABLE diff_output
                    ERROR_VARIABLE diff_output)
  else()
    set(diff_output "recorded:\n${expected}\nactual:\n${actual}")
  endif()
  message(
    FATAL_ERROR
      "plugin_abi_manifest_self: the generator lost or changed a fact from ${fixture_header} ('-' is recorded, '+' is the generator).\n"
      "${diff_output}\n"
      "Actual output kept in ${actual_file}")
endif()
