# SPDX-License-Identifier: MIT
# ==============================================================================
# CPM Configuration
# ==============================================================================

set(CPM_USE_LOCAL_PACKAGES ${DFTRACER_UTILS_LOCAL_PACKAGES})
set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cpmsource")

# ==============================================================================
# System Dependencies
# ==============================================================================

find_package(Threads REQUIRED)

# Find MPI if enabled
option(DFTRACER_UTILS_ENABLE_MPI "Enable MPI support" OFF)
if(DFTRACER_UTILS_ENABLE_MPI)
  find_package(MPI REQUIRED)
  dftracer_utils_section("MPI support enabled")
  dftracer_utils_item("MPI_CXX_COMPILER" "${MPI_CXX_COMPILER}")
  dftracer_utils_item("MPI_CXX_LIBRARIES" "${MPI_CXX_LIBRARIES}")
endif()

set(DEPENDENCY_LIBRARY_DIRS "")

if(CMAKE_VERSION VERSION_LESS 3.18)
  set(DEV_MODULE Development)
else()
  set(DEV_MODULE Development.Module)
endif()

find_package(
  Python 3.8
  COMPONENTS Interpreter ${DEV_MODULE}
  OPTIONAL_COMPONENTS Development.SABIModule)

# ==============================================================================
# Utility Dependencies
# ==============================================================================

function(need_argparse)
  if(NOT argparse_ADDED)
    cpmaddpackage(
      NAME
      argparse
      GITHUB_REPOSITORY
      p-ranav/argparse
      VERSION
      3.2
      OPTIONS
      "ARGPARSE_BUILD_TESTS OFF"
      "ARGPARSE_BUILD_SAMPLES OFF"
      FORCE
      YES)
  endif()
endfunction()

function(need_nonstd_span)
  if(NOT nonstd_span_ADDED)
    cpmaddpackage(NAME nonstd_span GITHUB_REPOSITORY nonstd-lite/span-lite
                  VERSION 0.11.0)
  endif()
endfunction()

function(need_unordered_dense)
  if(NOT unordered_dense_ADDED)
    cpmaddpackage(
      NAME
      unordered_dense
      GITHUB_REPOSITORY
      martinus/unordered_dense
      VERSION
      4.4.0
      OPTIONS
      "UNORDERED_DENSE_INSTALL ON"
      FORCE
      YES)
  endif()

  # Public dependency: our installed headers include <ankerl/unordered_dense.h>,
  # so a consumer (or a plugin that reaches the parser path) building against
  # the install prefix needs it there. The upstream UNORDERED_DENSE_INSTALL
  # export does not land in our prefix, so install the single header explicitly,
  # as simdjson and concurrentqueue are, matching the $<INSTALL_INTERFACE> dir.
  if(DEFINED unordered_dense_SOURCE_DIR)
    install(FILES ${unordered_dense_SOURCE_DIR}/include/ankerl/unordered_dense.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ankerl)
  endif()
endfunction()

