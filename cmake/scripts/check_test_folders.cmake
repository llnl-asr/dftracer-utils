# SPDX-License-Identifier: MIT
# Check that every folder directly under tests/ is the folder of a library in
# src/dftracer/utils/, or one of the folders that hold no library tests.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_test_folders.cmake

cmake_policy(SET CMP0057 NEW)

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "check_test_folders.cmake requires -DSOURCE_DIR=...")
endif()

set(non_library_folders binaries python layout layering tsan ubsan valgrind
                        zdefs bench cmake package_consumer)

file(GLOB library_dirs LIST_DIRECTORIES true RELATIVE
     "${SOURCE_DIR}/src/dftracer/utils" "${SOURCE_DIR}/src/dftracer/utils/*")
set(libraries "")
foreach(dir ${library_dirs})
  if(IS_DIRECTORY "${SOURCE_DIR}/src/dftracer/utils/${dir}")
    list(APPEND libraries ${dir})
  endif()
endforeach()
if(NOT libraries)
  message(FATAL_ERROR "test_folders: no library folders under ${SOURCE_DIR}/src/dftracer/utils")
endif()

file(GLOB test_dirs LIST_DIRECTORIES true RELATIVE "${SOURCE_DIR}/tests"
     "${SOURCE_DIR}/tests/*")
set(errors "")
foreach(dir ${test_dirs})
  if(NOT IS_DIRECTORY "${SOURCE_DIR}/tests/${dir}")
    continue()
  endif()
  if(dir IN_LIST libraries OR dir IN_LIST non_library_folders)
    continue()
  endif()
  string(APPEND errors "  tests/${dir}\n")
endforeach()

if(errors)
  message(FATAL_ERROR
          "test_folders: these folders under tests/ are neither a library folder "
          "of src/dftracer/utils/ nor one of: ${non_library_folders}\n${errors}")
endif()
message(STATUS "test_folders: ok")
