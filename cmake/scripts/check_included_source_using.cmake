# SPDX-License-Identifier: MIT
# Fails when a source file (.cpp, .cc, .c) that another file includes holds a
# using-directive. Such a file acts as a header, so the directive leaks into
# the file that includes it, the same as a using-directive in a header.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_included_source_using.cmake

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "check_included_source_using.cmake requires -DSOURCE_DIR=...")
endif()

set(_roots include src tests benchmarks examples)
set(_globs)
foreach(_root IN LISTS _roots)
  foreach(_ext h hpp inl cpp cc c)
    list(APPEND _globs "${SOURCE_DIR}/${_root}/*.${_ext}")
  endforeach()
endforeach()
file(GLOB_RECURSE _files ${_globs})
list(FILTER _files EXCLUDE REGEX "/dftracer/utils/dstd/vendor/")

set(_included)
foreach(_f IN LISTS _files)
  file(STRINGS "${_f}" _lines REGEX "^[ \t]*#[ \t]*include[ \t]*<[^>]+\\.(cpp|cc|c)>")
  foreach(_line IN LISTS _lines)
    string(REGEX REPLACE "^[^<]*<([^>]+)>.*$" "\\1" _path "${_line}")
    list(APPEND _included "${_path}")
  endforeach()
endforeach()
list(REMOVE_DUPLICATES _included)

set(_bad "")
foreach(_path IN LISTS _included)
  foreach(_root IN LISTS _roots)
    set(_full "${SOURCE_DIR}/${_root}/${_path}")
    if(EXISTS "${_full}")
      file(STRINGS "${_full}" _using REGEX "^[ \t]*using[ \t]+namespace[ \t]")
      if(_using)
        string(APPEND _bad "  ${_root}/${_path}\n")
      endif()
    endif()
  endforeach()
endforeach()

if(_bad)
  message("using namespace in a source file that another file includes:\n${_bad}"
          "qualify the names, as in a header.")
  message(FATAL_ERROR "included_source_using: failed")
endif()