function(link_unordered_dense TARGET_NAME)
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_unordered_dense: TARGET_NAME is required")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(
      FATAL_ERROR "link_unordered_dense: Target '${TARGET_NAME}' does not exist"
    )
  endif()

  if(NOT TARGET unordered_dense::unordered_dense)
    message(
      FATAL_ERROR
        "link_unordered_dense: ankerl::unordered_dense not found! Call need_unordered_dense() first."
    )
  endif()

  get_target_property(UD_INC unordered_dense::unordered_dense
                      INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(
    ${TARGET_NAME} PUBLIC "$<BUILD_INTERFACE:${UD_INC}>"
                          "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endfunction()

function(need_tl_expected)
  # tl::expected is only needed when C++23 std::expected is unavailable
  if(CMAKE_CXX_STANDARD GREATER_EQUAL 23)
    dftracer_utils_warn(
      "C++23 detected: using std::expected (skipping tl::expected)")
    return()
  endif()

  if(NOT tl_expected_ADDED)
    cpmaddpackage(
      NAME
      tl_expected
      GITHUB_REPOSITORY
      TartanLlama/expected
      VERSION
      1.1.0
      GIT_TAG
      "v1.1.0"
      DOWNLOAD_ONLY
      YES)
  endif()

  if(tl_expected_ADDED)
    if(NOT TARGET tl::expected)
      add_library(tl_expected INTERFACE)
      target_include_directories(
        tl_expected
        INTERFACE $<BUILD_INTERFACE:${tl_expected_SOURCE_DIR}/include>
                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      add_library(tl::expected ALIAS tl_expected)

      install(
        DIRECTORY ${tl_expected_SOURCE_DIR}/include/tl/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tl
        FILES_MATCHING
        PATTERN "*.hpp")

      install(TARGETS tl_expected EXPORT tl_expectedTargets)
      install(
        EXPORT tl_expectedTargets
        FILE tl_expectedTargets.cmake
        NAMESPACE tl::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tl_expected)
    endif()

    dftracer_utils_ok("Added tl::expected header-only library via CPM")
  endif()
endfunction()

function(link_tl_expected TARGET_NAME)
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_tl_expected: TARGET_NAME is required")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(
      FATAL_ERROR "link_tl_expected: Target '${TARGET_NAME}' does not exist")
  endif()

  # C++23: std::expected is built-in, no linking needed
  if(CMAKE_CXX_STANDARD GREATER_EQUAL 23)
    return()
  endif()

  if(TARGET tl::expected)
    target_link_libraries(${TARGET_NAME} PUBLIC tl::expected)
    dftracer_utils_ok("Linked ${TARGET_NAME} to tl::expected")
  elseif(TARGET tl_expected)
    target_link_libraries(${TARGET_NAME} PUBLIC tl_expected)
    dftracer_utils_ok("Linked ${TARGET_NAME} to tl_expected")
  else()
    message(
      FATAL_ERROR
        "link_tl_expected: No tl::expected found! Call need_tl_expected() first."
    )
  endif()
endfunction()

# ==============================================================================
# simdjson - SIMD-accelerated JSON parser (On-Demand API for zero-copy)
# ==============================================================================

function(need_simdjson)
  if(NOT simdjson_ADDED)
    cpmaddpackage(
      NAME
      simdjson
      GITHUB_REPOSITORY
      simdjson/simdjson
      VERSION
      4.6.4
      GIT_TAG
      v4.6.4
      DOWNLOAD_ONLY
      YES)
  endif()

  if(simdjson_ADDED AND NOT TARGET simdjson)
    dftracer_utils_ok("Building simdjson library (v4.6.4)")

    # simdjson is a single-header + single-source library
    set(SIMDJSON_SOURCES ${simdjson_SOURCE_DIR}/singleheader/simdjson.h
                         ${simdjson_SOURCE_DIR}/singleheader/simdjson.cpp)

    set(SIMDJSON_TARGETS)

    if(DFTRACER_UTILS_BUILD_STATIC)
      add_library(simdjson_static STATIC ${SIMDJSON_SOURCES})
      target_include_directories(
        simdjson_static SYSTEM
        PUBLIC $<BUILD_INTERFACE:${simdjson_SOURCE_DIR}/singleheader>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      target_compile_features(simdjson_static PUBLIC cxx_std_17)
      # Suppress warnings from simdjson (third-party code)
      target_compile_options(simdjson_static PRIVATE -w)
      set_target_properties(
        simdjson_static
        PROPERTIES OUTPUT_NAME simdjson
                   ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                   POSITION_INDEPENDENT_CODE ON)
      add_library(simdjson::simdjson_static ALIAS simdjson_static)
      list(APPEND SIMDJSON_TARGETS simdjson_static)
      dftracer_utils_ok("Added simdjson static library")
    endif()

    if(DFTRACER_UTILS_BUILD_SHARED)
      add_library(simdjson_shared SHARED ${SIMDJSON_SOURCES})
      target_include_directories(
        simdjson_shared SYSTEM
        PUBLIC $<BUILD_INTERFACE:${simdjson_SOURCE_DIR}/singleheader>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      target_compile_features(simdjson_shared PUBLIC cxx_std_17)
      # Suppress warnings from simdjson (third-party code)
      target_compile_options(simdjson_shared PRIVATE -w)
      set_target_properties(
        simdjson_shared
        PROPERTIES OUTPUT_NAME simdjson
                   LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                   ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
      add_library(simdjson::simdjson ALIAS simdjson_shared)
      list(APPEND SIMDJSON_TARGETS simdjson_shared)
      dftracer_utils_ok("Added simdjson shared library")
    elseif(DFTRACER_UTILS_BUILD_STATIC)
      add_library(simdjson::simdjson ALIAS simdjson_static)
    endif()

    # Install header
    install(FILES ${simdjson_SOURCE_DIR}/singleheader/simdjson.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    if(SIMDJSON_TARGETS)
      install(
        TARGETS ${SIMDJSON_TARGETS}
        EXPORT simdjsonTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

      install(
        EXPORT simdjsonTargets
        FILE simdjsonTargets.cmake
        NAMESPACE simdjson::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/simdjson)
    endif()
  endif()
endfunction()

function(link_simdjson TARGET_NAME LIBRARY_TYPE)
  # Validate parameters
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_simdjson: TARGET_NAME is required")
  endif()

  if(NOT LIBRARY_TYPE MATCHES "^(STATIC|SHARED)$")
    message(
      FATAL_ERROR "link_simdjson: LIBRARY_TYPE must be either STATIC or SHARED")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(FATAL_ERROR "link_simdjson: Target '${TARGET_NAME}' does not exist")
  endif()

  # Link appropriate simdjson variant
  if(LIBRARY_TYPE STREQUAL "STATIC")
    # For static libraries, prefer static simdjson if available
    if(TARGET simdjson_static)
      target_link_libraries(${TARGET_NAME} PUBLIC simdjson::simdjson_static)
      dftracer_utils_ok("Linked ${TARGET_NAME} to simdjson_static")
    elseif(TARGET simdjson_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC simdjson::simdjson)
      dftracer_utils_ok("Linked ${TARGET_NAME} to simdjson (shared)")
    elseif(TARGET simdjson::simdjson)
      # System / find_package() simdjson (e.g. Homebrew on macOS).
      target_link_libraries(${TARGET_NAME} PUBLIC simdjson::simdjson)
      dftracer_utils_ok("Linked ${TARGET_NAME} to system simdjson::simdjson")
    else()
      message(
        FATAL_ERROR
          "link_simdjson: No simdjson found! Call need_simdjson() first.")
    endif()
  else() # SHARED
    # For shared libraries, prefer shared simdjson if available
    if(TARGET simdjson_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC simdjson::simdjson)
      dftracer_utils_ok("Linked ${TARGET_NAME} to simdjson (shared)")
    elseif(TARGET simdjson_static)
      target_link_libraries(${TARGET_NAME} PUBLIC simdjson::simdjson_static)
      dftracer_utils_ok("Linked ${TARGET_NAME} to simdjson_static")
    elseif(TARGET simdjson::simdjson)
      # System / find_package() simdjson (e.g. Homebrew on macOS).
      target_link_libraries(${TARGET_NAME} PUBLIC simdjson::simdjson)
      dftracer_utils_ok("Linked ${TARGET_NAME} to system simdjson::simdjson")
    else()
      message(
        FATAL_ERROR
          "link_simdjson: No simdjson found! Call need_simdjson() first.")
    endif()
  endif()
endfunction()

# ==============================================================================
# Compression Dependencies
# ==============================================================================

# zlib-ng in its native API (zng_ names) as one static PIC archive with hidden
# visibility. The utilities library links it privately, so no library of ours
# exports a zlib symbol and a process that also loads the system zlib binds no
# call of its own to ours. No zlib header is installed: no public header
# includes one.
function(need_zlib)
  if(TARGET dftracer::zlib)
    return()
  endif()
  cpmaddpackage(
    NAME
    zlib-ng
    GITHUB_REPOSITORY
    zlib-ng/zlib-ng
    VERSION
    2.3.3
    GIT_TAG
    2.3.3
    OPTIONS
    "ZLIB_COMPAT OFF"
    "ZLIB_ALIASES OFF"
    "BUILD_TESTING OFF"
    "ZLIBNG_ENABLE_TESTS OFF"
    "WITH_GTEST OFF"
    "WITH_OPTIM ON"
    "WITH_NEW_STRATEGIES ON"
    "WITH_NATIVE_INSTRUCTIONS OFF"
    "INSTALL_UTILS OFF"
    "SKIP_INSTALL_ALL ON")
  if(NOT TARGET zlib-ng-static)
    message(FATAL_ERROR "need_zlib: zlib-ng made no zlib-ng-static target")
  endif()
  get_target_property(_zng zlib-ng-static ALIASED_TARGET)
  if(NOT _zng)
    set(_zng zlib-ng-static)
  endif()
  get_target_property(_zng_type ${_zng} TYPE)
  if(NOT _zng_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "need_zlib: zlib-ng-static is a ${_zng_type}")
  endif()
  if(TARGET zlib-ng AND NOT _zng STREQUAL "zlib-ng")
    get_target_property(_zng_shared zlib-ng ALIASED_TARGET)
    if(NOT _zng_shared)
      set(_zng_shared zlib-ng)
    endif()
    set_target_properties(${_zng_shared} PROPERTIES EXCLUDE_FROM_ALL ON)
  endif()
  set_target_properties(
    ${_zng}
    PROPERTIES OUTPUT_NAME dftracer_zlib
               ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
               POSITION_INDEPENDENT_CODE ON
               C_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON)
  add_library(dftracer::zlib ALIAS ${_zng})
  # A static library of ours that links the archive needs it at install.
  if(DFTRACER_UTILS_BUILD_STATIC)
    install(
      TARGETS ${_zng}
      EXPORT ZlibTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(
      EXPORT ZlibTargets
      FILE ZlibTargets.cmake
      NAMESPACE dftracer::
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlib)
  endif()
  dftracer_utils_ok("Using zlib-ng ${CPM_PACKAGE_zlib-ng_VERSION} (native, static, hidden)")
endfunction()

# Links the zlib-ng archive into TARGET_NAME privately: its headers reach only
# TARGET_NAME's own sources, and a shared library keeps its symbols hidden.
function(link_zlib TARGET_NAME)
  if(NOT TARGET ${TARGET_NAME})
    message(FATAL_ERROR "link_zlib: Target '${TARGET_NAME}' does not exist")
  endif()
  if(NOT TARGET dftracer::zlib)
    message(FATAL_ERROR "link_zlib: call need_zlib() first")
  endif()
  target_link_libraries(${TARGET_NAME} PRIVATE dftracer::zlib)
endfunction()

# zstd 1.5.7 from source for every build, never the system copy: one static
# PIC archive with hidden visibility. A system zstd can be years older and
# slower, and with hidden symbols a process that also loads the system libzstd
# (Python) binds no call of its own to ours. The legacy decoders are off: nothing we read predates zstd 1.0.
function(need_zstd)
  if(NOT zstd_ADDED)
    cpmaddpackage(
      NAME
      zstd
      GITHUB_REPOSITORY
      facebook/zstd
      VERSION
      1.5.7
      GIT_TAG
      v1.5.7
      SOURCE_SUBDIR
      build/cmake
      OPTIONS
      "CMAKE_OSX_DEPLOYMENT_TARGET ${CMAKE_OSX_DEPLOYMENT_TARGET}"
      "ZSTD_BUILD_PROGRAMS OFF"
      "ZSTD_BUILD_TESTS OFF"
      "ZSTD_BUILD_SHARED OFF"
      "ZSTD_BUILD_STATIC ON"
      "ZSTD_LEGACY_SUPPORT OFF"
      FORCE
      YES)
  endif()
  if(NOT TARGET libzstd_static)
    message(FATAL_ERROR "need_zstd: the zstd build made no libzstd_static")
  endif()

  set_target_properties(
    libzstd_static
    PROPERTIES POSITION_INDEPENDENT_CODE ON
               C_VISIBILITY_PRESET hidden
               ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
  # zstd.h marks its API visible unless these are defined; empty, the hidden
  # preset holds for every symbol of the archive.
  target_compile_definitions(
    libzstd_static PRIVATE "ZSTDLIB_VISIBLE=" "ZSTDERRORLIB_VISIBLE="
                           "ZDICTLIB_VISIBLE=")
  # zstd's own sources trip GCC -Wmaybe-uninitialized, and POOL_thread ends in
  # an assert(0) that GCC does not always treat as noreturn (-Wreturn-type).
  # Third-party source, so silence both on zstd's target only.
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(libzstd_static PRIVATE -Wno-maybe-uninitialized
                                                  -Wno-return-type)
  endif()
  set_property(
    TARGET libzstd_static
    APPEND
    PROPERTY INTERFACE_INCLUDE_DIRECTORIES
             "$<BUILD_INTERFACE:${zstd_SOURCE_DIR}/lib>")
  # A program that links our static libraries also links this archive.
  install(TARGETS libzstd_static ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES "${zstd_SOURCE_DIR}/lib/zstd.h"
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  # zstd's build tree names only libzstd_static; nanoarrow links
  # zstd::libzstd_static.
  if(NOT TARGET zstd::libzstd_static)
    add_library(zstd::libzstd_static ALIAS libzstd_static)
  endif()
  dftracer_utils_ok("Built zstd 1.5.7 with CPM (static, hidden)")
endfunction()

# ==============================================================================
# libdeflate - one-shot (whole-buffer) gzip/deflate codec used for
# member-at-a-time decompression on the read path. No decompress-speed work has
# landed upstream since ~v1.23, so we pin the tested release rather than master.
# ==============================================================================
function(need_libdeflate)
  if(DFTRACER_UTILS_LOCAL_PACKAGES)
    find_package(libdeflate QUIET CONFIG)
    if(libdeflate_FOUND)
      dftracer_utils_ok("Found system libdeflate")
      return()
    endif()
  endif()

  if(NOT libdeflate_ADDED)
    cpmaddpackage(
      NAME
      libdeflate
      GITHUB_REPOSITORY
      ebiggers/libdeflate
      VERSION
      1.25
      GIT_TAG
      v1.25
      OPTIONS
      "LIBDEFLATE_BUILD_SHARED_LIB OFF"
      "LIBDEFLATE_BUILD_STATIC_LIB ON"
      "LIBDEFLATE_BUILD_GZIP OFF")
  endif()

  if(libdeflate_ADDED)
    # The static lib links into our shared libraries, so it must be PIC (GNU ld
    # rejects non-PIC objects in a shared object; macOS ld tolerates it).
    if(TARGET libdeflate_static)
      set_target_properties(libdeflate_static
                            PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    dftracer_utils_ok("Built libdeflate with CPM")
  endif()
endfunction()

# Link the zstd archive need_zstd built into `target`.
function(link_zstd target)
  if(NOT TARGET zstd::libzstd_static)
    message(FATAL_ERROR "link_zstd: call need_zstd() first")
  endif()
  target_link_libraries(${target} PRIVATE zstd::libzstd_static)
endfunction()

# Link the resolved libdeflate target (name varies: CPM static build vs a system
# find_package) into `target`.
function(link_libdeflate target)
  if(TARGET libdeflate::libdeflate_static)
    target_link_libraries(${target} PRIVATE libdeflate::libdeflate_static)
  elseif(TARGET libdeflate::libdeflate_shared)
    target_link_libraries(${target} PRIVATE libdeflate::libdeflate_shared)
  elseif(TARGET libdeflate_static)
    target_link_libraries(${target} PRIVATE libdeflate_static)
  elseif(TARGET libdeflate_shared)
    target_link_libraries(${target} PRIVATE libdeflate_shared)
  else()
    message(FATAL_ERROR "link_libdeflate: no libdeflate target available")
  endif()
endfunction()

# ==============================================================================
# Concurrency Dependencies
# ==============================================================================

function(need_readerwriterqueue)
  if(NOT readerwriterqueue_ADDED)
    cpmaddpackage(
      NAME
      readerwriterqueue
      GITHUB_REPOSITORY
      cameron314/readerwriterqueue
      GIT_TAG
      211616e0554f93152ab3108b8d93fbc23174a9d9
      DOWNLOAD_ONLY
      YES)

    if(readerwriterqueue_ADDED)
      add_library(readerwriterqueue INTERFACE)
      target_include_directories(
        readerwriterqueue
        INTERFACE $<BUILD_INTERFACE:${readerwriterqueue_SOURCE_DIR}>
                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      install(FILES ${readerwriterqueue_SOURCE_DIR}/readerwriterqueue.h
                    ${readerwriterqueue_SOURCE_DIR}/atomicops.h
              DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

      # Install and export the target
      install(TARGETS readerwriterqueue EXPORT readerwriterqueueTargets)
      install(
        EXPORT readerwriterqueueTargets
        FILE readerwriterqueueTargets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/readerwriterqueue)

      dftracer_utils_ok("Added readerwriterqueue header-only library")
    endif()
  endif()
endfunction()

function(need_concurrentqueue)
  if(NOT concurrentqueue_ADDED)
    cpmaddpackage(
      NAME
      concurrentqueue
      GITHUB_REPOSITORY
      cameron314/concurrentqueue
      GIT_TAG
      36c5755dc0789e5bcddbbb9a201d3af2bbf3024c
      PATCHES
      ${PROJECT_SOURCE_DIR}/cmake/patches/concurrentqueue-thread-exit.patch
      DOWNLOAD_ONLY
      YES)

    if(concurrentqueue_ADDED)
      add_library(concurrentqueue INTERFACE)
      target_include_directories(
        concurrentqueue
        INTERFACE $<BUILD_INTERFACE:${concurrentqueue_SOURCE_DIR}>
                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      install(
        FILES ${concurrentqueue_SOURCE_DIR}/concurrentqueue.h
              ${concurrentqueue_SOURCE_DIR}/blockingconcurrentqueue.h
              ${concurrentqueue_SOURCE_DIR}/lightweightsemaphore.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

      # Install and export the target
      install(TARGETS concurrentqueue EXPORT concurrentqueueTargets)
      install(
        EXPORT concurrentqueueTargets
        FILE concurrentqueueTargets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/concurrentqueue)

      dftracer_utils_ok("Added concurrentqueue header-only library")
    endif()
  endif()
endfunction()

# ==============================================================================
# Arrow Data Interface Dependencies (nanoarrow)
# ==============================================================================

function(need_nanoarrow)
  if(NOT nanoarrow_ADDED)
    cpmaddpackage(
      NAME
      nanoarrow
      GITHUB_REPOSITORY
      apache/arrow-nanoarrow
      VERSION
      0.8.0
      GIT_TAG
      "apache-arrow-nanoarrow-0.8.0"
      DOWNLOAD_ONLY
      YES)
  endif()

  if(nanoarrow_ADDED)
    set(NANOARROW_SOVERSION 0)
    set(NANOARROW_SOURCES
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/common/array.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/common/array_stream.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/common/schema.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/common/utils.c)
    set(NANOARROW_TARGETS)
    set(NANOARROW_IPC_INCLUDE_DIRS)

    # Arrow IPC support (reader/writer for .arrows files)
    if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
      list(
        APPEND
        NANOARROW_SOURCES
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/ipc/decoder.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/ipc/encoder.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/ipc/reader.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/ipc/writer.c
        ${nanoarrow_SOURCE_DIR}/src/nanoarrow/ipc/codecs.c
        ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/src/runtime/builder.c
        ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/src/runtime/emitter.c
        ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/src/runtime/refmap.c
        ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/src/runtime/verifier.c)
      set(NANOARROW_FLATCC_INCLUDE
          ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/include)
      dftracer_utils_ok("nanoarrow IPC support enabled (reader + writer)")
    endif()

    # Generate nanoarrow_config.h from template
    set(NANOARROW_VERSION_MAJOR 0)
    set(NANOARROW_VERSION_MINOR 8)
    set(NANOARROW_VERSION_PATCH 0)
    set(NANOARROW_VERSION "0.8.0")
    set(NANOARROW_NAMESPACE_DEFINE "")
    configure_file(${nanoarrow_SOURCE_DIR}/src/nanoarrow/nanoarrow_config.h.in
                   ${CMAKE_CURRENT_BINARY_DIR}/nanoarrow/nanoarrow_config.h)

    if(DFTRACER_UTILS_BUILD_STATIC)
      add_library(nanoarrow_static STATIC ${NANOARROW_SOURCES})
      target_include_directories(
        nanoarrow_static
        PUBLIC $<BUILD_INTERFACE:${nanoarrow_SOURCE_DIR}/src>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
        target_include_directories(
          nanoarrow_static
          PUBLIC $<BUILD_INTERFACE:${NANOARROW_FLATCC_INCLUDE}>
                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        # Enable zstd compression for Arrow IPC
        target_compile_definitions(nanoarrow_static
                                   PRIVATE NANOARROW_IPC_WITH_ZSTD)
        target_link_libraries(nanoarrow_static PRIVATE zstd::libzstd_static)
      endif()
      set_target_properties(
        nanoarrow_static
        PROPERTIES VERSION ${PROJECT_VERSION}
                   SOVERSION ${NANOARROW_SOVERSION}
                   OUTPUT_NAME nanoarrow
                   ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
      add_library(nanoarrow::nanoarrow_static ALIAS nanoarrow_static)
      list(APPEND NANOARROW_TARGETS nanoarrow_static)
      dftracer_utils_ok("Added nanoarrow static library")
    endif()

    if(DFTRACER_UTILS_BUILD_SHARED)
      add_library(nanoarrow_shared SHARED ${NANOARROW_SOURCES})
      target_include_directories(
        nanoarrow_shared
        PUBLIC $<BUILD_INTERFACE:${nanoarrow_SOURCE_DIR}/src>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
        target_include_directories(
          nanoarrow_shared
          PUBLIC $<BUILD_INTERFACE:${NANOARROW_FLATCC_INCLUDE}>
                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        # Enable zstd compression for Arrow IPC
        target_compile_definitions(nanoarrow_shared
                                   PRIVATE NANOARROW_IPC_WITH_ZSTD)
        target_link_libraries(nanoarrow_shared PRIVATE zstd::libzstd_static)
      endif()
      set_target_properties(
        nanoarrow_shared
        PROPERTIES VERSION ${PROJECT_VERSION}
                   SOVERSION ${NANOARROW_SOVERSION}
                   OUTPUT_NAME nanoarrow
                   LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                   ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
      add_library(nanoarrow::nanoarrow ALIAS nanoarrow_shared)
      list(APPEND NANOARROW_TARGETS nanoarrow_shared)
      dftracer_utils_ok("Added nanoarrow shared library")
    elseif(DFTRACER_UTILS_BUILD_STATIC)
      add_library(nanoarrow::nanoarrow ALIAS nanoarrow_static)
    endif()

    # Install headers
    install(
      DIRECTORY ${nanoarrow_SOURCE_DIR}/src/nanoarrow/
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nanoarrow
      FILES_MATCHING
      PATTERN "*.h"
      PATTERN "*.hpp"
      PATTERN "testing" EXCLUDE
      PATTERN "integration" EXCLUDE
      PATTERN "device" EXCLUDE)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nanoarrow/nanoarrow_config.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nanoarrow)
    if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
      install(
        DIRECTORY ${nanoarrow_SOURCE_DIR}/thirdparty/flatcc/include/flatcc/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/flatcc
        FILES_MATCHING
        PATTERN "*.h")
    endif()

    # Suppress warnings from nanoarrow headers (redundant redeclarations, shadow
    # warnings in nanoarrow 0.8.0 internal headers)
    foreach(_na_target ${NANOARROW_TARGETS})
      get_target_property(_na_inc ${_na_target} INTERFACE_INCLUDE_DIRECTORIES)
      if(_na_inc)
        set_target_properties(
          ${_na_target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                                   "${_na_inc}")
      endif()
    endforeach()

    if(NANOARROW_TARGETS)
      install(
        TARGETS ${NANOARROW_TARGETS}
        EXPORT nanoarrowTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

      install(
        EXPORT nanoarrowTargets
        FILE nanoarrowTargets.cmake
        NAMESPACE nanoarrow::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nanoarrow)
    endif()

    dftracer_utils_ok("Added nanoarrow 0.8.0 via CPM")
  endif()
endfunction()

function(link_nanoarrow TARGET_NAME LIBRARY_TYPE)
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_nanoarrow: TARGET_NAME is required")
  endif()

  if(NOT LIBRARY_TYPE MATCHES "^(STATIC|SHARED)$")
    message(
      FATAL_ERROR "link_nanoarrow: LIBRARY_TYPE must be either STATIC or SHARED"
    )
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(
      FATAL_ERROR "link_nanoarrow: Target '${TARGET_NAME}' does not exist")
  endif()

  if(LIBRARY_TYPE STREQUAL "STATIC")
    if(TARGET nanoarrow_static)
      target_link_libraries(${TARGET_NAME} PUBLIC nanoarrow::nanoarrow_static)
      dftracer_utils_ok("Linked ${TARGET_NAME} to nanoarrow_static")
    elseif(TARGET nanoarrow_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC nanoarrow::nanoarrow)
      dftracer_utils_ok("Linked ${TARGET_NAME} to nanoarrow (shared)")
    else()
      message(
        FATAL_ERROR
          "link_nanoarrow: No nanoarrow found! Call need_nanoarrow() first.")
    endif()
  else()
    if(TARGET nanoarrow_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC nanoarrow::nanoarrow)
      dftracer_utils_ok("Linked ${TARGET_NAME} to nanoarrow (shared)")
    elseif(TARGET nanoarrow_static)
      target_link_libraries(${TARGET_NAME} PUBLIC nanoarrow::nanoarrow_static)
      dftracer_utils_ok("Linked ${TARGET_NAME} to nanoarrow_static")
    else()
      message(
        FATAL_ERROR
          "link_nanoarrow: No nanoarrow found! Call need_nanoarrow() first.")
    endif()
  endif()

endfunction()

# ==============================================================================
# Highway (SIMD kernels for the vec engine); runtime-dispatched
# ==============================================================================

function(need_highway)
  if(NOT highway_ADDED AND NOT TARGET hwy)
    cpmaddpackage(
      NAME
      highway
      GITHUB_REPOSITORY
      google/highway
      VERSION
      1.4.0
      GIT_TAG
      1.4.0
      OPTIONS
      "HWY_ENABLE_TESTS OFF"
      "HWY_ENABLE_EXAMPLES OFF"
      "HWY_ENABLE_CONTRIB ON"
      "HWY_ENABLE_INSTALL OFF")

    if(highway_ADDED)
      # Highway headers are third-party; treat as system to keep our strict
      # warnings from flagging them.
      if(TARGET hwy)
        get_target_property(_hwy_inc hwy INTERFACE_INCLUDE_DIRECTORIES)
        if(_hwy_inc)
          set_target_properties(
            hwy PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_hwy_inc}")
        endif()
      endif()
      dftracer_utils_ok("Added highway ${highway_VERSION} SIMD library via CPM")
    endif()
  endif()
endfunction()

# ==============================================================================
# PCRE2 (8-bit, JIT); the regex engine behind duql patterns
# ==============================================================================

function(need_pcre2)
  if(NOT pcre2_ADDED AND NOT TARGET pcre2-8-static)
    cpmaddpackage(
      NAME
      pcre2
      GITHUB_REPOSITORY
      PCRE2Project/pcre2
      VERSION
      10.48
      GIT_TAG
      pcre2-10.48
      OPTIONS
      "BUILD_SHARED_LIBS OFF"
      "BUILD_STATIC_LIBS ON"
      "PCRE2_STATIC_PIC ON"
      "PCRE2_BUILD_PCRE2_8 ON"
      "PCRE2_BUILD_PCRE2_16 OFF"
      "PCRE2_BUILD_PCRE2_32 OFF"
      "PCRE2_SUPPORT_JIT ON"
      "PCRE2_SUPPORT_UNICODE ON"
      "PCRE2_BUILD_PCRE2GREP OFF"
      "PCRE2_BUILD_TESTS OFF"
      "PCRE2_SUPPORT_LIBBZ2 OFF"
      "PCRE2_SUPPORT_LIBZ OFF"
      "PCRE2_SUPPORT_LIBEDIT OFF"
      "PCRE2_SUPPORT_LIBREADLINE OFF")
    if(pcre2_ADDED)
      dftracer_utils_ok("Added PCRE2 ${pcre2_VERSION} via CPM")
    endif()
  endif()
endfunction()

# ==============================================================================
# utf8proc (MIT); the Unicode tables behind Series.str.normalize
# ==============================================================================

function(need_utf8proc)
  if(NOT TARGET utf8proc_static)
    if(NOT utf8proc_ADDED)
      # The pinned release always builds from source, never a system copy.
      set(CPM_USE_LOCAL_PACKAGES OFF)
      cpmaddpackage(
        NAME
        utf8proc
        GITHUB_REPOSITORY
        JuliaStrings/utf8proc
        VERSION
        2.9.0
        GIT_TAG
        v2.9.0
        DOWNLOAD_ONLY
        YES)
    endif()
    if(NOT utf8proc_SOURCE_DIR)
      message(FATAL_ERROR "need_utf8proc: utf8proc sources were not fetched")
    endif()
    # utf8proc.c includes utf8proc_data.c itself. The static archive links into
    # our shared libraries, so it must be PIC.
    add_library(utf8proc_static STATIC ${utf8proc_SOURCE_DIR}/utf8proc.c)
    target_include_directories(utf8proc_static SYSTEM
                               PUBLIC ${utf8proc_SOURCE_DIR})
    target_compile_definitions(utf8proc_static PUBLIC UTF8PROC_STATIC)
    target_compile_options(utf8proc_static PRIVATE -w)
    set_target_properties(utf8proc_static PROPERTIES POSITION_INDEPENDENT_CODE
                                                     ON)
    dftracer_utils_ok("Built utf8proc ${utf8proc_VERSION} with CPM")
  endif()
endfunction()

# ==============================================================================
# Vectorscan (static, external build); Ragel and Boost headers are build-only
# ==============================================================================

function(need_vectorscan)
  if(TARGET dftracer_utils_hs)
    return()
  endif()
  include(ExternalProject)

  set(hs_version 5.4.12)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES
                                            "x86_64|AMD64")
    set(hs_fat ON)
  else()
    set(hs_fat OFF)
  endif()
  # RelWithDebInfo by default, so Valgrind reports name hs_scan and the
  # suppression for Vectorscan's deliberate over-reads can match it. Wheels
  # set DFTRACER_UTILS_DEPS_BUILD_TYPE=Release.
  set(hs_build_type ${DFTRACER_UTILS_DEPS_BUILD_TYPE})
  set(hs_args "")
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND hs_args
         "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
  if(CMAKE_OSX_ARCHITECTURES)
    list(APPEND hs_args
         "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
  endif()
  foreach(lang C CXX)
    if(CMAKE_${lang}_COMPILER_ID STREQUAL "GNU")
      list(APPEND hs_args "-DCMAKE_${lang}_FLAGS=-Wno-psabi")
    endif()
  endforeach()
  set(hs_fixed_args
      -Wno-dev
      -DCMAKE_BUILD_TYPE=${hs_build_type}
      -DCMAKE_INSTALL_LIBDIR=lib
      -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
      -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
      -DBUILD_STATIC_LIBS=ON
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_UNIT=OFF
      -DBUILD_TOOLS=OFF
      -DBUILD_EXAMPLES=OFF
      -DBUILD_BENCHMARKS=OFF
      -DBUILD_DOC=OFF
      -DBUILD_CHIMERA=OFF
      -DFAT_RUNTIME=${hs_fat})

  # Everything that changes the installed library. An existing install is
  # reused only when the fingerprint stored beside it equals this one, so a
  # copy built with other flags, another compiler or another version is
  # rebuilt, never used.
  string(
    SHA256 hs_fingerprint
    "vectorscan ${hs_version}|${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}|${CMAKE_C_COMPILER} ${CMAKE_C_COMPILER_VERSION}|${CMAKE_CXX_COMPILER} ${CMAKE_CXX_COMPILER_VERSION}|${hs_fixed_args}|${hs_args}"
  )
  string(SUBSTRING "${hs_fingerprint}" 0 16 hs_fingerprint_short)
  if(DFTRACER_UTILS_DEPS_PREFIX)
    set(hs_prefix
        "${DFTRACER_UTILS_DEPS_PREFIX}/vectorscan-${hs_fingerprint_short}")
  else()
    set(hs_prefix "${CMAKE_BINARY_DIR}/_deps/vectorscan-install")
  endif()
  set(hs_lib "${hs_prefix}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}hs${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(hs_stamp "${hs_prefix}/.dftracer-fingerprint")

  add_library(dftracer_utils_hs STATIC IMPORTED GLOBAL)
  set_target_properties(
    dftracer_utils_hs PROPERTIES IMPORTED_LOCATION "${hs_lib}"
                                 INTERFACE_INCLUDE_DIRECTORIES
                                 "${hs_prefix}/include/hs")

  if(EXISTS "${hs_lib}" AND EXISTS "${hs_stamp}")
    file(READ "${hs_stamp}" hs_stored)
    string(STRIP "${hs_stored}" hs_stored)
    if(hs_stored STREQUAL hs_fingerprint)
      dftracer_utils_ok("Vectorscan ${hs_version} reused from ${hs_prefix}")
      return()
    endif()
  endif()

  find_program(RAGEL ragel)
  if(NOT RAGEL)
    cpmaddpackage(
      NAME
      ragel
      URL
      https://www.colm.net/files/ragel/ragel-6.10.tar.gz
      DOWNLOAD_ONLY
      YES)
    set(ragel_prefix "${CMAKE_BINARY_DIR}/_deps/ragel-install")
    set(ragel_build "${CMAKE_BINARY_DIR}/_deps/ragel-build")
    set(RAGEL "${ragel_prefix}/bin/ragel")
    if(NOT EXISTS "${RAGEL}")
      dftracer_utils_status("Building Ragel 6.10 for Vectorscan")
      # Extraction leaves Makefile.am newer than Makefile.in, which makes
      # make re-run automake-1.15; touching the generated files prevents it.
      file(GLOB ragel_generated "${ragel_SOURCE_DIR}/aclocal.m4"
           "${ragel_SOURCE_DIR}/configure" "${ragel_SOURCE_DIR}/Makefile.in"
           "${ragel_SOURCE_DIR}/*/Makefile.in"
           "${ragel_SOURCE_DIR}/*/config.h.in")
      file(TOUCH ${ragel_generated})
      file(MAKE_DIRECTORY "${ragel_build}")
      foreach(step
              "${ragel_SOURCE_DIR}/configure;--prefix=${ragel_prefix};--disable-manual"
              "make;-C;ragel;-j4" "make;-C;ragel;install")
        execute_process(
          COMMAND ${step}
          WORKING_DIRECTORY "${ragel_build}"
          RESULT_VARIABLE ragel_rc
          OUTPUT_VARIABLE ragel_out
          ERROR_VARIABLE ragel_out)
        if(NOT ragel_rc EQUAL 0)
          message(FATAL_ERROR
            "Vectorscan needs Ragel and building ragel-6.10 failed "
            "(${step}):\n${ragel_out}\n"
            "Install ragel or set -DDFTRACER_UTILS_ENABLE_VECTORSCAN=OFF")
        endif()
      endforeach()
    endif()
  endif()

  find_package(Boost 1.61 CONFIG QUIET)
  if(Boost_FOUND AND TARGET Boost::headers)
    get_target_property(boost_inc Boost::headers INTERFACE_INCLUDE_DIRECTORIES)
    set(boost_args "-DBoost_INCLUDE_DIR=${boost_inc}")
  else()
    cpmaddpackage(
      NAME
      boost_headers
      URL
      https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz
      DOWNLOAD_ONLY
      YES)
    set(boost_args "-DBOOST_ROOT=${boost_headers_SOURCE_DIR}"
                   "-DBoost_INCLUDE_DIR=${boost_headers_SOURCE_DIR}")
  endif()

  cpmaddpackage(
    NAME
    vectorscan
    URL
    https://github.com/VectorCamp/vectorscan/archive/refs/tags/vectorscan/${hs_version}.tar.gz
    DOWNLOAD_ONLY
    YES)

  foreach(lang C CXX)
    if(CMAKE_${lang}_COMPILER_LAUNCHER)
      set(hs_launcher "${CMAKE_${lang}_COMPILER_LAUNCHER}")
      get_filename_component(hs_launcher_name "${hs_launcher}" NAME)
      if(hs_launcher_name STREQUAL "sccache")
        set(hs_launcher
            "${PROJECT_SOURCE_DIR}/cmake/scripts/sccache_launcher.sh")
      endif()
      list(APPEND hs_args "-DCMAKE_${lang}_COMPILER_LAUNCHER=${hs_launcher}")
    endif()
  endforeach()
  file(MAKE_DIRECTORY "${hs_prefix}/include/hs")

  ExternalProject_Add(
    dftracer_utils_vectorscan_ep
    SOURCE_DIR "${vectorscan_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/vectorscan-build"
    INSTALL_DIR "${hs_prefix}"
    CMAKE_ARGS ${hs_fixed_args}
               -DCMAKE_INSTALL_PREFIX=${hs_prefix}
               -DRAGEL=${RAGEL}
               ${boost_args}
               ${hs_args}
    BUILD_BYPRODUCTS "${hs_lib}"
    USES_TERMINAL_BUILD ON)
  # Written only after the install step succeeded, so an interrupted build
  # never leaves a prefix that looks complete.
  file(WRITE "${CMAKE_BINARY_DIR}/_deps/vectorscan.fingerprint"
       "${hs_fingerprint}")
  ExternalProject_Add_Step(
    dftracer_utils_vectorscan_ep fingerprint
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_BINARY_DIR}/_deps/vectorscan.fingerprint" "${hs_stamp}"
    DEPENDEES install)

  add_dependencies(dftracer_utils_hs dftracer_utils_vectorscan_ep)
  dftracer_utils_ok("Vectorscan ${hs_version} external static build (${hs_lib})")
endfunction()

# ==============================================================================
# Boost.Math (standalone, header-only); for statistical distributions
# ==============================================================================

function(need_boost_math)
  if(NOT boost_math_ADDED)
    cpmaddpackage(
      NAME
      boost_math
      GITHUB_REPOSITORY
      boostorg/math
      GIT_TAG
      boost-1.91.0
      DOWNLOAD_ONLY
      YES)
  endif()

  # CPMAddPackage only sets boost_math_SOURCE_DIR in the calling scope. Cache it
  # so link_boost_math() can find the include dir from anywhere in the tree.
  if(boost_math_SOURCE_DIR)
    set(boost_math_SOURCE_DIR
        "${boost_math_SOURCE_DIR}"
        CACHE INTERNAL "Boost.Math source tree from CPM")
    dftracer_utils_ok(
      "Added Boost.Math (standalone) headers from ${boost_math_SOURCE_DIR}/include"
    )
  endif()
endfunction()

# Apply Boost.Math standalone headers + BOOST_MATH_STANDALONE define as PRIVATE
# build-only properties. We deliberately avoid an INTERFACE link target so the
# headers/defines never enter the installed/exported target set.
function(link_boost_math TARGET_NAME)
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_boost_math: TARGET_NAME is required")
  endif()
  if(NOT TARGET ${TARGET_NAME})
    message(
      FATAL_ERROR "link_boost_math: target '${TARGET_NAME}' does not exist")
  endif()
  if(NOT boost_math_SOURCE_DIR)
    message(
      FATAL_ERROR
        "link_boost_math: boost_math_SOURCE_DIR is unset; call need_boost_math() first"
    )
  endif()

  target_include_directories(${TARGET_NAME} SYSTEM
                             PRIVATE ${boost_math_SOURCE_DIR}/include)
  target_compile_definitions(${TARGET_NAME} PRIVATE BOOST_MATH_STANDALONE)
  dftracer_utils_ok("Linked ${TARGET_NAME} to Boost.Math (standalone)")
endfunction()

# ==============================================================================
# yaml-cpp - YAML emit/parse for DLIO config generation
# ==============================================================================

function(need_yaml_cpp)
  if(NOT yaml-cpp_ADDED)
    cpmaddpackage(
      NAME
      yaml-cpp
      GITHUB_REPOSITORY
      jbeder/yaml-cpp
      GIT_TAG
      yaml-cpp-0.9.0
      OPTIONS
      "YAML_CPP_BUILD_TESTS OFF"
      "YAML_CPP_BUILD_TOOLS OFF"
      "YAML_CPP_BUILD_CONTRIB OFF"
      "YAML_BUILD_SHARED_LIBS OFF"
      "YAML_CPP_INSTALL ON"
      FORCE
      YES)
  endif()
endfunction()

# Link yaml-cpp PRIVATE so the static library is bundled into the consumer and
# the header path stays out of the installed/exported target set.
function(link_yaml_cpp TARGET_NAME)
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_yaml_cpp: TARGET_NAME is required")
  endif()
  if(NOT TARGET ${TARGET_NAME})
    message(FATAL_ERROR "link_yaml_cpp: target '${TARGET_NAME}' does not exist")
  endif()
  if(NOT TARGET yaml-cpp::yaml-cpp)
    message(
      FATAL_ERROR
        "link_yaml_cpp: yaml-cpp::yaml-cpp target missing; call need_yaml_cpp() first"
    )
  endif()
  target_link_libraries(${TARGET_NAME} PRIVATE yaml-cpp::yaml-cpp)
  dftracer_utils_ok("Linked ${TARGET_NAME} to yaml-cpp")
endfunction()

# ==============================================================================
# Testing Dependencies
# ==============================================================================

function(need_test_deps)
  cpmaddpackage(NAME doctest GITHUB_REPOSITORY doctest/doctest VERSION 2.4.11)

  cpmaddpackage(
    NAME
    unity
    GITHUB_REPOSITORY
    ThrowTheSwitch/Unity
    VERSION
    2.6.0
    DOWNLOAD_ONLY
    YES)

  if(TARGET unity)
    add_library(unity_lib ALIAS unity)
  else()
    add_library(unity_lib STATIC ${unity_SOURCE_DIR}/src/unity.c)
    target_include_directories(unity_lib PUBLIC ${unity_SOURCE_DIR}/src)
  endif()
endfunction()

# ==============================================================================
# Compiler Feature Checks and Helpers
# ==============================================================================

macro(link_libatomic)
  # libatomic safety net for targets that can't inline the atomic (e.g. 32-bit;
  # every 64-bit target we ship is inline lock-free and does not need it). Only
  # apply the directory-scope link when dftracer-utils owns the build, never
  # when it is consumed via add_subdirectory, so the link can't leak into a
  # parent project's targets.
  find_library(DFTRACER_UTILS_LIBATOMIC atomic)
  if(DFTRACER_UTILS_LIBATOMIC AND CMAKE_SOURCE_DIR STREQUAL
                                  CMAKE_CURRENT_SOURCE_DIR)
    link_libraries(${DFTRACER_UTILS_LIBATOMIC})
  endif()
endmacro()
