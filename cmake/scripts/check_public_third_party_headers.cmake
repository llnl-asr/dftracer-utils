# SPDX-License-Identifier: MIT
# Fails when a public header includes a third-party header that is not vendored.
# A header under include/ may include our own <dftracer/...> headers, a C or C++
# standard header, a C/POSIX/system header, <mpi.h>, and the vendored headers of
# include/dftracer/utils/dstd/vendor/ (an included path under dftracer/; the
# folder itself is not scanned, its text is upstream's). Every other third-party
# library comes through CPM and is used only in src/. simdjson and nanoarrow are
# the known exceptions until the API cut moves them out of include/.
#
# A header missing from a list below is added here, with a reason, not skipped.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_public_third_party_headers.cmake
#        cmake -DSOURCE_DIR=<repo root> -DSELF_TEST=ON -P check_public_third_party_headers.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED SOURCE_DIR)
  message(
    FATAL_ERROR
      "check_public_third_party_headers.cmake requires -DSOURCE_DIR=...")
endif()

set(VENDOR_ROOT "dftracer/utils/dstd/vendor")

set(CXX_HEADERS
    algorithm
    any
    array
    atomic
    barrier
    bit
    bitset
    charconv
    chrono
    codecvt
    compare
    complex
    concepts
    condition_variable
    coroutine
    cuchar
    cwchar
    cwctype
    deque
    exception
    execution
    expected
    experimental/memory_resource
    filesystem
    flat_map
    flat_set
    format
    forward_list
    fstream
    functional
    future
    generator
    initializer_list
    iomanip
    ios
    iosfwd
    iostream
    istream
    iterator
    latch
    limits
    list
    locale
    map
    mdspan
    memory
    memory_resource
    mutex
    new
    numbers
    numeric
    optional
    ostream
    print
    queue
    random
    ranges
    ratio
    regex
    scoped_allocator
    semaphore
    set
    shared_mutex
    source_location
    span
    spanstream
    sstream
    stack
    stacktrace
    stdfloat
    stdexcept
    stop_token
    streambuf
    string
    string_view
    strstream
    syncstream
    system_error
    thread
    tuple
    type_traits
    typeindex
    typeinfo
    unordered_map
    unordered_set
    utility
    valarray
    variant
    vector
    version)

# The C headers, and the C++ wrappers that include them.
set(C_HEADERS
    assert.h
    complex.h
    ctype.h
    errno.h
    fenv.h
    float.h
    inttypes.h
    iso646.h
    limits.h
    locale.h
    math.h
    setjmp.h
    signal.h
    stdalign.h
    stdarg.h
    stdatomic.h
    stdbit.h
    stdbool.h
    stddef.h
    stdint.h
    stdio.h
    stdlib.h
    stdnoreturn.h
    string.h
    tgmath.h
    threads.h
    time.h
    uchar.h
    wchar.h
    wctype.h)
set(CXX_HEADERS
    ${CXX_HEADERS}
    cassert
    cctype
    cerrno
    cfenv
    cfloat
    cinttypes
    ciso646
    climits
    clocale
    cmath
    csetjmp
    csignal
    cstdalign
    cstdarg
    cstdatomic
    cstdbool
    cstddef
    cstdint
    cstdio
    cstdlib
    cstring
    cthreads
    ctime)

# POSIX and compiler headers a public header may need on one platform.
set(PLATFORM_HEADERS fcntl.h intrin.h io.h mpi.h pthread.h sched.h unistd.h)

# Families of system headers, by directory.
set(SYSTEM_PREFIXES arpa/ netinet/ sanitizer/ sys/)

# Known exceptions until the API cut.
set(EXCEPTIONS simdjson.h)
set(EXCEPTION_PREFIXES nanoarrow/)

