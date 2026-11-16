# SPDX-License-Identifier: MIT
# Fail when a braced temporary `Name{` or `Name<...>{` starts an argument (right
# after `(` or `,`) inside a co_await expression under src/, include/, tests/
# and benchmarks/. GCC 12.1 destroys such a temporary twice, so the value must
# be a named local, or the whole operation a named local that is awaited after
# it. The expression runs from the co_await to the `;` at its own depth.
# Comments, string literals and the bodies of lambdas inside the expression are
# ignored; a co_await in such a body is its own expression. An expression may
# span lines. A template argument list `Name<...>` counts as part of the name; a `<` that
# turns out to be a comparison does not hide a later braced temporary.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_coro_temp_args.cmake
#        cmake -DSOURCE_DIR=<repo root> -DSELF_TEST=ON -P check_coro_temp_args.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "check_coro_temp_args.cmake requires -DSOURCE_DIR=...")
endif()

find_program(_awk awk REQUIRED)

set(_program [=[
function is_id(c) { return c != "" && index("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", c) > 0 }

function report(k) {
  print FILENAME ":" FNR ": braced temporary argument in a co_await"
  hits++
  dead[k] = 1
}

# A `<` after a name may open a template argument list or be a comparison, so
# ttrack scans it on its own while track goes on as if it were a comparison. The
# scan ends without a hit at a `;`, `{`, `}` or an unmatched `)` before its `>`
# closes.
function ttrack(k, c) {
  if (tang[k] > 0) {
    if (c == "<") tang[k]++
    else if (c == ">") { if (--tang[k] == 0) tclosed[k] = 1 }
    else if (c == "(") tpar[k]++
    else if (c == ")") { if (--tpar[k] < 0) tang[k] = 0 }
    else if (c == ";" || c == "{" || c == "}") tang[k] = 0
    return
  }
  if (tclosed[k]) {
    if (c == " " || c == "\t") return
    tclosed[k] = 0
    if (c == "{") report(k)
  }
}

# as: 0 none, 1 at an argument start, 2 inside `Name`.
function track(k, c) {
  ttrack(k, c)
  if (dead[k]) return
  if (as[k] == 2) {
    if (is_id(c) || c == ":" || c == " " || c == "\t") return
    if (c == "<") { tang[k] = 1; tpar[k] = 0; tclosed[k] = 0; as[k] = 0; return }
    if (c == "{") report(k)
    as[k] = (c == "(" || c == ",") ? 1 : 0
    return
  }
  if (c == "(" || c == ",") { as[k] = 1; return }
  if (as[k] == 1) {
    if (c == " " || c == "\t") return
    if (is_id(c) && c !~ /[0-9]/ || c == ":") { as[k] = 2; return }
    as[k] = 0
  }
}

function feed(k, c) {
  if (st[k] == 1) {
    if (c == "]") st[k] = 2
    return
  }
  if (st[k] == 2) {
    if (c == "{") { st[k] = 3; bd[k] = 1 }
    return
  }
  if (st[k] == 3) {
    if (c == "{") bd[k]++
    else if (c == "}" && --bd[k] == 0) st[k] = 0
    return
  }
  if (c == "[" && lambda_start) { st[k] = 1; as[k] = 0; return }
  track(k, c)
  if (dead[k]) return
  if (c == "(" || c == "[" || c == "{") d[k]++
  else if (c == ")" || c == "]" || c == "}") { if (--d[k] < 0) dead[k] = 1 }
  else if (c == ";" && d[k] == 0) dead[k] = 1
}

function feed_all(c,    k, live) {
  lambda_start = (prev == "(" || prev == "," || prev == "=" || prev == "{" || \
                  prev == "?" || prev == ":" || prev == "return" || prev == "co_await")
  live = 0
  for (k = 1; k <= nscan; k++) {
    if (dead[k]) continue
    feed(k, c)
    if (!dead[k]) live = 1
  }
  if (!live) nscan = 0
}

function word_end(w) {
  prev = w
  if (w == "co_await") {
    nscan++
    st[nscan] = 0; d[nscan] = 0; dead[nscan] = 0; as[nscan] = 0; tang[nscan] = 0; tclosed[nscan] = 0
  }
}

FNR == 1 { in_block = 0; nscan = 0; prev = "" }

{
  line = $0 " "
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
        d2 = substr(line, i, 1)
        if (d2 == "\\") { i += 2; continue }
        if (d2 == q) break
        i++
      }
      i++
      feed_all("\"")
      prev = q
      continue
    }
    if (is_id(c)) { word = word c; feed_all(c); i++; continue }
    if (word != "") { word_end(word); word = "" }
    feed_all(c)
    if (c != " " && c != "\t") prev = c
    i++
  }
  if (word != "") word_end(word)
}

