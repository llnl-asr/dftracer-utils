# SPDX-License-Identifier: MIT
# Fails when an area already moved to dstd names a std type that dstd has (see
# include/dftracer/utils/dstd/). A file that must keep one is listed in
# cmake/scripts/dstd_only_allow.txt with the exact std names it uses. The check
# also fails on a row that matches no file, on a row name no matching file
# uses, and on any line that contains "keep std". Comments are not checked.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_dstd_only.cmake
#        cmake -DSOURCE_DIR=<repo root> -DSELF_TEST=ON -P check_dstd_only.cmake

cmake_policy(SET CMP0057 NEW)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "check_dstd_only.cmake requires -DSOURCE_DIR=...")
endif()
if(NOT CMAKE_CURRENT_BINARY_DIR)
  set(CMAKE_CURRENT_BINARY_DIR "${CMAKE_BINARY_DIR}")
endif()

find_program(_awk awk REQUIRED)

set(_dirs
    include/dftracer/utils/dstd
    src/dftracer/utils/dstd
    examples
    include/dftracer/utils/common
    src/dftracer/utils/common
    include/dftracer/utils/coro
    src/dftracer/utils/coro
    include/dftracer/utils/runtime
    src/dftracer/utils/runtime
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
    src/dftracer/utils/server
    src/dftracer/utils/binaries
    src/dftracer/utils/python
    benchmarks
    tests/binaries
    tests/cmake
    tests/common
    tests/dataframe
    tests/dstd
    tests/compose
    tests/coro
    tests/duql
    tests/index
    tests/io
    tests/layering
    tests/layout
    tests/memory
    tests/reader
    tests/replay
    tests/runtime
    tests/server
    tests/store
    tests/utilities
    tests/trace)

set(_extra_files
    tests/index_test_helpers.h
    tests/instrumentation.h
    tests/mmix_lcg.h
    tests/no_core_dump.h
    tests/test_testing_utilities.cpp
    tests/testing_runtime.h
    tests/testing_utilities.cpp
    tests/testing_utilities.h)

# The Task, TaskGraph and Pipeline trees move in a later step.
set(_skip
    include/dftracer/utils/runtime/tasks
    src/dftracer/utils/runtime/tasks
    src/dftracer/utils/runtime/task_graph
    src/dftracer/utils/runtime/pipeline
    include/dftracer/utils/dstd/vendor)

