# SPDX-License-Identifier: MIT
# Fails on extern "C" in include/ and src/ outside the C surface. Allowed:
#   - c_api*.h and c_api*.cpp: the C API of each library.
#   - include/dftracer/utils/plugins/ and src/dftracer/utils/plugins/: the
#     plugin ABI headers and the host that defines the callbacks behind them.
#   - src/dftracer/utils/cli/plugin_templates/: the plugin scaffolds `dftracer
#     plugin new` writes. They are plugin code, embedded as bytes and never
#     compiled here.
#   - include/dftracer/utils/dstd/vendor/: vendored upstream headers.
#   - include/dftracer/utils/common/platform_compat.h: the TSan runtime hooks,
#     which are C symbols of the sanitizer.
# Comments are not checked.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_extern_c_only_in_c_api.cmake
#        cmake -DSOURCE_DIR=<repo root> -DSELF_TEST=ON -P check_extern_c_only_in_c_api.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "check_extern_c_only_in_c_api.cmake requires -DSOURCE_DIR=...")
endif()

set(_allowed_prefixes
    include/dftracer/utils/plugins/ src/dftracer/utils/plugins/
    src/dftracer/utils/cli/plugin_templates/
    include/dftracer/utils/dstd/vendor/
    include/dftracer/utils/common/platform_compat.h)

function(_check_tree root out_hits)
  set(hits "")
  foreach(dir IN ITEMS include src)
    if(NOT IS_DIRECTORY "${root}/${dir}")
      continue()
    endif()
    file(GLOB_RECURSE files "${root}/${dir}/*.h" "${root}/${dir}/*.hpp"
         "${root}/${dir}/*.inl" "${root}/${dir}/*.c" "${root}/${dir}/*.cc"
         "${root}/${dir}/*.cpp")
    foreach(file IN LISTS files)
      string(REPLACE "${root}/" "" relative "${file}")
      get_filename_component(name "${file}" NAME)
      if(name MATCHES "^c_api[A-Za-z0-9_]*\\.(h|cpp)$")
        continue()
      endif()
      set(skip FALSE)
      foreach(prefix IN LISTS _allowed_prefixes)
        string(FIND "${relative}" "${prefix}" at)
        if(at EQUAL 0)
          set(skip TRUE)
        endif()
      endforeach()
      if(skip)
        continue()
      endif()
      file(READ "${file}" text)
      string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " text "${text}")
      string(REGEX REPLACE "//[^\n]*" "" text "${text}")
      if(text MATCHES "extern[ \t\n]*\"C\"")
        list(APPEND hits "${relative}")
      endif()
    endforeach()
  endforeach()
  set(${out_hits} "${hits}" PARENT_SCOPE)
endfunction()

if(SELF_TEST)
  set(work "${CMAKE_CURRENT_BINARY_DIR}/extern_c_only_in_c_api_self")
  file(REMOVE_RECURSE "${work}")
  file(WRITE "${work}/src/lib/clean.cpp" "// extern \"C\" in a comment\nint f() { return 1; }\n")
  file(WRITE "${work}/src/lib/c_api.cpp" "extern \"C\" int g() { return 1; }\n")
  file(WRITE "${work}/include/dftracer/utils/plugins/abi.h" "extern \"C\" {\n}\n")
  _check_tree("${work}" hits)
  if(hits)
    message(FATAL_ERROR "self test: an allowed or clean file was reported: ${hits}")
  endif()
  file(WRITE "${work}/src/lib/bad.cpp" "extern \"C\" {\nint h() { return 1; }\n}\n")
  _check_tree("${work}" hits)
  if(NOT hits)
    message(FATAL_ERROR "self test: an extern \"C\" outside the C API was not reported")
  endif()
  file(REMOVE_RECURSE "${work}")
  message(STATUS "extern_c_only_in_c_api self test passed")
  return()
endif()

_check_tree("${SOURCE_DIR}" hits)
if(hits)
  list(JOIN hits "\n" joined)
  message(FATAL_ERROR "extern \"C\" outside c_api*.h, c_api*.cpp and the plugin boundary:\n${joined}")
endif()
