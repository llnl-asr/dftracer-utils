# SPDX-License-Identifier: MIT
# Fails on a parameter that does not state ownership (AGENTS.md, "A parameter
# type states ownership"): `const Vector<T>&`, `const vector<T>&`,
# `const String&` and `const string&` (with or without dstd:: or std::), and
# `const char*` outside the C ABI. The C ABI is a function named dftu_*, a file
# named c_api* or *abi*, a file under plugins/abi, python/ or vendor/, a file
# with an extern "C" line, io_uring_wrapper.h (it mirrors the kernel sqe ABI),
# the parameter list of a function or function-pointer type such as
# `R (*)(const char*)` (a C ABI signature, not a parameter of ours),
# main, and a C ABI trampoline: a function that is a direct element of the
# braced initializer of a table object (`const dftu_x_vt T = {..., f, ...}`,
# `&f` or `.slot = f` too) or the right side of `table.slot = f;` where `table`
# is a name that some file declares with a table type. A table type is a name
# that ends in `_vt`, starts with `dftu_svc_`, or is `dftu_plugin_host`,
# `dftu_io` or `dftu_test_harness`. The table fixes the signature. The same-file
# name must have one declaration that takes a `char` pointer (the branches after
# an `#else` or `#elif` are alternatives of the first and do not count): an
# overload that takes one too is not exempt, and neither is a name that is only an argument of a
# nested call. A constructor of a dstd view type (ByteView, MutableByteView,
# CStringView, PathView) is exempt for every parameter kind: it is where a
# view is made. A pointer pair such as (const char* first, const char* last)
# counts as two sites, one per parameter. Comments and string literals are not
# checked, and neither is include/dftracer/utils/dstd/vendor/.
#
# There is no allow list: every site fails the check.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -DWORK_DIR=<scratch dir> -P check_param_ownership.cmake
#        cmake -DSOURCE_DIR=<repo root> -DWORK_DIR=<scratch dir> -DSELF_TEST=ON -P check_param_ownership.cmake

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
  if (substr(T, j, 1) == ")" && substr(T, j - 9 > 0 ? j - 9 : 1, 10) != "operator()") return 0
  w = ""
  while (j > 0 && substr(T, j, 1) ~ /[A-Za-z0-9_:~]/) { w = substr(T, j, 1) w; j-- }
  while (j > 0 && substr(T, j, 1) ~ /[ \t\n*&]/) j--
  if (j > 0 && substr(T, j, 1) == "<") return 0
  if (w ~ /^(if|while|for|switch|catch|sizeof|alignof|decltype|typeid|static_assert|requires)$/) return 0
  sub(/.*::/, "", w)
  if (w ~ /^(ByteView|MutableByteView|CStringView|PathView)$/) return 0
  if (kind == "c" && (exempt_file || w ~ /^dftu_/ || w == "main" || (w in stored))) return 0
  return 1
}
# A table type that fixes the signature of the functions in its slots: a vtable
# (`*_vt`), a host service table (`dftu_svc_*`, `dftu_plugin_host`, `dftu_io`)
# or the C test harness `dftu_test_harness`.
function is_table_type(t) {
  return t ~ /_vt$/ || t ~ /^dftu_svc_/ || t == "dftu_plugin_host" || t == "dftu_io" || t == "dftu_test_harness"
}
# The number of distinct parameter lists among the declarations of `name` in
# text T that take a `char` pointer: an occurrence `name(` that follows a type
# or a scope, not a call. A forward declaration and its definition share one
# list; a same-named function with other parameters is no overload of the
# table's slot.
function decl_count(T, name,    rest, n, k, c, w, pos, pre, depth, params, i, ch, key, seen) {
  n = 0
  split("", seen)
  rest = T
  while (match(rest, "[^A-Za-z0-9_]" name "[ \t\n]*\\(")) {
    pos = RSTART + RLENGTH
    pre = substr(rest, 1, RSTART)
    rest = substr(rest, pos)
    k = length(pre)
    while (k > 0 && substr(pre, k, 1) ~ /[ \t\n]/) k--
    if (k < 1) continue
    c = substr(pre, k, 1)
    if (c == ">" && k > 1 && substr(pre, k - 1, 1) == "-") continue
    if (c !~ /[A-Za-z0-9_*&>:~]/) continue
    if (c ~ /[A-Za-z0-9_]/) {
      w = ""
      while (k > 0 && substr(pre, k, 1) ~ /[A-Za-z0-9_]/) { w = substr(pre, k, 1) w; k-- }
      if (w ~ /^(return|co_return|co_await|co_yield|else|case|throw|new|delete|sizeof|typeid|not|and|or)$/) continue
    }
    depth = 1
    params = ""
    for (i = 1; i <= length(rest); i++) {
      ch = substr(rest, i, 1)
      if (ch == "(") depth++
      else if (ch == ")") { depth--; if (depth == 0) break }
      params = params ch
    }
    if (params !~ /char/ || params !~ /\*/) continue
    key = params
    gsub(/[ \t\n]+/, " ", key)
    if (!(key in seen)) { seen[key] = 1; n++ }
  }
  return n
}
# Records name as a stored function unless the file declares it more than once.
function store_name(T, name) {
  sub(/.*::/, "", name)
  if (decl_count(T, name) <= 1) stored[name] = 1
}
# Takes the elements of a braced initializer that are a bare function name,
# `f`, `&f` or `.slot = f`. An element that calls something, or that sits in a
# nested brace, holds no stored function.
function take_initializer(T, after,    depth, i, c, elem, id) {
  depth = 0
  elem = ""
  for (i = 1; i <= length(after); i++) {
    c = substr(after, i, 1)
    if (c == "{" || c == "(" || c == "[") {
      depth++
      if (depth > 1) elem = elem c
      continue
    }
    if (c == "}" || c == ")" || c == "]") {
      depth--
      if (depth == 0) break
      elem = elem c
      continue
    }
    if (depth == 1 && c == ",") {
      take_element(T, elem)
      elem = ""
      continue
    }
    elem = elem c
  }
  take_element(T, elem)
}
function take_element(T, elem) {
  gsub(/^[ \t\n]+|[ \t\n]+$/, "", elem)
  sub(/^\.[A-Za-z_][A-Za-z0-9_]*[ \t\n]*=[ \t\n]*/, "", elem)
  sub(/^&[ \t\n]*/, "", elem)
  if (elem ~ /^[A-Za-z_][A-Za-z0-9_:]*$/) store_name(T, elem)
}
# Fills stored[] with the names of functions that a table fixes the signature
# of: a direct element of the initializer of a table object, or the right side
# of `table.slot = f;`. table_objs[] holds the names declared with a table type
# in any file (the first pass over the file list collects it), so `host_.f = fn;`
# counts where `host_` is a member declared in a header.
function find_stored(    T, TC, k, rest, i, c, id, after, pos, tail, ty, pp, skip_at) {
  delete stored
  T = ""
  TC = ""
  pp = 0
  skip_at = 0
  for (k = 1; k <= nlines; k++) {
    T = T L[k] "\n"
    if (L[k] ~ /^[ \t]*#[ \t]*if/) pp++
    else if (L[k] ~ /^[ \t]*#[ \t]*(else|elif)/) { if (!skip_at) skip_at = pp }
    else if (L[k] ~ /^[ \t]*#[ \t]*endif/) { if (skip_at == pp) skip_at = 0; pp-- }
    if (!skip_at) TC = TC L[k] "\n"
  }
  rest = T
  while (match(rest, /dftu_[A-Za-z0-9_]*[ \t\n*&]+[A-Za-z_][A-Za-z0-9_]*[ \t\n]*(\[[^]]*\])?[ \t\n]*[=;{]/)) {
    tail = substr(rest, RSTART, RLENGTH)
    pos = RSTART + RLENGTH
    ty = tail
    sub(/[^A-Za-z0-9_].*$/, "", ty)
    id = tail
    sub(/^dftu_[A-Za-z0-9_]*[ \t\n*&]+/, "", id)
    sub(/[^A-Za-z0-9_].*$/, "", id)
    if (is_table_type(ty)) {
      table_objs[id] = 1
      if (substr(tail, length(tail), 1) == "=") {
        after = substr(rest, pos)
        sub(/^[ \t\n]+/, "", after)
        if (substr(after, 1, 1) == "{") take_initializer(TC, after)
      } else if (substr(tail, length(tail), 1) == "{") {
        take_initializer(TC, "{" substr(rest, pos))
      }
    }
    rest = substr(rest, pos)
  }
  rest = T
  while (match(rest, /[A-Za-z_][A-Za-z0-9_]*(\.|->)[A-Za-z_][A-Za-z0-9_]*[ \t\n]*=[ \t\n]*&?[A-Za-z_][A-Za-z0-9_:]*[ \t\n]*;/)) {
    tail = substr(rest, RSTART, RLENGTH)
    rest = substr(rest, RSTART + RLENGTH)
    id = tail
    sub(/(\.|->).*$/, "", id)
    if (!(id in table_objs)) continue
    sub(/^.*=[ \t\n]*&?/, "", tail)
    sub(/[ \t\n]*;$/, "", tail)
    store_name(TC, tail)
  }
}
function flush_file(    n, a, k, lo, hi, ctx, base, line, col, rest, off, s, e, count, i, rel, exempt, sites, nsites, kn) {
  if (cur == "") return
  find_stored()
  if (!pass2) return
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
  if (count > 0) {
    for (i = 1; i <= nsites; i++) print rel ":" sites[i]
    print rel ": " count " parameter sites  (take a view, or an owning type by value: AGENTS.md, a parameter type states ownership)"
    hits++
  }
}
FNR == 1 {
  flush_file()
  if (FILENAME in first_pass) pass2 = 1
  first_pass[FILENAME] = 1
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
  exit hits > 0 ? 1 : 0
}
]=])

