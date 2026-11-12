# SPDX-License-Identifier: MIT
# Check that every source of a library sits in that library's folder: a file
# compiled into dftracer_utils_<name>_objects must be under
# src/dftracer/utils/<name>/.
#
# Usage: cmake -DCOMPILE_COMMANDS=<build>/compile_commands.json -P check_library_folders.cmake

if(NOT DEFINED COMPILE_COMMANDS)
  message(FATAL_ERROR "check_library_folders.cmake requires -DCOMPILE_COMMANDS=...")
endif()

file(READ "${COMPILE_COMMANDS}" json)
string(JSON count LENGTH "${json}")
math(EXPR last "${count} - 1")
set(errors "")
set(checked 0)
foreach(i RANGE ${last})
  string(JSON file GET "${json}" ${i} file)
  string(JSON command ERROR_VARIABLE no_command GET "${json}" ${i} command)
  if(no_command)
    string(JSON command GET "${json}" ${i} output)
  endif()
  if(NOT command MATCHES "dftracer_utils_([a-z]+)_objects\\.dir")
    continue()
  endif()
  set(lib "${CMAKE_MATCH_1}")
  if(NOT file MATCHES "/src/dftracer/utils/([a-z_]+)/")
    continue()
  endif()
  set(folder "${CMAKE_MATCH_1}")
  math(EXPR checked "${checked} + 1")
  if(NOT folder STREQUAL lib)
    string(REGEX REPLACE "^.*/src/" "src/" rel "${file}")
    string(APPEND errors "  ${rel} is compiled into ${lib} but sits in ${folder}/\n")
  endif()
endforeach()

if(checked EQUAL 0)
  message(FATAL_ERROR "library folders: no library source found in ${COMPILE_COMMANDS}")
endif()
if(errors)
  message(FATAL_ERROR "library sources outside their folder:\n${errors}")
endif()
message(STATUS "library folders ok (${checked} sources)")
