cmake_minimum_required(VERSION 3.20)

# Allow dependencies with older CMake minimum versions
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(APPLE
   AND DEFINED ENV{MACOSX_DEPLOYMENT_TARGET}
   AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET
      "$ENV{MACOSX_DEPLOYMENT_TARGET}"
      CACHE STRING "Minimum macOS version" FORCE)
endif()

set(DFTRACER_UTILS_VERSION_MAJOR 0)
set(DFTRACER_UTILS_VERSION_MINOR 0)
set(DFTRACER_UTILS_VERSION_PATCH 10)
set(DFTRACER_UTILS_VERSION
    "${DFTRACER_UTILS_VERSION_MAJOR}.${DFTRACER_UTILS_VERSION_MINOR}.${DFTRACER_UTILS_VERSION_PATCH}"
)

# Pretty-print helpers (dftracer_utils_status/ok/warn/section/item). Included
# early so even pre-project status output can use it.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/Pretty.cmake)

# Use Ninja as default generator if available and not already specified
if(NOT DEFINED ENV{CMAKE_GENERATOR} AND NOT CMAKE_GENERATOR)
  find_program(NINJA_EXECUTABLE ninja)
  if(NINJA_EXECUTABLE)
    set(CMAKE_GENERATOR
        "Ninja"
        CACHE INTERNAL "Default generator" FORCE)
    dftracer_utils_ok("Using Ninja generator")
  endif()
endif()

project(
  dftracer_utils
  VERSION ${DFTRACER_UTILS_VERSION}
  LANGUAGES C CXX)

find_program(CCACHE_EXECUTABLE ccache)
if(CCACHE_EXECUTABLE)
  foreach(lang C CXX ASM)
    if(NOT CMAKE_${lang}_COMPILER_LAUNCHER)
      set(CMAKE_${lang}_COMPILER_LAUNCHER
          "${CCACHE_EXECUTABLE}"
          CACHE STRING "Compiler launcher for ${lang}" FORCE)
    endif()
  endforeach()
  dftracer_utils_ok("Using ccache: ${CCACHE_EXECUTABLE}")
