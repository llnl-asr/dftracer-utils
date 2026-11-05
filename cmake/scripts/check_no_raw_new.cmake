# SPDX-License-Identifier: MIT
# Fails on a raw `new` (also `new (std::nothrow)`) or `delete[]` in src/ and
# include/, so every object of ours comes from this library's heap and is
# charged to a pool. Placement new is not raw. A type listed in
# cmake/scripts/raw_new_allow.txt may be made with `new`. Comments and strings
# are not checked.
#
# Rows of the allow file:
#   <type> | <reason>                  the type may be made with new anywhere
#   <path glob> | <reason>             the files are not checked
#   <path glob> | <type> | <reason>    the type may be made with new in the files
# The check also fails on a row that matches no file or that no site uses.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_no_raw_new.cmake
#        cmake -DSOURCE_DIR=<repo root> -DSELF_TEST=ON -P check_no_raw_new.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "check_no_raw_new.cmake requires -DSOURCE_DIR=...")
endif()
if(NOT CMAKE_CURRENT_BINARY_DIR)
  set(CMAKE_CURRENT_BINARY_DIR "${CMAKE_BINARY_DIR}")
endif()

find_program(_awk awk REQUIRED)

set(_program [=[
BEGIN { inblock = 0 }
FNR == 1 { inblock = 0 }
NR == FNR {
  n = split($0, f, "\t")
  if (f[1] == "T") typerow[f[2]] = f[3]
  else if (f[1] == "F") filetype[f[2], f[3]] = f[4]
  next
}
FILENAME != lastfile { lastfile = FILENAME; inblock = 0 }
function hit(key, text,    k) {
  if (key in typerow) { usedtype[key] = 1; return }
  k = FILENAME SUBSEP key
  if (k in filetype) { usedfile[k] = 1; return }
  print FILENAME ":" FNR ": " text "  (raw new of " key ": derive dstd::HeapObject or use dstd::make_unique)"
  hits++
}
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
function type_key(tail,    depth, i, c, t) {
  depth = 0
  t = ""
  for (i = 1; i <= length(tail); i++) {
    c = substr(tail, i, 1)
    if (c == "<") depth++
    else if (c == ">") { if (depth == 0) break; depth-- }
    else if (depth == 0 && (c == "(" || c == "{" || c == "[" || c == ";" || c == "," || c == ")" || c == "=")) break
    t = t c
  }
  while (t ~ /<[^<>]*>/) sub(/<[^<>]*>/, "", t)
  gsub(/[ \t]+$/, "", t)
  sub(/^(const|struct|class)[ \t]+/, "", t)
  gsub(/.*::/, "", t)
  return t
}
{
  s = strip($0)
  if (s ~ /delete[ \t]*\[[ \t]*\]/ && s !~ /operator[ \t]+delete[ \t]*\[/) hit("delete[]", $0)
  off = 1
  while ((i = index(substr(s, off), "new")) > 0) {
    a = off + i - 1
    off = a + 3
    before = a > 1 ? substr(s, a - 1, 1) : " "
    after = substr(s, a + 3, 1)
    if (before ~ /[A-Za-z0-9_]/ || after ~ /[A-Za-z0-9_]/) continue
    if (substr(s, 1, a - 1) ~ /operator[ \t]*$/) continue
    tail = substr(s, a + 3)
    sub(/^[ \t]+/, "", tail)
    if (tail ~ /^\(/) {
      if (tail ~ /^\([ \t]*(std::)?nothrow[ \t]*\)/) sub(/^\([^)]*\)[ \t]*/, "", tail)
      else continue
    }
    if (tail == "") { if (substr(s, 1, a - 1) ~ /<$/) continue; hit("?", $0); continue }
    if (tail !~ /^[A-Za-z_:]/) continue
    hit(type_key(tail), $0)
  }
}
END {
  for (key in typerow)
    if (!(key in usedtype)) {
      print "stale allow row, no site makes " key " with new: " typerow[key]
      hits++
    }
  for (k in filetype)
    if (!(k in usedfile)) {
      split(k, p, SUBSEP)
      print "stale allow row, " p[1] " does not make " p[2] " with new: " filetype[k]
      hits++
    }
  exit hits > 0 ? 1 : 0
}
]=])

# no_raw_new_run(<root> <allow file> <work dir> <out var> <rc var> <files...>)
function(no_raw_new_run root allow work out_var rc_var)
  file(MAKE_DIRECTORY "${work}")
  set(_awk_file "${work}/check_no_raw_new.awk")
  file(WRITE "${_awk_file}" "${_program}")
  set(_files ${ARGN})
  set(_problems "")
  set(_lines "#")
  file(READ "${allow}" _text)
  string(REGEX REPLACE "[;\\[\\]]" " " _text "${_text}")
  string(REPLACE "\n" ";" _rows "${_text}")
  foreach(_r IN LISTS _rows)
    string(STRIP "${_r}" _r)
    if(_r STREQUAL "" OR _r MATCHES "^#")
      continue()
    endif()
    string(REPLACE "|" ";" _parts "${_r}")
    list(LENGTH _parts _n)
    list(GET _parts 0 _key)
    string(STRIP "${_key}" _key)
    if(_n EQUAL 2 AND NOT _key MATCHES "[/.]")
      string(APPEND _lines "\nT\t${_key}\t${_r}")
    elseif(_n EQUAL 2 OR _n EQUAL 3)
      if(_key MATCHES "[*?]")
        file(GLOB_RECURSE _matched "${root}/${_key}")
      else()
        # A plain path names one file, not its namesakes in subfolders.
        file(GLOB _matched "${root}/${_key}")
      endif()
      set(_hit "")
      foreach(_m IN LISTS _matched)
        if(_m IN_LIST _files)
          list(APPEND _hit "${_m}")
        endif()
      endforeach()
      if(NOT _hit)
        string(APPEND _problems "stale allow row, glob matches no checked file: ${_key}\n")
        continue()
      endif()
      if(_n EQUAL 2)
        list(REMOVE_ITEM _files ${_hit})
      else()
        list(GET _parts 1 _type)
        string(STRIP "${_type}" _type)
        foreach(_m IN LISTS _hit)
          string(APPEND _lines "\nF\t${_m}\t${_type}\t${_r}")
        endforeach()
      endif()
    else()
      message(FATAL_ERROR "no_raw_new: allow row needs 'type | reason', 'glob | reason' or 'glob | type | reason': ${_r}")
    endif()
  endforeach()
  set(_allow_data "${work}/check_no_raw_new_allow.tsv")
  file(WRITE "${_allow_data}" "${_lines}\n")
  execute_process(
    COMMAND ${_awk} -f "${_awk_file}" "${_allow_data}" ${_files}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(_err)
    message(FATAL_ERROR "no_raw_new: awk failed: ${_err}")
  endif()
  if(_problems)
    set(_rc 1)
  endif()
  set(${out_var} "${_problems}${_out}" PARENT_SCOPE)
  set(${rc_var} ${_rc} PARENT_SCOPE)
endfunction()

if(SELF_TEST)
  set(_root "${CMAKE_CURRENT_BINARY_DIR}/no_raw_new_self")
  file(REMOVE_RECURSE "${_root}")
  file(MAKE_DIRECTORY "${_root}")
  function(_expect label want_rc out rc)
    if(want_rc AND rc EQUAL 0)
      message(FATAL_ERROR "no_raw_new self test (${label}): expected a failure, got:\n${out}")
    endif()
    if(NOT want_rc AND NOT rc EQUAL 0)
      message(FATAL_ERROR "no_raw_new self test (${label}): expected a pass, got:\n${out}")
    endif()
    foreach(_re IN LISTS ARGN)
      if(NOT out MATCHES "${_re}")
        message(FATAL_ERROR "no_raw_new self test (${label}): expected '${_re}' in:\n${out}")
      endif()
    endforeach()
  endfunction()

  set(_file "${_root}/a.cpp")
  file(WRITE "${_root}/allow.txt" "# comment\nListed | self test\n")

  file(WRITE "${_file}"
       "auto* a = new Listed;\n"
       "auto* b = new Listed{1, 2};\n"
       "auto* c = new (std::nothrow) ns::Listed<int>(3);\n"
       "void* d = ::new (mem) Other();\n"
       "// auto* e = new Other;\n"
       "/* auto* f = new Other;\n"
       "   auto* g = new Other; */\n"
       "const char* i = \"new Other\";\n"
       "static void* operator new(std::size_t size);\n"
       "int renew = 1; int new_count = 2;\n"
       "#include <new>\n")
  no_raw_new_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("allowed type and ignored text" FALSE "${_out}" ${_rc})

  file(WRITE "${_file}" "int x;\nauto* a = new SomeType;\n")
  no_raw_new_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("planted new" TRUE "${_out}" ${_rc} "a.cpp:2: .*raw new of SomeType" "stale allow row, no site makes Listed")

  file(WRITE "${_file}" "auto* a = new SomeType[4];\nauto* b = new (std::nothrow) Other;\ndelete[] a;\n")
  no_raw_new_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("array, nothrow and delete[]" TRUE "${_out}" ${_rc}
          "a.cpp:1: .*raw new of SomeType" "a.cpp:2: .*raw new of Other" "a.cpp:3: .*raw new of delete\\[\\]")

  file(WRITE "${_root}/allow.txt" "a.cpp | SomeType | self test\n")
  file(WRITE "${_file}" "auto* a = new SomeType;\nauto* b = new Other;\n")
  no_raw_new_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("file type row" TRUE "${_out}" ${_rc} "a.cpp:2: .*raw new of Other")
  if(_out MATCHES "a.cpp:1:")
    message(FATAL_ERROR "no_raw_new self test: the listed type was reported:\n${_out}")
  endif()

  file(WRITE "${_root}/allow.txt" "a.cpp | self test\n")
  no_raw_new_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("file row" FALSE "${_out}" ${_rc})

  file(WRITE "${_root}/allow.txt" "missing.cpp | self test\n")
  no_raw_new_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("stale glob" TRUE "${_out}" ${_rc} "glob matches no checked file: missing.cpp")
  return()
endif()

set(_files "")
foreach(_dir src include)
  file(GLOB_RECURSE _found "${SOURCE_DIR}/${_dir}/*.h" "${SOURCE_DIR}/${_dir}/*.hpp"
       "${SOURCE_DIR}/${_dir}/*.cpp" "${SOURCE_DIR}/${_dir}/*.inl")
  list(APPEND _files ${_found})
endforeach()
list(SORT _files)
if(NOT _files)
  message(FATAL_ERROR "no_raw_new: no source file under src and include")
endif()

no_raw_new_run("${SOURCE_DIR}" "${SOURCE_DIR}/cmake/scripts/raw_new_allow.txt" "${CMAKE_CURRENT_BINARY_DIR}" _out _rc ${_files})
if(_rc)
  string(REPLACE "${SOURCE_DIR}/" "" _out "${_out}")
  message("${_out}")
  message(FATAL_ERROR "no_raw_new: a raw new outside the heap types")
endif()
