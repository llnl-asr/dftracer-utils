# SPDX-License-Identifier: MIT
# Fails on exception code outside the Python bindings: throw, try, catch,
# std::exception_ptr, std::current_exception, std::rethrow_exception,
# bad_alloc, doctest's CHECK_THROWS, REQUIRE_THROWS and WARN_THROWS families, and a
# -fexceptions flag in a CMake file. Comments and string literals are not
# checked. Not checked: src/dftracer/utils/python/, the vendored headers in
# include/dftracer/utils/dstd/vendor/ and tests/python/.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_no_exceptions.cmake
#        cmake -DSOURCE_DIR=<repo root> -DSELF_TEST=ON -P check_no_exceptions.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "check_no_exceptions.cmake requires -DSOURCE_DIR=...")
endif()

set(_not_word "[^A-Za-z0-9_]")
set(_patterns
    "(^|${_not_word})throw(${_not_word}|$)"
    "(^|${_not_word})try[ \t\n]*[{]"
    "(^|${_not_word})catch[ \t\n]*[(]"
    "exception_ptr"
    "current_exception"
    "rethrow_exception"
    "(^|${_not_word})bad_alloc(${_not_word}|$)"
    "(CHECK|REQUIRE|WARN)_THROWS")

function(_check_tree root excluded out_hits)
  set(hits "")
  foreach(dir IN ITEMS include src tests benchmarks)
    if(NOT IS_DIRECTORY "${root}/${dir}")
      continue()
    endif()
    file(GLOB_RECURSE files "${root}/${dir}/*.h" "${root}/${dir}/*.hpp"
         "${root}/${dir}/*.inl" "${root}/${dir}/*.c" "${root}/${dir}/*.cc"
         "${root}/${dir}/*.cpp")
    foreach(file IN LISTS files)
      set(skip FALSE)
      foreach(prefix IN LISTS excluded)
        string(FIND "${file}" "${root}/${prefix}" at)
        if(at EQUAL 0)
          set(skip TRUE)
        endif()
      endforeach()
      if(skip)
        continue()
      endif()
      file(READ "${file}" text)
      string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " text "${text}")
      string(REGEX REPLACE "\"([^\"\\\\\n]|\\\\.)*\"" "\"\"" text "${text}")
      string(REGEX REPLACE "//[^\n]*" "" text "${text}")
      foreach(pattern IN LISTS _patterns)
        if(text MATCHES "${pattern}")
          string(REPLACE "${root}/" "" relative "${file}")
          list(APPEND hits "${relative}: matches ${pattern}")
        endif()
      endforeach()
    endforeach()
  endforeach()

  file(GLOB_RECURSE cmake_files "${root}/src/*.txt" "${root}/tests/*.txt"
       "${root}/benchmarks/*.txt" "${root}/cmake/*.cmake")
  if(EXISTS "${root}/CMakeLists.txt")
    list(APPEND cmake_files "${root}/CMakeLists.txt")
  endif()
  foreach(file IN LISTS cmake_files)
    string(FIND "${file}" "${root}/cmake/scripts/check_no_exceptions.cmake" at)
    if(NOT at EQUAL -1)
      continue()
    endif()
    string(FIND "${file}" "${root}/src/dftracer/utils/python/" at)
    if(at EQUAL 0)
      continue()
    endif()
    file(READ "${file}" text)
    if(text MATCHES "-fexceptions")
      string(REPLACE "${root}/" "" relative "${file}")
      list(APPEND hits "${relative}: -fexceptions")
    endif()
  endforeach()
  set(${out_hits} "${hits}" PARENT_SCOPE)
endfunction()

set(_excluded src/dftracer/utils/python/ include/dftracer/utils/dstd/vendor/
              tests/python/)

if(SELF_TEST)
  set(work "${CMAKE_CURRENT_BINARY_DIR}/no_exceptions_self")
  file(REMOVE_RECURSE "${work}")
  file(WRITE "${work}/src/clean.cpp"
       "// throw in a comment\nconst char* s = \"throw\";\nint f() { return 1; }\n")
  _check_tree("${work}" "" hits)
  if(hits)
    message(FATAL_ERROR "self test: a comment or string was reported: ${hits}")
  endif()
  file(WRITE "${work}/src/bad.cpp"
       "int f() { try { return 1; } catch (...) { throw; } }\n")
  _check_tree("${work}" "" hits)
  if(NOT hits)
    message(FATAL_ERROR "self test: a throw was not reported")
  endif()
  file(REMOVE_RECURSE "${work}")
  message(STATUS "no_exceptions self test passed")
  return()
endif()

_check_tree("${SOURCE_DIR}" "${_excluded}" hits)
if(hits)
  list(JOIN hits "\n" joined)
  message(FATAL_ERROR "exception code outside the Python bindings:\n${joined}")
endif()