END { exit hits > 0 ? 1 : 0 }
]=])


if(SELF_TEST)
  set(_dir "${SOURCE_DIR}/tests/cmake/coro_temp_args")
  file(GLOB _cases "${_dir}/*.cpp")
  list(SORT _cases)
  set(_failed 0)
  set(_seen 0)
  foreach(_case ${_cases})
    get_filename_component(_name "${_case}" NAME)
    execute_process(
      COMMAND ${_awk} "${_program}" "${_case}"
      RESULT_VARIABLE _rc
      OUTPUT_VARIABLE _out
      ERROR_VARIABLE _err)
    if(_err)
      message(FATAL_ERROR "coro_temp_args_self: awk failed: ${_err}")
    endif()
    string(REGEX MATCHALL "braced temporary argument" _found "${_out}")
    list(LENGTH _found _n)
    if(_name MATCHES "^fail")
      if(NOT _rc OR NOT _n EQUAL 1)
        message(SEND_ERROR "${_name}: expected exactly 1 hit, got ${_n}")
        set(_failed 1)
      endif()
    elseif(_name MATCHES "^pass")
      if(_rc OR _n)
        message(SEND_ERROR "${_name}: expected no hit, got ${_n}\n${_out}")
        set(_failed 1)
      endif()
    else()
      message(FATAL_ERROR "coro_temp_args_self: case ${_name} must start with fail or pass")
    endif()
    math(EXPR _seen "${_seen} + 1")
  endforeach()
  if(_seen LESS 2)
    message(FATAL_ERROR "coro_temp_args_self: missing cases under ${_dir}")
  endif()
  if(_failed)
    message(FATAL_ERROR "coro_temp_args_self: failed")
  endif()
  message(STATUS "coro_temp_args_self: ${_seen} cases as expected")
  return()
endif()

file(GLOB_RECURSE _files
     "${SOURCE_DIR}/src/*.h" "${SOURCE_DIR}/src/*.cpp" "${SOURCE_DIR}/src/*.inc"
     "${SOURCE_DIR}/include/*.h" "${SOURCE_DIR}/include/*.hpp"
     "${SOURCE_DIR}/tests/*.h" "${SOURCE_DIR}/tests/*.cpp" "${SOURCE_DIR}/tests/*.inc"
     "${SOURCE_DIR}/benchmarks/*.h" "${SOURCE_DIR}/benchmarks/*.cpp")
# Vendored third-party text is not ours to check, and the self-test samples
# hold bad code on purpose.
list(FILTER _files EXCLUDE REGEX "/dftracer/utils/dstd/vendor/|/tests/cmake/")

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
  message(FATAL_ERROR "coro_temp_args: no file with co_await under ${SOURCE_DIR}")
endif()

execute_process(
  COMMAND ${_awk} "${_program}" ${_candidates}
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)

if(_err)
  message(FATAL_ERROR "coro_temp_args: awk failed: ${_err}")
endif()

if(_rc)
  string(REPLACE "${SOURCE_DIR}/" "" _out "${_out}")
  string(STRIP "${_out}" _out)
  string(REGEX MATCHALL "[^\n]+" _lines "${_out}")
  list(LENGTH _lines _n)
  message("${_out}")
  message(FATAL_ERROR "coro_temp_args: ${_n} braced temporary argument in a co_await. "
                      "Name the temporary, or the whole operation, in its own statement and await it after.")
endif()

message(STATUS "coro_temp_args: ${_count} files with co_await, no braced temporary argument in one")
