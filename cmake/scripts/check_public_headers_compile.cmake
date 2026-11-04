# SPDX-License-Identifier: MIT
# Installs the build into a scratch prefix, then compiles every installed header
# alone: one source file per header holding only that include, C++20, with the
# prefix as the only include path. A header that needs src/ or a third-party
# include fails here. The abi headers also compile as plain C.
#
# Usage: cmake -DBUILD_DIR=<build tree> -DWORK_DIR=<scratch dir> -DGENERATOR=...
#              -DC_COMPILER=... -DCXX_COMPILER=... -DCXX_FLAGS=... -P this file

cmake_minimum_required(VERSION 3.20)

foreach(var BUILD_DIR WORK_DIR GENERATOR C_COMPILER CXX_COMPILER)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "check_public_headers_compile.cmake requires -D${var}=...")
  endif()
endforeach()

set(PREFIX "${WORK_DIR}/prefix")
set(PROJECT_DIR "${WORK_DIR}/project")
file(REMOVE_RECURSE "${WORK_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}"
  OUTPUT_QUIET
  RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "the install into ${PREFIX} failed: ${rc}")
endif()

set(HEADER_ROOT "${PREFIX}/include")
file(GLOB_RECURSE headers RELATIVE "${HEADER_ROOT}" "${HEADER_ROOT}/dftracer/*.h"
     "${HEADER_ROOT}/dftracer/*.hpp")
list(SORT headers)
if(NOT headers)
  message(FATAL_ERROR "the install put no header under ${HEADER_ROOT}")
endif()

set(cxx_sources "")
set(c_sources "")
foreach(header IN LISTS headers)
  string(MAKE_C_IDENTIFIER "${header}" id)
  set(source "${PROJECT_DIR}/src/${id}.cpp")
  file(WRITE "${source}" "#include <${header}>\n")
  list(APPEND cxx_sources "${source}")
  if(header MATCHES "abi\\.h$|/abi/[^/]+\\.h$")
    set(source "${PROJECT_DIR}/src/${id}.c")
    file(WRITE "${source}" "#include <${header}>\n")
    list(APPEND c_sources "${source}")
  endif()
endforeach()
list(LENGTH headers header_count)
list(LENGTH c_sources c_count)

file(WRITE "${PROJECT_DIR}/CMakeLists.txt"
     "cmake_minimum_required(VERSION 3.20)\n"
     "project(public_headers_compile LANGUAGES C CXX)\n"
     "set(CMAKE_CXX_STANDARD 20)\n"
     "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
     "set(CMAKE_CXX_EXTENSIONS OFF)\n"
     "set(CMAKE_C_STANDARD 11)\n"
     "add_library(headers_cxx OBJECT ${cxx_sources})\n"
     "target_include_directories(headers_cxx PRIVATE \"${HEADER_ROOT}\")\n")
if(c_sources)
  file(APPEND "${PROJECT_DIR}/CMakeLists.txt"
       "add_library(headers_c OBJECT ${c_sources})\n"
       "target_include_directories(headers_c PRIVATE \"${HEADER_ROOT}\")\n")
endif()

execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -S "${PROJECT_DIR}" -B "${WORK_DIR}/build" -G
    "${GENERATOR}" "-DCMAKE_C_COMPILER=${C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
  OUTPUT_QUIET
  RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "the header project does not configure: ${rc}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK_DIR}/build"
                        --parallel
                RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "an installed header does not compile on its own: ${rc}")
endif()
message(STATUS "public_headers_compile: ${header_count} installed headers "
               "compile alone, ${c_count} of them as C")
