cmake_minimum_required(VERSION 3.20)
project(zdefs LANGUAGES CXX)

set(CMAKE_INSTALL_LIBDIR lib)
set(CMAKE_INSTALL_BINDIR bin)
set(CMAKE_INSTALL_INCLUDEDIR include)
set(DFTRACER_UTILS_API_VERSION 1.0.0)
set(DFTRACER_UTILS_ABI_VERSION 1.0.0)

include(${ZDEFS_ROOT}/cmake/modules/Utils.cmake)
include(${ZDEFS_ROOT}/cmake/modules/Rpath.cmake)
include(${ZDEFS_ROOT}/cmake/modules/LibraryHelpers.cmake)

create_library(
  NAME
  zdefs_undefined
  NAMESPACE
  zdefs
  VERSION
  1.0.0
  SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/undefined.cpp
  BUILD_SHARED
  ON
  BUILD_STATIC
  OFF)
