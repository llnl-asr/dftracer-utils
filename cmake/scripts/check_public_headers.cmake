# SPDX-License-Identifier: MIT
# Fails when include/ and the checked-in list cmake/public_headers.txt disagree,
# or when a public header only includes other headers. A file under include/ is
# public on purpose: a new one is added to the list in the same change, a moved
# one leaves it. An umbrella header that forwards includes is not a public
# header, except the two named below.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_public_headers.cmake
#        cmake -DSOURCE_DIR=<repo root> -DSELF_TEST=ON -P check_public_headers.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "check_public_headers.cmake requires -DSOURCE_DIR=...")
endif()

# Relative to include/.
set(UMBRELLA_HEADERS dftracer/utils/plugins/abi.h
                     dftracer/utils/plugins/plugin.h)

# True when `content` has nothing but comments, blank lines, include guards,
# conditionals and #include lines.
function(dftu_only_includes content out_var)
  string(REPLACE ";" "," content "${content}")
  string(REPLACE "[" "(" content "${content}")
  string(REPLACE "]" ")" content "${content}")
  string(REPLACE "\n" ";" lines "${content}")
  set(in_block FALSE)
  set(has_include FALSE)
  foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(in_block)
      if(line MATCHES "\\*/")
        set(in_block FALSE)
      endif()
      continue()
    endif()
    if(line STREQUAL "" OR line MATCHES "^//")
      continue()
    endif()
    if(line MATCHES "^/\\*")
      if(NOT line MATCHES "\\*/")
        set(in_block TRUE)
      endif()
      continue()
    endif()
    if(line MATCHES "^#[ \t]*include")
      set(has_include TRUE)
      continue()
    endif()
    if(line MATCHES "^#[ \t]*(ifndef|ifdef|if|elif|else|endif|pragma once)"
       OR line MATCHES "^#[ \t]*define[ \t]+[A-Za-z0-9_]+[ \t]*(//.*)?$")
      continue()
    endif()
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endforeach()
  set(${out_var} ${has_include} PARENT_SCOPE)
endfunction()

# Appends one line per problem to out_problems and counts them in out_count.
function(dftu_public_headers_problems include_root list_file out_problems
         out_count)
  file(GLOB_RECURSE actual RELATIVE "${include_root}" "${include_root}/*")
  list(SORT actual)
  file(STRINGS "${list_file}" listed)
  list(FILTER listed EXCLUDE REGEX "^[ \t]*$")
  list(SORT listed)

  set(problems "")
  set(count 0)
  foreach(path IN LISTS actual)
    list(FIND listed "${path}" index)
    if(index EQUAL -1)
      string(APPEND problems
             "${path}: in include/ but not in cmake/public_headers.txt\n")
      math(EXPR count "${count} + 1")
    endif()
  endforeach()
  foreach(path IN LISTS listed)
    if(NOT EXISTS "${include_root}/${path}")
      string(APPEND problems
             "${path}: in cmake/public_headers.txt but not in include/\n")
      math(EXPR count "${count} + 1")
    endif()
  endforeach()
  foreach(path IN LISTS actual)
    if(NOT path MATCHES "\\.(h|hpp)$")
      continue()
    endif()
    list(FIND UMBRELLA_HEADERS "${path}" index)
    if(NOT index EQUAL -1)
      continue()
    endif()
    file(READ "${include_root}/${path}" content)
    dftu_only_includes("${content}" only_includes)
    if(only_includes)
      string(APPEND problems "${path}: the header only includes other headers\n")
      math(EXPR count "${count} + 1")
    endif()
  endforeach()

  set(${out_problems} "${problems}" PARENT_SCOPE)
  set(${out_count} "${count}" PARENT_SCOPE)
endfunction()

if(SELF_TEST)
  set(dir "${CMAKE_CURRENT_BINARY_DIR}/public_headers_self")
  file(REMOVE_RECURSE "${dir}")

  set(good "${dir}/good/include")
  file(WRITE "${good}/dftracer/utils/a/real.h"
       "#ifndef REAL_H\n#define REAL_H\n#include <vector>\nstruct Real {};\n#endif\n")
  file(WRITE "${good}/dftracer/utils/a/table.def" "ROW(1)\n")
  file(WRITE "${good}/dftracer/utils/plugins/abi.h"
       "// SPDX\n#ifndef ABI_H\n#define ABI_H\n#include <vector>\n#endif\n")
  file(WRITE "${good}/dftracer/utils/plugins/plugin.h"
       "#include <vector>\n")
  file(WRITE "${dir}/good/list.txt"
       "dftracer/utils/a/real.h\ndftracer/utils/a/table.def\n"
       "dftracer/utils/plugins/abi.h\ndftracer/utils/plugins/plugin.h\n")
  dftu_public_headers_problems("${good}" "${dir}/good/list.txt" problems count)
  if(NOT count EQUAL 0)
    message(FATAL_ERROR "public_headers self test: a clean tree was rejected:\n"
                        "${problems}")
  endif()

  set(bad "${dir}/bad/include")
  file(WRITE "${bad}/dftracer/utils/a/real.h" "struct Real {};\n")
  file(WRITE "${bad}/dftracer/utils/a/stray.h" "struct Stray {};\n")
  file(WRITE "${bad}/dftracer/utils/a/forward.h"
       "#ifndef FORWARD_H\n#define FORWARD_H\n/* forwards\n   the rest */\n"
       "#include <dftracer/utils/a/real.h>\n#endif  // FORWARD_H\n")
  file(WRITE "${dir}/bad/list.txt"
       "dftracer/utils/a/forward.h\ndftracer/utils/a/gone.h\n"
       "dftracer/utils/a/real.h\n")
  dftu_public_headers_problems("${bad}" "${dir}/bad/list.txt" problems count)
  if(NOT count EQUAL 3)
    message(FATAL_ERROR "public_headers self test: expected 3 problems, got "
                        "${count}:\n${problems}")
  endif()
  if(NOT problems MATCHES "a/stray\\.h: in include/ but not in"
     OR NOT problems MATCHES "a/gone\\.h: in cmake/public_headers\\.txt but not"
     OR NOT problems MATCHES "a/forward\\.h: the header only includes")
    message(FATAL_ERROR "public_headers self test: wrong problems:\n"
                        "${problems}")
  endif()
  file(REMOVE_RECURSE "${dir}")
  message(STATUS "public_headers_self: a clean tree and 3 problems as expected")
  return()
endif()

dftu_public_headers_problems("${SOURCE_DIR}/include"
                             "${SOURCE_DIR}/cmake/public_headers.txt" PROBLEMS
                             COUNT)
if(COUNT GREATER 0)
  message("${PROBLEMS}")
  message(FATAL_ERROR "public_headers: ${COUNT} problem(s). Update "
                      "cmake/public_headers.txt in the change that adds or "
                      "moves a public header; move a header that no outside "
                      "user needs to src/.")
endif()
message(STATUS "public_headers: include/ matches cmake/public_headers.txt")