endif()

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
option(DFTRACER_UTILS_ENABLE_PCH "Enable precompiled headers" ON)
option(DFTRACER_UTILS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(DFTRACER_UTILS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(DFTRACER_UTILS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(DFTRACER_UTILS_ENABLE_MPI "Enable MPI support for call tree" OFF)
option(DFTRACER_USE_ZLIB_NG "Use zlib-ng (compat) instead of madler/zlib; falls back to madler on failure" ON)
option(DFTRACER_UTILS_ENABLE_ARROW "Enable Arrow C Data Interface via nanoarrow" ON)
option(DFTRACER_UTILS_ENABLE_ARROW_IPC "Enable Arrow IPC file read/write via nanoarrow" ON)
option(DFTRACER_UTILS_ENABLE_ZSTD "Enable ZSTD compression for RocksDB" ON)
option(DFTRACER_UTILS_ENABLE_LZ4 "Enable LZ4 compression for RocksDB" OFF)
option(
  DFTRACER_UTILS_LOCAL_PACKAGES
  "Prefer system-installed bundled dependencies (RocksDB, zstd, simdjson, ...) when available; set OFF to build them from source for portable wheels"
  ON)

if(DFTRACER_UTILS_TESTS)
  dftracer_utils_ok("Building tests")
  set(DFTRACER_UTILS_TESTS
      ON
      CACHE BOOL "" FORCE)
  if(NOT DFTRACER_UTILS_ENABLE_TSAN)
    set(DFTRACER_UTILS_COVERAGE
        ON
        CACHE BOOL "" FORCE)
  else()
    set(DFTRACER_UTILS_COVERAGE
        OFF
        CACHE BOOL "" FORCE)
  endif()
  # set(DFTRACER_UTILS_DEBUG
  #     ON
  #     CACHE BOOL "" FORCE)
  set(DFTRACER_UTILS_BUILD_SHARED
      ON
      CACHE BOOL "" FORCE)
  set(DFTRACER_UTILS_BUILD_STATIC
      ON
      CACHE BOOL "" FORCE)
  set(DFTRACER_UTILS_BUILD_BINARIES
      ON
      CACHE BOOL "" FORCE)
  # set(DFTRACER_UTILS_ENABLE_ASAN ON CACHE BOOL "" FORCE)
  # set(DFTRACER_UTILS_ENABLE_UBSAN ON CACHE BOOL "" FORCE)
  # set(DFTRACER_UTILS_ENABLE_TSAN ON CACHE BOOL "" FORCE)
endif()

if(DFTRACER_UTILS_ENABLE_MPI)
  find_package(MPI REQUIRED)
endif()

# Validation: Ensure at least one library type is enabled
if(NOT DFTRACER_UTILS_BUILD_SHARED AND NOT DFTRACER_UTILS_BUILD_STATIC)
  message(
    FATAL_ERROR
      "At least one of DFTRACER_UTILS_BUILD_SHARED or DFTRACER_UTILS_BUILD_STATIC must be enabled"
  )
endif()

# io_uring: detect kernel header (no external library needed)
include(CheckIncludeFile)
include(CheckSymbolExists)
check_include_file("linux/io_uring.h" HAVE_LINUX_IO_URING_H)
if(HAVE_LINUX_IO_URING_H)
  check_symbol_exists(__NR_io_uring_setup "sys/syscall.h" HAVE_IO_URING_SYSCALL)
endif()
if(HAVE_LINUX_IO_URING_H AND HAVE_IO_URING_SYSCALL)
  set(DFTRACER_UTILS_HAVE_IO_URING ON)
  dftracer_utils_ok("io_uring support: enabled (kernel header found)")
else()
  set(DFTRACER_UTILS_HAVE_IO_URING OFF)
  dftracer_utils_warn(
    "io_uring support: disabled (kernel header or syscall not found)")
endif()

# kqueue: detect availability (macOS, FreeBSD, etc.)
check_include_file("sys/event.h" HAVE_SYS_EVENT_H)
if(HAVE_SYS_EVENT_H)
  set(DFTRACER_UTILS_HAVE_KQUEUE ON)
  dftracer_utils_ok("kqueue support: enabled (sys/event.h found)")
else()
  set(DFTRACER_UTILS_HAVE_KQUEUE OFF)
  dftracer_utils_warn("kqueue support: disabled (sys/event.h not found)")
endif()

# lustreapi: optional for stripe_count / stripe_size queries. When absent, the
# parallel writer treats Lustre like an opaque POSIX filesystem.
check_include_file("lustre/lustreapi.h" HAVE_LUSTRE_LUSTREAPI_H)
find_library(LUSTREAPI_LIBRARY NAMES lustreapi)
if(HAVE_LUSTRE_LUSTREAPI_H AND LUSTREAPI_LIBRARY)
  set(DFTRACER_UTILS_HAVE_LUSTREAPI ON)
  dftracer_utils_ok("lustreapi support: enabled (${LUSTREAPI_LIBRARY})")
else()
  set(DFTRACER_UTILS_HAVE_LUSTREAPI OFF)
  dftracer_utils_warn(
    "lustreapi support: disabled (header or library not found)")
endif()

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

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
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/Sanitizer.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/PrecompiledHeader.cmake)

# Enable the lock-free 16-byte CAS (DWCAS) fast path for ObjectPool when the
# target supports it; otherwise the header transparently uses a packed fallback.
check_dwcas()

configure_sanitizers()
print_sanitizer_status()

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

# Configuration summary
dftracer_utils_section("dftracer-utils ${DFTRACER_UTILS_VERSION}")
dftracer_utils_item("Build type     " "${CMAKE_BUILD_TYPE}")
dftracer_utils_item("Compiler       "
                    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
dftracer_utils_item("Target arch    " "${CMAKE_SYSTEM_PROCESSOR}")
dftracer_utils_item("Libraries      "
                    "shared=${DFTRACER_UTILS_BUILD_SHARED} static=${DFTRACER_UTILS_BUILD_STATIC}"
)
dftracer_utils_item("Binaries / PyExt"
                    "${DFTRACER_UTILS_BUILD_BINARIES} / ${DFTRACER_UTILS_BUILD_PYTHON}"
)
dftracer_utils_item("Tests          " "${DFTRACER_UTILS_TESTS}")
dftracer_utils_item("MPI            " "${DFTRACER_UTILS_ENABLE_MPI}")
dftracer_utils_item("Compression    "
                    "zstd=${DFTRACER_UTILS_ENABLE_ZSTD} lz4=${DFTRACER_UTILS_ENABLE_LZ4}"
)
dftracer_utils_item("Arrow          " "${DFTRACER_UTILS_ENABLE_ARROW}")
dftracer_utils_item("Async I/O      "
                    "io_uring=${DFTRACER_UTILS_HAVE_IO_URING} kqueue=${DFTRACER_UTILS_HAVE_KQUEUE}"
)
dftracer_utils_item("Lustre API     " "${DFTRACER_UTILS_HAVE_LUSTREAPI}")
if(DFTRACER_UTILS_HAS_DWCAS)
  set(_dftracer_atomics_path "DWCAS (lock-free 16-byte CAS)")
else()
  set(_dftracer_atomics_path "packed (lock-free 64-bit CAS, portable)")
endif()
dftracer_utils_item("Atomics path   " "${_dftracer_atomics_path}")

add_subdirectory(src)

if(DFTRACER_UTILS_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