# Appends "<file>:<line>: #include <header>" to out_hits for every include that
# is not allowed, and counts them in out_count. The vendored folder is skipped.
function(dftu_public_header_hits include_root out_hits out_count)
  file(GLOB_RECURSE _files "${include_root}/*.h" "${include_root}/*.hpp"
       "${include_root}/*.inl")
  list(FILTER _files EXCLUDE REGEX "/${VENDOR_ROOT}/")
  list(SORT _files)

  set(_hits "")
  set(_count 0)
  foreach(_file IN LISTS _files)
    # Split on newlines only: file(STRINGS) would also split a line at ";",
    # which shifts the line numbers it reports.
    file(READ "${_file}" _content)
    string(REPLACE ";" "\\;" _content "${_content}")
    string(REPLACE "\n" ";" _lines "${_content}")
    set(_lineno 0)
    foreach(_line IN LISTS _lines)
      math(EXPR _lineno "${_lineno} + 1")
      if(NOT _line MATCHES "^[ \t]*#[ \t]*include[ \t]*<([^>]+)>")
        continue()
      endif()
      set(_header "${CMAKE_MATCH_1}")
      set(_allowed FALSE)
      if(_header MATCHES "^dftracer/")
        set(_allowed TRUE)
      elseif(_header IN_LIST CXX_HEADERS
             OR _header IN_LIST C_HEADERS
             OR _header IN_LIST PLATFORM_HEADERS
             OR _header IN_LIST EXCEPTIONS)
        set(_allowed TRUE)
      else()
        foreach(_prefix IN LISTS SYSTEM_PREFIXES EXCEPTION_PREFIXES)
          if(_header MATCHES "^${_prefix}")
            set(_allowed TRUE)
          endif()
        endforeach()
      endif()
      if(NOT _allowed)
        string(APPEND _hits "${_file}:${_lineno}: #include <${_header}>\n")
        math(EXPR _count "${_count} + 1")
      endif()
    endforeach()
  endforeach()

  set(${out_hits} "${_hits}" PARENT_SCOPE)
  set(${out_count} "${_count}" PARENT_SCOPE)
endfunction()

if(SELF_TEST)
  set(_dir "${CMAKE_CURRENT_BINARY_DIR}/public_third_party_headers_self")
  file(REMOVE_RECURSE "${_dir}")
  set(_allowed_root "${_dir}/allowed/include")
  file(WRITE "${_allowed_root}/good/good.h"
       "#include <dftracer/utils/dstd/hash_map.h>\n"
       "#include <vector>\n"
       "#include <stdint.h>\n"
       "#include <sys/types.h>\n"
       "#include <sanitizer/asan_interface.h>\n"
       "#include <mpi.h>\n"
       "#include <simdjson.h>\n"
       "#include <nanoarrow/nanoarrow.h>\n")
  set(_rejected_root "${_dir}/rejected/include")
  file(WRITE "${_rejected_root}/bad/bad.h"
       "#ifndef BAD_H\n"
       "#define BAD_H 1  // a; b keeps the line number right\n"
       "#include <ankerl/unordered_dense.h>\n"
       "#include <tl/expected.hpp>\n"
       "#endif\n")
  # Vendored text may include anything; it is not scanned.
  file(WRITE "${_rejected_root}/${VENDOR_ROOT}/vendored.h"
       "#include <ankerl/unordered_dense.h>\n")

  dftu_public_header_hits("${_allowed_root}" _hits _count)
  if(NOT _count EQUAL 0)
    message(FATAL_ERROR "public_third_party_headers self test: the allowed "
                        "includes were rejected:\n${_hits}")
  endif()

  dftu_public_header_hits("${_rejected_root}" _hits _count)
  if(NOT _count EQUAL 2)
    message(FATAL_ERROR "public_third_party_headers self test: expected 2 hits "
                        "in bad/bad.h, got ${_count}:\n${_hits}")
  endif()
  if(NOT _hits MATCHES "bad/bad\\.h:3: #include <ankerl/unordered_dense\\.h>"
     OR NOT _hits MATCHES "bad/bad\\.h:4: #include <tl/expected\\.hpp>")
    message(FATAL_ERROR "public_third_party_headers self test: wrong hits:\n${_hits}")
  endif()
  file(REMOVE_RECURSE "${_dir}")
  message(
    STATUS
      "public_third_party_headers_self: 8 allowed includes and 2 hits as expected"
  )
  return()
endif()

dftu_public_header_hits("${SOURCE_DIR}/include" HITS COUNT)
if(COUNT GREATER 0)
  string(REPLACE "${SOURCE_DIR}/" "" HITS "${HITS}")
  message("${HITS}")
  message(FATAL_ERROR "public_third_party_headers: ${COUNT} third-party "
                      "include(s) in a public header. Vendor a single header in "
                      "include/dftracer/utils/dstd/vendor/, or use the library "
                      "from src/ only.")
endif()
message(STATUS "public_third_party_headers: every include in include/ is ours, "
               "standard, system, mpi, or vendored")
