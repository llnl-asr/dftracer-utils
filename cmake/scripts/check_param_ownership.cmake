# SPDX-License-Identifier: MIT
# Fails on a parameter that does not state ownership (AGENTS.md, "A parameter
# type states ownership"): `const Vector<T>&`, `const vector<T>&`,
# `const String&` and `const string&` (with or without dstd:: or std::), and
# `const char*` outside the C ABI. The C ABI is a function named dftu_*, a file
# named c_api* or *abi*, a file under plugins/abi, python/ or vendor/, a file
# with an extern "C" line, io_uring_wrapper.h (it mirrors the kernel sqe ABI),
# and main. A constructor of a dstd view type (ByteView, MutableByteView,
# CStringView, PathView) is exempt for every parameter kind: it is where a
# view is made. A pointer pair such as (const char* first, const char* last)
# counts as two sites, one per parameter. Comments and string literals are not
# checked, and neither is include/dftracer/utils/dstd/vendor/.
#
# cmake/scripts/param_ownership_allow.txt holds one row per file:
#   <path> <site count>
# The check fails on a file with more sites than its row (a new site), on a
# file with fewer (lower the row, or remove it at 0), and on a row whose file
# does not exist. The list only shrinks.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -DWORK_DIR=<scratch dir> -P check_param_ownership.cmake
#        cmake -DSOURCE_DIR=<repo root> -DWORK_DIR=<scratch dir> -DSELF_TEST=ON -P check_param_ownership.cmake
#        cmake -DSOURCE_DIR=<repo root> -DWORK_DIR=<scratch dir> -DGENERATE=ON -P check_param_ownership.cmake
#   GENERATE prints the allow rows for the current tree.

cmake_policy(SET CMP0057 NEW)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "check_param_ownership.cmake requires -DSOURCE_DIR=...")
endif()
if(NOT WORK_DIR)
  message(FATAL_ERROR "check_param_ownership.cmake requires -DWORK_DIR=<scratch dir>")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")

find_program(_awk awk REQUIRED)

set(_dirs src include tests benchmarks examples)
set(_skip include/dftracer/utils/dstd/vendor)

