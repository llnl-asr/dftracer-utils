# SPDX-License-Identifier: MIT
# Fails on a bare fixed-width or size type (uint64_t, int32_t, size_t,
# uintptr_t, ...) in C++ code, which must name it std::uint64_t and so on.
# Comments, strings and preprocessor lines are not checked. Headers that plain
# C also compiles (the plugin ABI, the C APIs, the ABI test consumers, the
# shared test utilities) and
# vendored files keep the C names.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -DWORK_DIR=<dir> -P check_std_int_types.cmake
#        cmake -DSOURCE_DIR=<repo root> -DWORK_DIR=<dir> -DSELF_TEST=ON -P check_std_int_types.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "check_std_int_types.cmake requires -DSOURCE_DIR=...")
endif()
if(NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "check_std_int_types.cmake requires -DWORK_DIR=...")
endif()

find_program(_awk awk REQUIRED)

set(_program [=[
FILENAME != lastfile { lastfile = FILENAME; inblock = 0 }
function strip(s,    p1, p2, rest, q) {
  gsub(/"([^"\\]|\\.)*"/, "\"\"", s)
  gsub(/'([^'\\]|\\.)'/, "' '", s)
  if (inblock) {
    q = index(s, "*/")
    if (q == 0) return ""
    s = substr(s, q + 2)
    inblock = 0
  }
  for (;;) {
    p1 = index(s, "/*")
    p2 = index(s, "//")
    if (p2 && (!p1 || p2 < p1)) return substr(s, 1, p2 - 1)
    if (!p1) return s
    rest = substr(s, p1 + 2)
    q = index(rest, "*/")
    if (q) { s = substr(s, 1, p1 - 1) " " substr(rest, q + 2); continue }
    inblock = 1
    return substr(s, 1, p1 - 1)
  }
}
{
  s = strip($0)
  if (s ~ /^[ \t]*#/) next
  t = " " s " "
  if (t ~ /[^:_A-Za-z0-9](u?int(8|16|32|64)_t|size_t|u?intptr_t)[^_A-Za-z0-9]/) {
    print FILENAME ":" FNR ": " $0
    hits++
  }
}
END { exit hits > 0 ? 1 : 0 }
]=])

# std_int_types_run(<work dir> <out var> <rc var> <files...>)
function(std_int_types_run work out_var rc_var)
  file(MAKE_DIRECTORY "${work}")
  set(_awk_file "${work}/check_std_int_types.awk")
  file(WRITE "${_awk_file}" "${_program}")
  execute_process(
    COMMAND ${_awk} -f "${_awk_file}" ${ARGN}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(_err)
    message(FATAL_ERROR "std_int_types: awk failed: ${_err}")
  endif()
  set(${out_var} "${_out}" PARENT_SCOPE)
  set(${rc_var} ${_rc} PARENT_SCOPE)
endfunction()

if(SELF_TEST)
  set(_root "${WORK_DIR}/std_int_types_self")
  file(REMOVE_RECURSE "${_root}")
  file(MAKE_DIRECTORY "${_root}")
  set(_file "${_root}/a.cpp")
  file(WRITE "${_file}"
       "std::uint64_t a = 0;\n"
       "std::size_t b = sizeof(std::int32_t);\n"
       "// uint64_t c;\n"
       "/* size_t d;\n"
       "   int8_t e; */\n"
       "const char* f = \"uint64_t\";\n"
       "#include <cstdint>\n"
       "my_uint64_t g; uint64_type h; __uint128_t i;\n")
  std_int_types_run("${_root}/work" _out _rc "${_file}")
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "std_int_types self test: expected a pass, got:\n${_out}")
  endif()
  file(WRITE "${_file}" "int x;\nuint64_t a = 0;\nstd::vector<size_t> v;\nauto p = (uintptr_t)q;\n")
  std_int_types_run("${_root}/work" _out _rc "${_file}")
  if(_rc EQUAL 0)
    message(FATAL_ERROR "std_int_types self test: expected a failure, got a pass")
  endif()
  foreach(_re "a.cpp:2: " "a.cpp:3: " "a.cpp:4: ")
    if(NOT _out MATCHES "${_re}")
      message(FATAL_ERROR "std_int_types self test: expected '${_re}' in:\n${_out}")
    endif()
  endforeach()
  return()
endif()

set(_files "")
foreach(_dir src include tests benchmarks examples)
  file(GLOB_RECURSE _found "${SOURCE_DIR}/${_dir}/*.h" "${SOURCE_DIR}/${_dir}/*.hpp"
       "${SOURCE_DIR}/${_dir}/*.cpp" "${SOURCE_DIR}/${_dir}/*.cc" "${SOURCE_DIR}/${_dir}/*.inl")
  list(APPEND _files ${_found})
endforeach()
list(FILTER _files EXCLUDE REGEX "/include/dftracer/utils/dstd/vendor/")
list(FILTER _files EXCLUDE REGEX "/include/dftracer/utils/plugins/abi/")
list(FILTER _files EXCLUDE REGEX "/include/dftracer/utils/plugins/(abi|prims)\\.h$")
list(FILTER _files EXCLUDE REGEX "/c_api[^/]*\\.h$")
list(FILTER _files EXCLUDE REGEX "/tests/plugins/abi_c_consumers\\.h$")
list(FILTER _files EXCLUDE REGEX "/tests/testing_utilities\\.h$")
list(FILTER _files EXCLUDE REGEX "/tests/cmake/")
list(SORT _files)
if(NOT _files)
  message(FATAL_ERROR "std_int_types: no source file found")
endif()

std_int_types_run("${WORK_DIR}" _out _rc ${_files})
if(_rc)
  string(REPLACE "${SOURCE_DIR}/" "" _out "${_out}")
  message("${_out}")
  message(FATAL_ERROR "std_int_types: name fixed-width and size types with std::, as std::uint64_t")
endif()