# param_ownership_run(<root> <work dir> <out var> <rc var> <files...>)
function(param_ownership_run root work out_var rc_var)
  file(MAKE_DIRECTORY "${work}")
  set(_awk_file "${work}/check_param_ownership.awk")
  file(WRITE "${_awk_file}" "${_program}")
  set(_files ${ARGN})
  execute_process(
    COMMAND ${_awk} -v "root=${root}/" -f "${_awk_file}" ${_files} ${_files}
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
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
  _expect("every site is reported" TRUE "${_out}" ${_rc} "a.cpp:1: void f" "a.cpp:3:        const std::string& b" "a.cpp:4: void h" "a.cpp: 3 parameter sites")
  if(_out MATCHES "a.cpp:[5-9]:" OR _out MATCHES "a.cpp:1[0-6]:")
    message(FATAL_ERROR "param_ownership self test: a masked or exempt line was reported:\n${_out}")
  endif()

  file(WRITE "${_other}" "int x;\nvoid f(const std::vector<int>& a);\n")
  file(WRITE "${_file}" "int y;\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}" "${_other}")
  _expect("one file" TRUE "${_out}" ${_rc} "b.cpp:2: void f" "b.cpp: 1 parameter sites")
  if(_out MATCHES "a.cpp")
    message(FATAL_ERROR "param_ownership self test: a clean file was reported:\n${_out}")
  endif()

  file(WRITE "${_file}" "int x;\n")
  file(REMOVE "${_other}")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
  _expect("clean" FALSE "${_out}" ${_rc})

  file(WRITE "${_abi}" "void dftu_f(const char* a);\nvoid h(const char* c);\n")
  file(WRITE "${_file}" "extern \"C\" {\nvoid f(const char* a);\n}\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}" "${_abi}")
  _expect("c abi files" FALSE "${_out}" ${_rc})

  file(WRITE "${_file}" "ByteView(const char* a, int n);\nclass X { CStringView(const char* s); };\nvoid g(const dstd::String& b);\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
  _expect("view type constructors" TRUE "${_out}" ${_rc} "a.cpp:3: void g")
  if(_out MATCHES "a.cpp:[12]:")
    message(FATAL_ERROR "param_ownership self test: a view constructor was reported:\n${_out}")
  endif()

  file(WRITE "${_file}"
       "void a1(const dftracer::utils::dstd::Vector<int>& a);\n"
       "void a2(const ::dftracer::utils::dstd::String& b);\n"
       "void a3(dstd::Vector<int> const& c);\n"
       "void a4(std::string const& d);\n"
       "void a5(char const* e);\n"
       "void a6(int x, const std::vector<int>& f);\n"
       "void a7(int x, std::string const& g, int y);\n"
       "void a8(::std::string const& h);\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
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
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
  _expect("locals and range for" FALSE "${_out}" ${_rc})

  file(WRITE "${_file}"
       "dftu_status tramp(dftu_ctx* c, const char* msg, int n);\n"
       "dftu_status plain(dftu_ctx* c, const char* msg, int n);\n"
       "const void* assigned(void* h, const char* id);\n"
       "void other(const char* s);\n"
       "static const dftu_ctx_vt VT = {sizeof(dftu_ctx_vt), &tramp, nullptr};\n"
       "void setup(Plain& o) {\n"
       "  dftu_svc_ops host;\n"
       "  host.get_service = assigned;\n"
       "  o.cb = other;\n"
       "}\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
  _expect("c abi trampolines" TRUE "${_out}" ${_rc} "a.cpp:2: dftu_status plain" "a.cpp:4: void other" "a.cpp: 2 parameter sites")
  if(_out MATCHES "a.cpp:[13]:")
    message(FATAL_ERROR "param_ownership self test: a trampoline was reported:\n${_out}")
  endif()

  file(WRITE "${_file}" "class X { dftu_plugin_host host_; };\n")
  file(WRITE "${_other}"
       "const void* cb(void* h, const char* id);\n"
       "void init() { host_.get_service = cb; }\n"
       "void other(const char* s);\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}" "${_other}")
  _expect("trampoline stored through a member declared elsewhere" TRUE "${_out}" ${_rc} "b.cpp:3: void other" "b.cpp: 1 parameter sites")
  if(_out MATCHES "b.cpp:1:")
    message(FATAL_ERROR "param_ownership self test: a stored trampoline was reported:\n${_out}")
  endif()

  file(WRITE "${_file}"
       "void nested(const char* s);\n"
       "void direct(const char* s);\n"
       "static const dftu_ctx_vt VT = {sizeof(dftu_ctx_vt), wrap(nested), direct};\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
  _expect("nested call argument" TRUE "${_out}" ${_rc} "a.cpp:1: void nested" "a.cpp: 1 parameter sites")
  if(_out MATCHES "a.cpp:2:")
    message(FATAL_ERROR "param_ownership self test: a direct element was reported:\n${_out}")
  endif()

  file(WRITE "${_file}"
       "dftu_status slot(dftu_ctx* c, const char* msg);\n"
       "dftu_status slot(const char* a, int msg);\n"
       "dftu_status solo(dftu_ctx* c, const char* msg);\n"
       "static const dftu_ctx_vt VT = {sizeof(dftu_ctx_vt), slot, solo};\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
  _expect("overload of a stored name" TRUE "${_out}" ${_rc} "a.cpp:1: dftu_status slot" "a.cpp:2: dftu_status slot" "a.cpp: 2 parameter sites")
  if(_out MATCHES "a.cpp:3:")
    message(FATAL_ERROR "param_ownership self test: a single declaration was reported:\n${_out}")
  endif()

  file(WRITE "${_file}"
       "dftu_status fwd(dftu_ctx* c, const char* msg);\n"
       "static const dftu_ctx_vt VT = {sizeof(dftu_ctx_vt), fwd};\n"
       "dftu_status fwd(dftu_ctx* c, const char* msg) { return 0; }\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
  _expect("forward declaration of a stored name" FALSE "${_out}" ${_rc})

  file(WRITE "${_file}"
       "#if HAVE_X\n"
       "dftu_status alt(dftu_ctx* c, const char* msg) { return 0; }\n"
       "#else\n"
       "dftu_status alt(dftu_ctx* c, const char* msg) { return 1; }\n"
       "#endif\n"
       "static const dftu_io IO = {sizeof(dftu_io), alt};\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
  _expect("alternatives of one preprocessor branch" FALSE "${_out}" ${_rc})

  file(WRITE "${_file}"
       "void cb(const char* s);\n"
       "void setup() {\n"
       "  dftu_options opts;\n"
       "  opts.callback = cb;\n"
       "}\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
  _expect("field of a type that is not a table" TRUE "${_out}" ${_rc} "a.cpp:1: void cb" "a.cpp: 1 parameter sites")

  file(WRITE "${_file}" "void dftu_f(const char* a);\nvoid g(const char* b);\n")
  param_ownership_run("${_root}" "${_root}/work" _out _rc "${_file}")
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

param_ownership_run("${SOURCE_DIR}" "${WORK_DIR}" _out _rc ${_files})
if(_rc)
  message("${_out}")
  message(FATAL_ERROR "param_ownership: a parameter type that does not state ownership")
endif()