set(_program [=[
BEGIN { TYPE_RE = "(::)?([A-Za-z_][A-Za-z0-9_]*::)*(Vector|vector|String|string|char)" }
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
# Counts the parameter whose type token (with any namespace prefix) spans
# [s, e) of text T, written `const T` or `T const`. Returns 1 for a parameter
# that breaks the rule.
function site(T, s, e, exempt_file,    m, kind, p, c, q, depth, n, i, opener, j, k, w, lim, stop, east) {
  if (s > 1 && substr(T, s - 1, 1) ~ /[A-Za-z0-9_]/) return 0
  m = substr(T, s, e - s)
  sub(/.*::/, "", m)
  kind = (m == "char") ? "c" : "s"
  p = e
  if (substr(T, p, 1) ~ /[A-Za-z0-9_]/) return 0
  while (substr(T, p, 1) ~ /[ \t\n]/) p++
  if (kind == "c") {
    east = (substr(T, p, 5) == "const" && substr(T, p + 5, 1) !~ /[A-Za-z0-9_]/)
    if (east) { p += 5; while (substr(T, p, 1) ~ /[ \t\n]/) p++ }
    if (substr(T, p, 1) != "*") return 0
  } else {
    if (substr(T, p, 1) == "<") {
      depth = 0
      for (; p <= length(T); p++) {
        c = substr(T, p, 1)
        if (c == "<") depth++
        else if (c == ">") { depth--; if (depth == 0) { p++; break } }
        else if (c == ";" || c == "{") return 0
      }
      while (substr(T, p, 1) ~ /[ \t\n]/) p++
    }
    east = (substr(T, p, 5) == "const" && substr(T, p + 5, 1) !~ /[A-Za-z0-9_]/)
    if (east) { p += 5; while (substr(T, p, 1) ~ /[ \t\n]/) p++ }
    if (substr(T, p, 1) != "&" || substr(T, p + 1, 1) == "&") return 0
  }
  if (!east) {
    k = s - 1
    while (k > 0 && substr(T, k, 1) ~ /[ \t\n]/) k--
    if (k < 5 || substr(T, k - 4, 5) != "const" || (k > 5 && substr(T, k - 5, 1) ~ /[A-Za-z0-9_]/)) return 0
    s = k - 4
  }
  q = s - 1
  while (q > 0 && substr(T, q, 1) ~ /[ \t\n]/) q--
  if (q < 1) return 0
  c = substr(T, q, 1)
  if (c != "(" && c != ",") return 0
  opener = 0
  if (c == "(") opener = q
  else {
    depth = 0
    lim = q > 3000 ? q - 3000 : 1
    for (i = q - 1; i >= lim; i--) {
      c = substr(T, i, 1)
      if (c == ")" || c == "]" || c == "}") depth++
      else if (c == "(" || c == "[" || c == "{") {
        if (depth > 0) depth--
        else { if (c == "(") opener = i; break }
      } else if (c == ";" && depth == 0) break
    }
  }
  if (!opener) return 0
  j = opener - 1
  while (j > 0 && substr(T, j, 1) ~ /[ \t\n]/) j--
  w = ""
  while (j > 0 && substr(T, j, 1) ~ /[A-Za-z0-9_:~]/) { w = substr(T, j, 1) w; j-- }
  while (j > 0 && substr(T, j, 1) ~ /[ \t\n*&]/) j--
  if (j > 0 && substr(T, j, 1) == "<") return 0
  if (w ~ /^(if|while|for|switch|catch|sizeof|alignof|decltype|typeid|static_assert|requires)$/) return 0
  sub(/.*::/, "", w)
  if (w ~ /^(ByteView|MutableByteView|CStringView|PathView)$/) return 0
  if (kind == "c" && (exempt_file || w ~ /^dftu_/ || w == "main")) return 0
  return 1
}
function flush_file(    n, a, k, lo, hi, ctx, base, line, col, rest, off, s, e, count, i, rel, exempt, sites, nsites, kn) {
  if (cur == "") return
  rel = substr(cur, length(root) + 1)
  exempt = extern_c || rel ~ /(^|\/)(c_api[^\/]*|[^\/]*abi[^\/]*|io_uring_wrapper\.h)$/ || rel ~ /(^|\/)(plugins\/abi|python|vendor)\//
  count = 0
  nsites = 0
  for (n = 1; n <= nlines; n++) {
    if (L[n] !~ TYPE_RE) continue
    if (L[n] !~ /const/ && (n == 1 || L[n - 1] !~ /const/)) continue
    lo = n > 60 ? n - 60 : 1
    hi = n + 12 > nlines ? nlines : n + 12
    ctx = ""
    for (k = lo; k <= hi; k++) ctx = ctx L[k] "\n"
    base = 0
    for (k = lo; k < n; k++) base += length(L[k]) + 1
    rest = L[n]
    off = 0
    while (match(rest, TYPE_RE)) {
      s = base + off + RSTART
      e = s + RLENGTH
      off += RSTART + RLENGTH - 1
      rest = substr(rest, RSTART + RLENGTH)
      if (site(ctx, s, e, exempt)) {
        count++
        sites[++nsites] = n ": " L[n]
      }
    }
  }
  seen[rel] = 1
  if (generate) {
    if (count > 0) print rel " " count
  } else if (rel in allowed) {
    if (count > allowed[rel]) {
      for (i = 1; i <= nsites; i++) print rel ":" sites[i]
      print rel ": " count " parameter sites, the row allows " allowed[rel] "  (take a view, or an owning type by value: AGENTS.md, a parameter type states ownership)"
      hits++
    } else if (count < allowed[rel]) {
      print "allow row must shrink: " rel " allows " allowed[rel] " and has " count " (set the row to " count ", or remove it at 0)"
      hits++
    }
  } else if (count > 0) {
    for (i = 1; i <= nsites; i++) print rel ":" sites[i]
    print rel ": " count " parameter sites  (take a view, or an owning type by value: AGENTS.md, a parameter type states ownership)"
    hits++
  }
}
NR == FNR {
  if ($1 == "A") allowed[$2] = $3 + 0
  next
}
FILENAME != cur {
  flush_file()
  cur = FILENAME
  nlines = 0
  inblock = 0
  extern_c = 0
}
{
  if ($0 ~ /^[ \t]*extern[ \t]+"C"/) extern_c = 1
  L[++nlines] = strip($0)
}
END {
  flush_file()
  if (!generate) {
    for (r in allowed)
      if (!(r in seen)) {
        print "stale allow row, no checked file: " r
        hits++
      }
  }
  exit hits > 0 ? 1 : 0
}
]=])

