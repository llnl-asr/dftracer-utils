# Fail when a lambda with a by-value capture sits inside a co_await expression
# under src/ and include/. GCC 12.1 destroys such a lambda temporary twice, so
# the lambda must be a named local that is awaited after it. A capture list may
# hold only `&`, `&name` and `this`. The expression runs from the co_await to
# the `;` at its own depth. Comments, string literals and the bodies of lambdas
# inside the expression are ignored; a co_await in such a body is its own
# expression. An expression may span lines.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_coro_lambdas.cmake
#        cmake -DSOURCE_DIR=<repo root> -DSELF_TEST=ON -P check_coro_lambdas.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "check_coro_lambdas.cmake requires -DSOURCE_DIR=...")
endif()

find_program(_awk awk REQUIRED)

set(_program [=[
function is_id(c) { return c != "" && index("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", c) > 0 }

function check_capture(k,    parts, m, j, item) {
  m = split(cap[k], parts, ",")
  for (j = 1; j <= m; j++) {
    item = parts[j]
    gsub(/[ \t]/, "", item)
    if (item == "" || item == "&" || item == "this" || item ~ /^&[A-Za-z_][A-Za-z_0-9]*$/) continue
    print FILENAME ":" capline[k] ": lambda with a by-value capture in a co_await"
    hits++
    return
  }
}

function feed(k, c) {
  if (st[k] == 1) {
    if (c == "]") { check_capture(k); st[k] = 2 } else cap[k] = cap[k] c
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
  if (c == "[" && lambda_start) { st[k] = 1; cap[k] = ""; capline[k] = FNR; return }
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
    st[nscan] = 0; d[nscan] = 0; dead[nscan] = 0
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

set(_awk_file "${CMAKE_CURRENT_BINARY_DIR}/check_coro_lambdas.awk")
file(WRITE "${_awk_file}" "${_program}")

if(SELF_TEST)
  set(_dir "${SOURCE_DIR}/tests/cmake/coro_lambdas")
  file(GLOB _cases "${_dir}/*.cpp")
  list(SORT _cases)
  set(_failed 0)
  set(_seen 0)
  foreach(_case ${_cases})
    get_filename_component(_name "${_case}" NAME)
    execute_process(
      COMMAND ${_awk} -f "${_awk_file}" "${_case}"
      RESULT_VARIABLE _rc
      OUTPUT_VARIABLE _out
      ERROR_VARIABLE _err)
    if(_err)
      message(FATAL_ERROR "coro_lambdas_self: awk failed: ${_err}")
    endif()
    string(REGEX MATCHALL "by-value capture" _found "${_out}")
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
      message(FATAL_ERROR "coro_lambdas_self: case ${_name} must start with fail or pass")
    endif()
    math(EXPR _seen "${_seen} + 1")
  endforeach()
  if(_seen LESS 2)
    message(FATAL_ERROR "coro_lambdas_self: missing cases under ${_dir}")
  endif()
  if(_failed)
    message(FATAL_ERROR "coro_lambdas_self: failed")
  endif()
  message(STATUS "coro_lambdas_self: ${_seen} cases as expected")
  return()
endif()

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
  message(FATAL_ERROR "coro_lambdas: no file with co_await under ${SOURCE_DIR}")
endif()

execute_process(
  COMMAND ${_awk} -f "${_awk_file}" ${_candidates}
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)

if(_err)
  message(FATAL_ERROR "coro_lambdas: awk failed: ${_err}")
endif()

if(_rc)
  string(REPLACE "${SOURCE_DIR}/" "" _out "${_out}")
  string(STRIP "${_out}" _out)
  string(REGEX MATCHALL "[^\n]+" _lines "${_out}")
  list(LENGTH _lines _n)
  message("${_out}")
  message(FATAL_ERROR "coro_lambdas: ${_n} lambda with a by-value capture in a co_await. "
                      "Name the lambda in its own statement and await it after.")
endif()

message(STATUS "coro_lambdas: ${_count} files with co_await, no by-value lambda in one")
