# SPDX-License-Identifier: MIT
# Check that a build with DFTRACER_UTILS_SINGLE_LIBRARY links exactly one
# library of ours, libdftracer_utils, into the Python extension and into a CLI
# tool. A library per area in the list means the split libraries leaked in.
# The extension may also link libdftracer_utils_arrow, which stays its own
# library in this mode.
#
# Usage: cmake -DEXTENSION=<file> -DTOOL=<file> -P check_single_library.cmake

cmake_minimum_required(VERSION 3.20)

foreach(_var EXTENSION TOOL)
  if(NOT DEFINED ${_var})
    message(FATAL_ERROR "check_single_library.cmake requires -D${_var}=...")
  endif()
endforeach()

set(_extra_extension_libraries dftracer_utils_arrow)

if(APPLE)
  find_program(_reader otool REQUIRED)
  set(_reader_args -L)
else()
  find_program(_reader readelf REQUIRED)
  set(_reader_args -d)
endif()

set(_failed FALSE)
foreach(_file "${EXTENSION}" "${TOOL}")
  execute_process(
    COMMAND ${_reader} ${_reader_args} "${_file}"
    RESULT_VARIABLE _status
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error)
  if(NOT _status EQUAL 0)
    message(FATAL_ERROR "${_reader} failed on ${_file}: ${_error}")
  endif()

  string(REGEX MATCHALL "lib(dftracer_utils[A-Za-z0-9_]*)\\." _entries
               "${_output}")
  set(_names "")
  foreach(_entry ${_entries})
    string(REGEX REPLACE "^lib" "" _name "${_entry}")
    string(REGEX REPLACE "\\.$" "" _name "${_name}")
    list(APPEND _names "${_name}")
  endforeach()
  list(REMOVE_DUPLICATES _names)

  set(_want dftracer_utils)
  if(_file STREQUAL EXTENSION)
    list(APPEND _want ${_extra_extension_libraries})
  endif()
  list(SORT _want)
  list(SORT _names)

  if(NOT "${_names}" STREQUAL "${_want}")
    message(SEND_ERROR "${_file} links: '${_names}', want '${_want}'")
    set(_failed TRUE)
  else()
    message(STATUS "${_file} links only ${_want}")
  endif()
endforeach()

if(_failed)
  message(FATAL_ERROR "single_library: split libraries are linked")
endif()