# param_ownership_run(<root> <allow file> <work dir> <out var> <rc var> <files...>)
function(param_ownership_run root allow work out_var rc_var)
  file(MAKE_DIRECTORY "${work}")
  set(_awk_file "${work}/check_param_ownership.awk")
  file(WRITE "${_awk_file}" "${_program}")
  set(_files ${ARGN})
  set(_lines "#")
  file(READ "${allow}" _text)
  string(REPLACE "\n" ";" _rows "${_text}")
  foreach(_r IN LISTS _rows)
    string(STRIP "${_r}" _r)
    if(_r STREQUAL "" OR _r MATCHES "^#")
      continue()
    endif()
    if(NOT _r MATCHES "^([^ ]+) ([0-9]+)$")
      message(FATAL_ERROR "param_ownership: allow row needs '<path> <count>': ${_r}")
    endif()
    string(APPEND _lines "\nA\t${CMAKE_MATCH_1}\t${CMAKE_MATCH_2}")
  endforeach()
  set(_allow_data "${work}/check_param_ownership_allow.tsv")
  file(WRITE "${_allow_data}" "${_lines}\n")
  set(_gen 0)
  if(GENERATE)
    set(_gen 1)
  endif()
  execute_process(
    COMMAND ${_awk} -v "root=${root}/" -v "generate=${_gen}" -f "${_awk_file}" "${_allow_data}" ${_files}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(_err)
    message(FATAL_ERROR "param_ownership: awk failed: ${_err}")
  endif()
  set(${out_var} "${_out}" PARENT_SCOPE)
  set(${rc_var} ${_rc} PARENT_SCOPE)
endfunction()

if(SELF_TEST)
  set(_root "${WORK_DIR}/param_ownership_self")
  file(REMOVE_RECURSE "${_root}")
  file(MAKE_DIRECTORY "${_root}")
  function(_expect label want_rc out rc)
    if(want_rc AND rc EQUAL 0)
      message(FATAL_ERROR "param_ownership self test (${label}): expected a failure, got:\n${out}")
    endif()
    if(NOT want_rc AND NOT rc EQUAL 0)
      message(FATAL_ERROR "param_ownership self test (${label}): expected a pass, got:\n${out}")
    endif()
    foreach(_re IN LISTS ARGN)
      if(NOT out MATCHES "${_re}")
        message(FATAL_ERROR "param_ownership self test (${label}): expected '${_re}' in:\n${out}")
      endif()
    endforeach()
  endfunction()

  set(_file "${_root}/a.cpp")
  set(_other "${_root}/b.cpp")
  set(_abi "${_root}/c_api.cpp")
  file(WRITE "${_root}/allow.txt" "# comment\na.cpp 3\n")

  file(WRITE "${_file}"
       "void f(const dstd::Vector<int>& a);\n"
       "void g(int x,\n"
       "       const std::string& b);\n"
       "void h(const char* c, dstd::Span<const int> ok);\n"
       "// void i(const std::string& in_a_comment);\n"
       "/* void j(const std::vector<int>& in_a_block);\n"
       "   void k(const std::string& in_a_block); */\n"
       "const char* s = \"void l(const std::string& in_a_string)\";\n"
       "void m(const dstd::StringView& v, const std::string_view w);\n"
       "void n(const dstd::String&& moved);\n"
       "void o() { for (const std::string& s : names) {} }\n"
       "void p() { if (const std::string& s = x()) {} }\n"
       "using Fn = std::function<void(const std::string&)>;\n"
       "int main(int argc, const char* argv[]);\n"
       "void q(dstd::Vector<int>& grow, dstd::String by_value);\n"
       "void r(const std::map<int, int>& ok);\n")
  param_ownership_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("count at the row" FALSE "${_out}" ${_rc})

  file(WRITE "${_file}" "void f(const dstd::Vector<int>& a);\nvoid g(int x, const std::string& b);\n")
  param_ownership_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("row must shrink" TRUE "${_out}" ${_rc} "allow row must shrink: a.cpp allows 3 and has 2")

  file(WRITE "${_root}/allow.txt" "a.cpp 2\n")
  file(WRITE "${_file}" "void f(const dstd::Vector<int>& a);\nvoid g(int x, const std::string& b);\nvoid h(const char* c);\n")
  param_ownership_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("new site" TRUE "${_out}" ${_rc} "a.cpp:1: void f" "a.cpp:3: void h" "a.cpp: 3 parameter sites, the row allows 2")

  file(WRITE "${_other}" "int x;\nvoid f(const std::vector<int>& a);\n")
  file(WRITE "${_file}" "void f(const dstd::Vector<int>& a);\nvoid g(int x, const std::string& b);\n")
  param_ownership_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}" "${_other}")
  _expect("unlisted file" TRUE "${_out}" ${_rc} "b.cpp:2: void f" "b.cpp: 1 parameter sites")
  if(_out MATCHES "a.cpp")
    message(FATAL_ERROR "param_ownership self test: the listed file was reported:\n${_out}")
  endif()

  file(WRITE "${_root}/allow.txt" "missing.cpp 1\n")
  file(WRITE "${_file}" "int x;\n")
  param_ownership_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("stale row" TRUE "${_out}" ${_rc} "stale allow row, no checked file: missing.cpp")

  file(WRITE "${_root}/allow.txt" "# empty\n")
  file(WRITE "${_abi}" "void dftu_f(const char* a);\nvoid h(const char* c);\n")
  file(WRITE "${_file}" "extern \"C\" {\nvoid f(const char* a);\n}\n")
  param_ownership_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}" "${_abi}")
  _expect("c abi files" FALSE "${_out}" ${_rc})

  file(WRITE "${_root}/allow.txt" "# empty\n")
  file(WRITE "${_file}" "ByteView(const char* a, int n);\nclass X { CStringView(const char* s); };\nvoid g(const dstd::String& b);\n")
  param_ownership_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("view type constructors" TRUE "${_out}" ${_rc} "a.cpp:3: void g")
  if(_out MATCHES "a.cpp:[12]:")
    message(FATAL_ERROR "param_ownership self test: a view constructor was reported:\n${_out}")
  endif()

  file(WRITE "${_root}/allow.txt" "# empty\n")
  file(WRITE "${_file}"
       "void a1(const dftracer::utils::dstd::Vector<int>& a);\n"
       "void a2(const ::dftracer::utils::dstd::String& b);\n"
       "void a3(dstd::Vector<int> const& c);\n"
       "void a4(std::string const& d);\n"
       "void a5(char const* e);\n"
       "void a6(int x, const std::vector<int>& f);\n"
       "void a7(int x, std::string const& g, int y);\n"
       "void a8(::std::string const& h);\n")
  param_ownership_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("qualified and east const" TRUE "${_out}" ${_rc} "a.cpp: 8 parameter sites")

  file(WRITE "${_file}"
       "void f() {\n"
       "  const std::string& s = name();\n"
       "  std::string const& t = name();\n"
       "  const dstd::Vector<int>& v = vec();\n"
       "  for (const std::string& u : names) {}\n"
       "  for (std::string const& w : names) {}\n"
       "  const char* p = \"x\";\n"
       "  char const* q = \"x\";\n"
       "}\n")
  param_ownership_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("locals and range for" FALSE "${_out}" ${_rc})

  file(WRITE "${_file}" "void dftu_f(const char* a);\nvoid g(const char* b);\n")
  param_ownership_run("${_root}" "${_root}/allow.txt" "${_root}/work" _out _rc "${_file}")
  _expect("c abi function name" TRUE "${_out}" ${_rc} "a.cpp:2: void g")
  if(_out MATCHES "a.cpp:1:")
    message(FATAL_ERROR "param_ownership self test: a dftu_ function was reported:\n${_out}")
  endif()
  return()
endif()

set(_files "")
foreach(_dir IN LISTS _dirs)
  file(GLOB_RECURSE _found "${SOURCE_DIR}/${_dir}/*.h" "${SOURCE_DIR}/${_dir}/*.hpp"
       "${SOURCE_DIR}/${_dir}/*.cpp" "${SOURCE_DIR}/${_dir}/*.inl")
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
  message(FATAL_ERROR "param_ownership: no source file under the listed areas")
endif()

param_ownership_run("${SOURCE_DIR}" "${SOURCE_DIR}/cmake/scripts/param_ownership_allow.txt" "${WORK_DIR}" _out _rc ${_files})
if(GENERATE)
  message("${_out}")
  return()
endif()
if(_rc)
  message("${_out}")
  message(FATAL_ERROR "param_ownership: a parameter type that does not state ownership")
endif()
