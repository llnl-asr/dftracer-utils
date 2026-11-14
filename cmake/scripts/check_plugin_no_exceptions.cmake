# SPDX-License-Identifier: MIT
# The plugin SDK holds no throw, try or catch: every SDK header, and the example
# plugins that use no exception of their own, compile with -fno-exceptions and
# -Werror. One source file per SDK header holding only that include.
#
# Usage: cmake -DSOURCE_DIR=<repo> -DBUILD_DIR=<build tree> -DWORK_DIR=<scratch>
#              -DGENERATOR=... -DC_COMPILER=... -DCXX_COMPILER=...
#              -DCXX_FLAGS=... -P this file

cmake_minimum_required(VERSION 3.20)

foreach(var SOURCE_DIR BUILD_DIR WORK_DIR GENERATOR C_COMPILER CXX_COMPILER)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "check_plugin_no_exceptions.cmake requires -D${var}=...")
  endif()
endforeach()

set(PROJECT_DIR "${WORK_DIR}/project")
file(REMOVE_RECURSE "${WORK_DIR}")

set(header_root "${SOURCE_DIR}/include")
file(GLOB sdk_headers RELATIVE "${header_root}"
     "${header_root}/dftracer/utils/plugins/plugin/*.h")
list(APPEND sdk_headers "dftracer/utils/plugins/plugin.h")
list(SORT sdk_headers)

set(sources "")
foreach(header IN LISTS sdk_headers)
  string(MAKE_C_IDENTIFIER "${header}" id)
  set(source "${PROJECT_DIR}/src/${id}.cpp")
  file(WRITE "${source}" "#include <${header}>\n")
  list(APPEND sources "${source}")
endforeach()
list(APPEND sources "${SOURCE_DIR}/examples/plugins/duration_report.cpp"
     "${SOURCE_DIR}/examples/plugins/duration_histogram.cpp"
     "${SOURCE_DIR}/examples/plugins/query_filter.cpp"
     "${SOURCE_DIR}/examples/plugins/duration_udfs.cpp"
     "${SOURCE_DIR}/examples/plugins/stats_udafs.cpp")

file(WRITE "${PROJECT_DIR}/CMakeLists.txt"
     "cmake_minimum_required(VERSION 3.20)\n"
     "project(plugin_no_exceptions LANGUAGES CXX)\n"
     "set(CMAKE_CXX_STANDARD 20)\n"
     "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
     "set(CMAKE_CXX_EXTENSIONS OFF)\n"
     "add_library(sdk OBJECT ${sources})\n"
     "target_include_directories(sdk PRIVATE \"${header_root}\" "
     "\"${BUILD_DIR}/include\")\n"
     "target_compile_options(sdk PRIVATE -fno-exceptions -Wall -Wextra -Werror)\n")

execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -S "${PROJECT_DIR}" -B "${WORK_DIR}/build" -G
    "${GENERATOR}" "-DCMAKE_C_COMPILER=${C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
  OUTPUT_QUIET
  RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "the no-exceptions project does not configure: ${rc}")
endif()

if(GENERATOR MATCHES "Ninja")
  set(keep_going -k 0)
else()
  set(keep_going -k)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK_DIR}/build"
                        --parallel -- ${keep_going}
                RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "a plugin SDK header or example does not compile with "
                      "-fno-exceptions: ${rc}")
endif()
list(LENGTH sdk_headers header_count)
message(STATUS "plugin_no_exceptions: ${header_count} SDK headers and 3 "
               "examples compile with -fno-exceptions")