set(_program [=[
NR == FNR {
  n = split($0, f, "\t")
  if (f[1] == "A") allowed[f[2], f[3]] = allowed[f[2], f[3]] " " f[4]
  else if (f[1] == "N") rowname[f[2], f[3]] = f[4]
  next
}
function emit(name, text, hint,    key, n, ids, i) {
  key = FILENAME SUBSEP name
  if (key in allowed) {
    n = split(allowed[key], ids, " ")
    for (i = 1; i <= n; i++) used[ids[i], name] = 1
    return
  }
  print FILENAME ":" FNR ": " text hint
  hits++
}
{
  if ($0 ~ /keep std/) {
    print FILENAME ":" FNR ": " $0 "  (the keep std marker is gone: list the file in cmake/scripts/dstd_only_allow.txt)"
    hits++
  }
  line = $0
  sub(/\/\/.*$/, "", line)
  if (line ~ /^[ \t]*(\/\*|\*)/) next
  if (line ~ /ankerl::/ || line ~ /dstd\/vendor\/unordered_dense\.h/) {
    emit("ankerl", $0, "  (use dstd::HashMap or dstd::HashSet)")
    next
  }
  found = 0
  rest = line
  while (match(rest, /(^|[^A-Za-z0-9_:])std::(string|string_view|vector|span|map|set|multimap|multiset|unordered_map|unordered_set|deque|list|queue|priority_queue|unique_ptr|make_unique|shared_ptr|make_shared|weak_ptr|optional|nullopt|variant|monostate|pair|make_pair|tuple|any|any_cast|function|thread|promise|future|shared_future|to_string|stoi|stol|stoll|stoul|stoull|stod|stof|stable_sort|ofstream|ifstream|fstream|ostringstream|stringstream|istringstream|to_chars|from_chars|hash)([^A-Za-z0-9_]|$)/)) {
    m = substr(rest, RSTART, RLENGTH)
    sub(/^[^A-Za-z0-9_:]?std::/, "", m)
    sub(/[^A-Za-z0-9_]$/, "", m)
    emit(m, $0, "")
    found = 1
    rest = substr(rest, RSTART + RLENGTH - 1)
  }
  if (!found && (line ~ /UniquePtr<.*>[ \t]*([A-Za-z_][A-Za-z0-9_]*)?[ \t]*[({][ \t]*new[ \t]/ ||
                 line ~ /[A-Za-z0-9]_[ \t]*[({][ \t]*new[ \t]+[A-Za-z_:]/)) {
    emit("new", $0, "  (raw new: use dstd::make_unique)")
  }
}
END {
  for (key in rowname) {
    if (!(key in used)) {
      split(key, p, SUBSEP)
      print "stale allow row, no matching file uses std::" p[2] ": " rowname[key]
      hits++
    }
  }
  exit hits > 0 ? 1 : 0
}
]=])

# dstd_only_run(<root> <allow file> <work dir> <out var> <rc var> <files...>)
# Checks the given absolute files against the rows of the allow file. The
# scratch files go to <work dir>, so two runs never share one.
function(dstd_only_run root allow work out_var rc_var)
  file(MAKE_DIRECTORY "${work}")
  set(_awk_file "${work}/check_dstd_only.awk")
  file(WRITE "${_awk_file}" "${_program}")
  set(_files ${ARGN})
  set(_problems "")
  set(_lines "#")
  set(_row 0)
  file(READ "${allow}" _text)
  string(REGEX REPLACE "[;\\[\\]]" " " _text "${_text}")
  string(REPLACE "\n" ";" _rows "${_text}")
  foreach(_r IN LISTS _rows)
    string(STRIP "${_r}" _r)
    if(_r STREQUAL "" OR _r MATCHES "^#")
      continue()
    endif()
    math(EXPR _row "${_row} + 1")
    string(REPLACE "|" ";" _parts "${_r}")
    list(LENGTH _parts _n)
    if(NOT _n EQUAL 3)
      message(FATAL_ERROR "dstd_only: allow row needs 'glob | names | reason': ${_r}")
    endif()
    list(GET _parts 0 _glob)
    list(GET _parts 1 _names)
    string(STRIP "${_glob}" _glob)
    string(REPLACE "," ";" _names "${_names}")
    file(GLOB _matched "${root}/${_glob}")
    set(_hit "")
    foreach(_m IN LISTS _matched)
      if(_m IN_LIST _files)
        list(APPEND _hit "${_m}")
      endif()
    endforeach()
    if(NOT _hit)
      string(APPEND _problems "stale allow row, glob matches no checked file: ${_glob}\n")
      continue()
    endif()
    foreach(_name IN LISTS _names)
      string(STRIP "${_name}" _name)
      string(APPEND _lines "\nN\t${_row}\t${_name}\t${_glob}")
      foreach(_m IN LISTS _hit)
        string(APPEND _lines "\nA\t${_m}\t${_name}\t${_row}")
      endforeach()
    endforeach()
  endforeach()
  set(_allow_data "${work}/check_dstd_only_allow.tsv")
  file(WRITE "${_allow_data}" "${_lines}\n")
  execute_process(
    COMMAND ${_awk} -f "${_awk_file}" "${_allow_data}" ${_files}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(_err)
    message(FATAL_ERROR "dstd_only: awk failed: ${_err}")
  endif()
  if(_problems)
    set(_rc 1)
  endif()
  set(${out_var} "${_problems}${_out}" PARENT_SCOPE)
  set(${rc_var} ${_rc} PARENT_SCOPE)
endfunction()

if(SELF_TEST)
  set(_root "${CMAKE_CURRENT_BINARY_DIR}/dstd_only_self")
  file(REMOVE_RECURSE "${_root}")
  file(MAKE_DIRECTORY "${_root}")
  function(_expect label want_rc out rc)
    if(want_rc AND rc EQUAL 0)
      message(FATAL_ERROR "dstd_only self test (${label}): expected a failure, got:\n${out}")
    endif()
    if(NOT want_rc AND NOT rc EQUAL 0)
      message(FATAL_ERROR "dstd_only self test (${label}): expected a pass, got:\n${out}")
    endif()
    foreach(_re IN LISTS ARGN)
      if(NOT out MATCHES "${_re}")
        message(FATAL_ERROR "dstd_only self test (${label}): expected '${_re}' in:\n${out}")
      endif()
    endforeach()
  endfunction()

  set(_listed "${_root}/listed.cpp")
  set(_unlisted "${_root}/unlisted.cpp")
  file(WRITE "${_root}/allow.txt" "# comment\nlisted.cpp | vector | self test\n")
  file(WRITE "${_listed}"
       "std::vector<std::vector<int>> a;\n"
       "dstd::Vector<int> b;\n"
       "// std::string in a comment\n"
       " * std::string in a doc comment\n"
       "dstd::hash::splitmix64(1);\n"
       "void *p = new (mem) T();\n")
  file(WRITE "${_unlisted}" "int x;\nstd::vector<int> a;\n")

  dstd_only_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_listed}")
  _expect("allowed name in a listed file" FALSE "${_out}" ${_rc})

  dstd_only_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_listed}" "${_unlisted}")
  _expect("same name in an unlisted file" TRUE "${_out}" ${_rc} ":2: std::vector<int> a;")
  if(_out MATCHES "listed.cpp:[0-9]+: std::vector<std")
    message(FATAL_ERROR "dstd_only self test: the listed file was reported:\n${_out}")
  endif()

  file(WRITE "${_listed}" "std::vector<int> a;\nstd::string s;\n")
  dstd_only_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_listed}")
  _expect("other name in a listed file" TRUE "${_out}" ${_rc} ":2: std::string s;")
  if(_out MATCHES ":1: std::vector")
    message(FATAL_ERROR "dstd_only self test: the allowed name was reported:\n${_out}")
  endif()

  file(WRITE "${_listed}" "std::vector<int> a;\n")
  file(WRITE "${_root}/allow.txt" "listed.cpp | vector, deque | self test\n")
  dstd_only_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_listed}")
  _expect("stale name in a row" TRUE "${_out}" ${_rc} "stale allow row, no matching file uses std::deque")

  file(WRITE "${_root}/allow.txt" "missing.cpp | vector | self test\n")
  dstd_only_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_listed}")
  _expect("stale glob" TRUE "${_out}" ${_rc} "glob matches no checked file: missing.cpp")

  file(WRITE "${_root}/allow.txt" "listed.cpp | vector | self test\n")
  file(WRITE "${_listed}"
       "std::vector<int> a;  // keep std: reason\n"
       "dstd::UniquePtr<int> f(new int(1));\n"
       "ankerl::unordered_dense::map<int, int> g;\n"
       "#include <dftracer/utils/dstd/vendor/unordered_dense.h>\n"
       "std::hash<int> h;\n"
       "A() : m_(new int(1)) {}\n")
  dstd_only_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_listed}")
  _expect("marker and other rules" TRUE "${_out}" ${_rc}
          ":1: .*keep std marker is gone" ":2: dstd::UniquePtr" ":3: ankerl"
          ":4: #include" ":5: std::hash" ":6: A\\(\\)")
  return()
endif()

set(_files "")
foreach(_dir IN LISTS _dirs)
  file(GLOB_RECURSE _found "${SOURCE_DIR}/${_dir}/*.h" "${SOURCE_DIR}/${_dir}/*.cpp"
       "${SOURCE_DIR}/${_dir}/*.inl")
  list(APPEND _files ${_found})
endforeach()
foreach(_file IN LISTS _extra_files)
  list(APPEND _files "${SOURCE_DIR}/${_file}")
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

dstd_only_run("${SOURCE_DIR}" "${SOURCE_DIR}/cmake/scripts/dstd_only_allow.txt" "${CMAKE_CURRENT_BINARY_DIR}" _out _rc ${_files})
if(_rc)
  string(REPLACE "${SOURCE_DIR}/" "" _out "${_out}")
  message("${_out}")
  message(FATAL_ERROR "dstd_only: std types that dstd has, in an area moved to dstd")
endif()
