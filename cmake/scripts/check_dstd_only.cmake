# SPDX-License-Identifier: MIT
# Fails when an area already moved to dstd names a std type that dstd has (see
# include/dftracer/utils/dstd/). A line that must keep one carries a
# "// keep std: <reason>" comment, at its end or on the line above. Comments
# are not checked.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_dstd_only.cmake
#        cmake -DSOURCE_DIR=<repo root> -DSELF_TEST=ON -P check_dstd_only.cmake

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "check_dstd_only.cmake requires -DSOURCE_DIR=...")
endif()

find_program(_awk awk REQUIRED)

set(_dirs
    include/dftracer/utils/common
    src/dftracer/utils/common
    include/dftracer/utils/coro
    src/dftracer/utils/coro
    include/dftracer/utils/runtime
    src/dftracer/utils/runtime
    include/dftracer/utils/store
    src/dftracer/utils/store
    include/dftracer/utils/json
    src/dftracer/utils/json
    include/dftracer/utils/duql
    src/dftracer/utils/duql
    include/dftracer/utils/plugins
    src/dftracer/utils/plugins
    include/dftracer/utils/index
    src/dftracer/utils/index
    include/dftracer/utils/utilities
    src/dftracer/utils/utilities
    include/dftracer/utils/trace
    src/dftracer/utils/trace
    include/dftracer/utils/dataframe
    src/dftracer/utils/dataframe
    include/dftracer/utils/server
    src/dftracer/utils/server
    src/dftracer/utils/python
    src/dftracer/utils/binaries)

# The Task, TaskGraph and Pipeline trees move in a later step.
set(_skip
    include/dftracer/utils/runtime/tasks
    include/dftracer/utils/runtime/task_graph
    include/dftracer/utils/runtime/pipeline
    src/dftracer/utils/runtime/tasks
    src/dftracer/utils/runtime/task_graph
    src/dftracer/utils/runtime/pipeline
    include/dftracer/utils/dstd/vendor)

set(_program [=[
FNR == 1 { kept = 0 }
{
  line = $0
  if (line ~ /\/\/ keep std: /) { kept = (line ~ /^[ \t]*\/\//); next }
  if (kept) { kept = 0; next }
  sub(/\/\/.*$/, "", line)
  if (line ~ /(^|[^A-Za-z0-9_:])std::(string|string_view|vector|span|map|set|multimap|multiset|unordered_map|unordered_set|deque|list|queue|priority_queue|unique_ptr|make_unique|shared_ptr|make_shared|weak_ptr|optional|nullopt|variant|monostate|pair|make_pair|tuple|any|any_cast|function|thread|promise|future|shared_future|to_string|stoi|stol|stoll|stoul|stoull|stod|stof|stable_sort|ofstream|ifstream|ostringstream|stringstream|istringstream|to_chars|from_chars)([^A-Za-z0-9_]|$)/) {
    print FILENAME ":" FNR ": " $0
    hits++
  } else if (line ~ /UniquePtr<.*>[ \t]*([A-Za-z_][A-Za-z0-9_]*)?[ \t]*[({][ \t]*new[ \t]/) {
    print FILENAME ":" FNR ": " $0 "  (raw new: use dstd::make_unique)"
    hits++
  }
}
END { exit hits > 0 ? 1 : 0 }
]=])

set(_awk_file "${CMAKE_CURRENT_BINARY_DIR}/check_dstd_only.awk")
file(WRITE "${_awk_file}" "${_program}")

if(SELF_TEST)
  set(_sample "${CMAKE_CURRENT_BINARY_DIR}/check_dstd_only_sample.cpp")
  file(WRITE "${_sample}"
       "std::vector<int> a;\n"
       "dstd::Vector<int> b;\n"
       "// std::string in a comment\n"
       "std::string_view c;  // keep std: test\n"
       "std::atomic<int> d;\n"
       "// keep std: test\n"
       "std::deque<int> e;\n"
       "dstd::UniquePtr<int> f(new int(1));\n")
  execute_process(COMMAND ${_awk} -f "${_awk_file}" "${_sample}"
                  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out)
  string(REGEX REPLACE "[^\n]" "" _newlines "${_out}")
  string(LENGTH "${_newlines}" _n)
  if(NOT _rc OR NOT _n EQUAL 2 OR NOT _out MATCHES ":1: std::vector"
     OR NOT _out MATCHES ":8: dstd::UniquePtr")
    message(FATAL_ERROR "dstd_only self test: expected hits on lines 1 and 8, got:\n${_out}")
  endif()
  return()
endif()

set(_files "")
foreach(_dir IN LISTS _dirs)
  file(GLOB_RECURSE _found "${SOURCE_DIR}/${_dir}/*.h" "${SOURCE_DIR}/${_dir}/*.cpp"
       "${SOURCE_DIR}/${_dir}/*.inl")
  list(APPEND _files ${_found})
endforeach()
foreach(_dir IN LISTS _skip)
  file(GLOB_RECURSE _found "${SOURCE_DIR}/${_dir}/*.h" "${SOURCE_DIR}/${_dir}/*.cpp"
       "${SOURCE_DIR}/${_dir}/*.inl")
  if(_found)
    list(REMOVE_ITEM _files ${_found})
  endif()
endforeach()
list(SORT _files)
if(NOT _files)
  message(FATAL_ERROR "dstd_only: no file under the listed areas")
endif()

execute_process(
  COMMAND ${_awk} -f "${_awk_file}" ${_files}
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)
if(_err)
  message(FATAL_ERROR "dstd_only: awk failed: ${_err}")
endif()
if(_rc)
  string(REPLACE "${SOURCE_DIR}/" "" _out "${_out}")
  message("${_out}")
  message(FATAL_ERROR "dstd_only: std types that dstd has, in an area moved to dstd")
endif()
