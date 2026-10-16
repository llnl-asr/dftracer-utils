# Fail when a co_await sits inside the parentheses of an if, while, for or
# switch header under src/ and include/. GCC 12 lays out a coroutine frame
# differently in the ramp and the actor when a temporary lives in such a
# condition across the co_await, so the result must go through a named local.
# A header may span lines. Comments, string literals and the bodies of lambdas
# inside a header are ignored.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_coro_conditions.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "check_coro_conditions.cmake requires -DSOURCE_DIR=...")
endif()

find_program(_awk awk REQUIRED)

set(_program [=[
function is_id(c) { return c != "" && index("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", c) > 0 }

function word_end(w) {
  if (depth > 0) {
    if (w == "co_await" && braces == 0) { print FILENAME ":" FNR ": co_await in a condition"; hits++ }
    return
  }
  if (w == "if" || w == "while" || w == "for" || w == "switch") pending = 1
  else if (w != "constexpr") pending = 0
}

FNR == 1 { in_block = 0; depth = 0; pending = 0; braces = 0 }

{
  line = $0
  n = length(line)
  word = ""
  i = 1
  while (i <= n) {
    c = substr(line, i, 1)
    two = substr(line, i, 2)
    if (in_block) {
      if (two == "*/") { in_block = 0; i += 2 } else i++
      continue
    }
    if (two == "/*") { in_block = 1; i += 2; continue }
    if (two == "//") break
    if (c == "\"" || c == "'") {
      if (word != "") { word_end(word); word = "" }
      q = c
      i++
      while (i <= n) {
        d = substr(line, i, 1)
        if (d == "\\") { i += 2; continue }
        if (d == q) break
        i++
      }
      i++
      pending = 0
      continue
    }
    if (is_id(c)) { word = word c; i++; continue }
    if (word != "") { word_end(word); word = "" }
    if (c == "(") {
      if (depth > 0) depth++
      else if (pending) { depth = 1; pending = 0 }
    } else if (c == ")") {
      if (depth > 0) depth--
    } else if (c == "{" && depth > 0) {
      braces++
    } else if (c == "}" && braces > 0) {
      braces--
    } else if (c != " " && c != "\t") {
      pending = 0
    }
    i++
  }
  if (word != "") word_end(word)
}

END { exit hits > 0 ? 1 : 0 }
]=])

set(_awk_file "${CMAKE_CURRENT_BINARY_DIR}/check_coro_conditions.awk")
file(WRITE "${_awk_file}" "${_program}")

file(GLOB_RECURSE _files
     "${SOURCE_DIR}/src/*.h" "${SOURCE_DIR}/src/*.cpp" "${SOURCE_DIR}/src/*.inc"
     "${SOURCE_DIR}/include/*.h" "${SOURCE_DIR}/include/*.hpp")

set(_candidates "")
foreach(_file ${_files})
  file(STRINGS "${_file}" _hit REGEX "co_await" LIMIT_COUNT 1)
  if(_hit)
    list(APPEND _candidates "${_file}")
  endif()
endforeach()
list(SORT _candidates)

list(LENGTH _candidates _count)
if(_count EQUAL 0)
  message(FATAL_ERROR "coro_conditions: no file with co_await under ${SOURCE_DIR}")
endif()

execute_process(
  COMMAND ${_awk} -f "${_awk_file}" ${_candidates}
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)

if(_err)
  message(FATAL_ERROR "coro_conditions: awk failed: ${_err}")
endif()

if(_rc)
  string(REPLACE "${SOURCE_DIR}/" "" _out "${_out}")
  string(STRIP "${_out}" _out)
  string(REGEX MATCHALL "[^\n]+" _lines "${_out}")
  list(LENGTH _lines _n)
  message("${_out}")
  message(FATAL_ERROR "coro_conditions: ${_n} co_await in a condition. "
                      "Move each into a named local before the statement.")
endif()

message(STATUS "coro_conditions: ${_count} files with co_await, none in a condition")
