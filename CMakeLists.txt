cmake_minimum_required(VERSION 3.20)

set(DFTRACER_UTILS_VERSION_MAJOR 0)
set(DFTRACER_UTILS_VERSION_MINOR 1)
set(DFTRACER_UTILS_VERSION_PATCH 0)
set(DFTRACER_UTILS_VERSION
    "${DFTRACER_UTILS_VERSION_MAJOR}.${DFTRACER_UTILS_VERSION_MINOR}.${DFTRACER_UTILS_VERSION_PATCH}"
)

# Use Ninja as default generator if available and not already specified
if(NOT DEFINED ENV{CMAKE_GENERATOR} AND NOT CMAKE_GENERATOR)
  find_program(NINJA_EXECUTABLE ninja)
  if(NINJA_EXECUTABLE)
    set(CMAKE_GENERATOR
        "Ninja"
        CACHE INTERNAL "Default generator" FORCE)
    message(STATUS "Using Ninja generator")
  endif()
endif()

project(
  dftracer_utils
  VERSION ${DFTRACER_UTILS_VERSION}
  LANGUAGES C CXX)

set(DFTRACER_UTILS_PACKAGE ${PROJECT_NAME})
set(DFTRACER_UTILS_PACKAGE_NAME ${PROJECT_NAME})
set(DFTRACER_UTILS_PACKAGE_VERSION "${PROJECT_VERSION}")
set(DFTRACER_UTILS_PACKAGE_VERSION_MAJOR
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
set(DFTRACER_UTILS_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_PATCH}")
set(DFTRACER_UTILS_PACKAGE_STRING
    "${DFTRACER_UTILS_PACKAGE_NAME} ${DFTRACER_UTILS_PACKAGE_VERSION}")
set(DFTRACER_UTILS_PACKAGE_TARNAME "${DFTRACER_UTILS_PACKAGE}")
set(DEPENDENCY_LIBRARY_DIRS "")

# ##############################################################################
# Options
# ##############################################################################

option(DFTRACER_UTILS_TESTS "Build tests" OFF)
option(DFTRACER_UTILS_COVERAGE "Enable coverage reporting" OFF)
option(DFTRACER_UTILS_DEBUG "Enable debug mode including verbose logging" OFF)
option(DFTRACER_UTILS_BUILD_SHARED "Build shared library" ON)
option(DFTRACER_UTILS_BUILD_STATIC "Build static library" ON)
option(DFTRACER_UTILS_BUILD_BINARIES "Build command-line binaries" ON)
option(DFTRACER_UTILS_BUILD_PYTHON "Build Python bindings" OFF)

if(DFTRACER_UTILS_TESTS)
  message(STATUS "Building tests")
  set(DFTRACER_UTILS_TESTS
      ON
      CACHE BOOL "" FORCE)
  set(DFTRACER_UTILS_COVERAGE
      ON
      CACHE BOOL "" FORCE)
  set(DFTRACER_UTILS_DEBUG
      ON
      CACHE BOOL "" FORCE)
  set(DFTRACER_UTILS_BUILD_SHARED
      ON
      CACHE BOOL "" FORCE)
  set(DFTRACER_UTILS_BUILD_STATIC
      ON
      CACHE BOOL "" FORCE)
  set(DFTRACER_UTILS_BUILD_BINARIES
      ON
      CACHE BOOL "" FORCE)
endif()

# Validation: Ensure at least one library type is enabled
if(NOT DFTRACER_UTILS_BUILD_SHARED AND NOT DFTRACER_UTILS_BUILD_STATIC)
  message(
    FATAL_ERROR
      "At least one of DFTRACER_UTILS_BUILD_SHARED or DFTRACER_UTILS_BUILD_STATIC must be enabled"
  )
endif()

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Enable optimization

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ##############################################################################
# Internal Paths for cmake libraries and Setup install and output Directories
# ##############################################################################

# This sets where to look for dependent libraries
set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${CMAKE_BINARY_DIR}
                      ${CMAKE_INSTALL_PREFIX})
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/CompilerWarnings.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/LibraryHelpers.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/InstallHelpers.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/CPM.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/Dependencies.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/Utils.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/Rpath.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/PythonHelpers.cmake)

if(SKBUILD)
  set(CMAKE_INSTALL_LIBDIR
      "dftracer/lib"
      CACHE STRING "Library install directory" FORCE)
  set(CMAKE_INSTALL_BINDIR
      # "${SKBUILD_SCRIPTS_DIR}"
      "dftracer/bin"
      CACHE STRING "Binary install directory" FORCE)
  set(CMAKE_INSTALL_VENV_BIN_DIR
      "${SKBUILD_SCRIPTS_DIR}"
      CACHE STRING "Virtualenv build directory" FORCE)
  set(CMAKE_INSTALL_INCLUDEDIR
      "dftracer/include"
      CACHE STRING "Include install directory" FORCE)
endif()

if(DFTRACER_UTILS_DEBUG)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/configure_files/dftracer_utils_config.dbg.h.in"
    "${CMAKE_BINARY_DIR}/include/dftracer/utils/core/common/config.h"
    @ONLY)
else()
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/configure_files/dftracer_utils_config.h.in"
    "${CMAKE_BINARY_DIR}/include/dftracer/utils/core/common/config.h"
    @ONLY)
endif()

set_coverage_compiler_flags(DFTRACER_UTILS_TESTS AND DFTRACER_UTILS_COVERAGE)

check_std_filesystem()
add_subdirectory(src)

if(DFTRACER_UTILS_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
