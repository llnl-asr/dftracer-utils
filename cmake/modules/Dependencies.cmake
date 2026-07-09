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
# Logging Dependencies
# ==============================================================================

function(need_cpplogger)
  # First try to find cpp-logger from the system or other projects
  find_package(cpp-logger 0.0.7 QUIET)

  if(cpp-logger_FOUND)
    dftracer_utils_ok("Found system cpp-logger")

    # The system package should provide cpp-logger::cpp-logger target If for
    # some reason it doesn't, try to create it from cpp-logger target
    if(NOT TARGET cpp-logger::cpp-logger AND TARGET cpp-logger)
      dftracer_utils_ok(
        "Creating cpp-logger::cpp-logger alias for system cpp-logger")
      add_library(cpp-logger::cpp-logger ALIAS cpp-logger)
    endif()

    # Set variables in parent scope so they persist outside the function
    set(cpp-logger_FOUND
        ${cpp-logger_FOUND}
        PARENT_SCOPE)
    set(cpplogger_CPM
        FALSE
        PARENT_SCOPE)
  else()
    # Build with CPM
    if(NOT cpplogger_ADDED)
      # Download cpp-logger source without using their CMakeLists.txt This
      # avoids their broken installation paths and gives us full control
      cpmaddpackage(
        NAME
        cpplogger
        GITHUB_REPOSITORY
        hariharan-devarajan/cpp-logger
        VERSION
        0.0.7
        DOWNLOAD_ONLY
        YES)
    endif()

    if(cpplogger_ADDED)
      dftracer_utils_ok("Built cpp-logger with CPM (manual target creation)")

      set(CPPLOGGER_TARGETS)

      # Build static library if requested
      if(DFTRACER_UTILS_BUILD_STATIC)
        add_library(
          cpp-logger_static STATIC
          ${cpplogger_SOURCE_DIR}/src/cpp-logger/clogger.cpp
          ${cpplogger_SOURCE_DIR}/src/cpp-logger/logger.cpp)
        target_include_directories(
          cpp-logger_static
          PUBLIC $<BUILD_INTERFACE:${cpplogger_SOURCE_DIR}/include>
                 $<INSTALL_INTERFACE:include>)
        set_target_properties(
          cpp-logger_static
          PROPERTIES OUTPUT_NAME cpp-logger ARCHIVE_OUTPUT_DIRECTORY
                                            ${CMAKE_BINARY_DIR}/lib)
        # Suppress GNU extension warning from cpp-logger's use of ##__VA_ARGS__
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
          target_compile_options(cpp-logger_static PRIVATE -Wno-gnu-zero-variadic-macro-arguments)
        endif()
        add_library(cpp-logger::cpp-logger_static ALIAS cpp-logger_static)
        list(APPEND CPPLOGGER_TARGETS cpp-logger_static)
        dftracer_utils_ok("Added cpp-logger static library")
      endif()

      # Build shared library if requested
      if(DFTRACER_UTILS_BUILD_SHARED)
        add_library(
          cpp-logger_shared SHARED
          ${cpplogger_SOURCE_DIR}/src/cpp-logger/clogger.cpp
          ${cpplogger_SOURCE_DIR}/src/cpp-logger/logger.cpp)
        target_include_directories(
          cpp-logger_shared
          PUBLIC $<BUILD_INTERFACE:${cpplogger_SOURCE_DIR}/include>
                 $<INSTALL_INTERFACE:include>)
        set_target_properties(
          cpp-logger_shared
          PROPERTIES OUTPUT_NAME cpp-logger
                     LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                     ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
        # Suppress GNU extension warning from cpp-logger's use of ##__VA_ARGS__
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
          target_compile_options(cpp-logger_shared PRIVATE -Wno-gnu-zero-variadic-macro-arguments)
        endif()
        add_library(cpp-logger::cpp-logger ALIAS cpp-logger_shared)
        list(APPEND CPPLOGGER_TARGETS cpp-logger_shared)
        dftracer_utils_ok("Added cpp-logger shared library")
      elseif(DFTRACER_UTILS_BUILD_STATIC)
        # If only static is built, make it the default alias
        add_library(cpp-logger::cpp-logger ALIAS cpp-logger_static)
      endif()

      # Install headers
      install(
        DIRECTORY ${cpplogger_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp")

      # Install the libraries
      if(CPPLOGGER_TARGETS)
        install(
          TARGETS ${CPPLOGGER_TARGETS}
          EXPORT cpp-logger-targets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

        # Install export Note: Using cpp-logger-targets.cmake to match what
        # their config file expects (the original cpp-logger has an
        # inconsistency between export and config)
        install(
          EXPORT cpp-logger-targets
          FILE cpp-logger-targets.cmake
          NAMESPACE cpp-logger::
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cpp-logger)
      endif()

      set(cpplogger_CPM
          TRUE
          PARENT_SCOPE)
    endif()

    if(cpplogger_ADDED AND SKBUILD)
      set(DEPENDENCY_LIBRARY_DIRS
          "${DEPENDENCY_LIBRARY_DIRS}" ${Python_SITELIB}/${CMAKE_INSTALL_LIBDIR}
          PARENT_SCOPE)
    endif()
  endif()
endfunction()

function(link_cpp_logger TARGET_NAME LIBRARY_TYPE)
  # Validate parameters
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_cpp_logger: TARGET_NAME is required")
  endif()

  if(NOT LIBRARY_TYPE MATCHES "^(STATIC|SHARED)$")
    message(
      FATAL_ERROR
        "link_cpp_logger: LIBRARY_TYPE must be either STATIC or SHARED")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(
      FATAL_ERROR "link_cpp_logger: Target '${TARGET_NAME}' does not exist")
  endif()

  # Link appropriate cpp-logger variant Use PUBLIC linkage since cpp-logger
  # headers are included in public headers
  if(LIBRARY_TYPE STREQUAL "STATIC")
    # For static libraries, prefer static cpp-logger if available
    if(TARGET cpp-logger_static)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger_static)
      dftracer_utils_ok("Linked ${TARGET_NAME} to cpp-logger_static")
    elseif(TARGET cpp-logger_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger)
      dftracer_utils_ok("Linked ${TARGET_NAME} to cpp-logger (shared)")
    elseif(TARGET cpp-logger::cpp-logger)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger)
      dftracer_utils_ok("Linked ${TARGET_NAME} to system cpp-logger")
    else()
      message(
        FATAL_ERROR
          "link_cpp_logger: No cpp-logger found! Call need_cpplogger() first.")
    endif()
  else() # SHARED
    # For shared libraries, prefer shared cpp-logger if available
    if(TARGET cpp-logger_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger)
      dftracer_utils_ok("Linked ${TARGET_NAME} to cpp-logger (shared)")
    elseif(TARGET cpp-logger_static)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger_static)
      dftracer_utils_ok("Linked ${TARGET_NAME} to cpp-logger_static")
    elseif(TARGET cpp-logger::cpp-logger)
      target_link_libraries(${TARGET_NAME} PUBLIC cpp-logger::cpp-logger)
      dftracer_utils_ok("Linked ${TARGET_NAME} to system cpp-logger")
    else()
      message(
        FATAL_ERROR
          "link_cpp_logger: No cpp-logger found! Call need_cpplogger() first.")
    endif()
  endif()
endfunction()

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

function(need_ghc_filesystem)
  if(NOT ghc_filesystem_ADDED)
    cpmaddpackage(
      NAME
      ghc_filesystem
      GITHUB_REPOSITORY
      gulrak/filesystem
      VERSION
      1.5.14
      OPTIONS
      "GHC_FILESYSTEM_WITH_INSTALL ON"
      FORCE
      YES)
  endif()
endfunction()

function(need_nonstd_span)
  if(NOT nonstd_span_ADDED)
    cpmaddpackage(
      NAME
      nonstd_span
      GITHUB_REPOSITORY
      nonstd-lite/span-lite
      VERSION
      0.11.0)
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
endfunction()

function(link_unordered_dense TARGET_NAME)
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_unordered_dense: TARGET_NAME is required")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(
      FATAL_ERROR
        "link_unordered_dense: Target '${TARGET_NAME}' does not exist")
  endif()

  if(NOT TARGET unordered_dense::unordered_dense)
    message(
      FATAL_ERROR
        "link_unordered_dense: ankerl::unordered_dense not found! Call need_unordered_dense() first."
    )
  endif()

  get_target_property(UD_INC unordered_dense::unordered_dense
                      INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(${TARGET_NAME} PUBLIC
    "$<BUILD_INTERFACE:${UD_INC}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endfunction()

function(need_tl_expected)
  # tl::expected is only needed when C++23 std::expected is unavailable
  if(CMAKE_CXX_STANDARD GREATER_EQUAL 23)
    dftracer_utils_warn("C++23 detected: using std::expected (skipping tl::expected)")
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
      OPTIONS
      "EXPECTED_BUILD_TESTS OFF"
      FORCE
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
    set(SIMDJSON_SOURCES
      ${simdjson_SOURCE_DIR}/singleheader/simdjson.h
      ${simdjson_SOURCE_DIR}/singleheader/simdjson.cpp)

    set(SIMDJSON_TARGETS)

    if(DFTRACER_UTILS_BUILD_STATIC)
      add_library(simdjson_static STATIC ${SIMDJSON_SOURCES})
      target_include_directories(
        simdjson_static SYSTEM PUBLIC
        $<BUILD_INTERFACE:${simdjson_SOURCE_DIR}/singleheader>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      target_compile_features(simdjson_static PUBLIC cxx_std_17)
      # Suppress warnings from simdjson (third-party code)
      target_compile_options(simdjson_static PRIVATE -w)
      set_target_properties(
        simdjson_static
        PROPERTIES
          OUTPUT_NAME simdjson
          ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
          POSITION_INDEPENDENT_CODE ON)
      add_library(simdjson::simdjson_static ALIAS simdjson_static)
      list(APPEND SIMDJSON_TARGETS simdjson_static)
      dftracer_utils_ok("Added simdjson static library")
    endif()

    if(DFTRACER_UTILS_BUILD_SHARED)
      add_library(simdjson_shared SHARED ${SIMDJSON_SOURCES})
      target_include_directories(
        simdjson_shared SYSTEM PUBLIC
        $<BUILD_INTERFACE:${simdjson_SOURCE_DIR}/singleheader>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      target_compile_features(simdjson_shared PUBLIC cxx_std_17)
      # Suppress warnings from simdjson (third-party code)
      target_compile_options(simdjson_shared PRIVATE -w)
      set_target_properties(
        simdjson_shared
        PROPERTIES
          OUTPUT_NAME simdjson
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
        FATAL_ERROR "link_simdjson: No simdjson found! Call need_simdjson() first.")
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
        FATAL_ERROR "link_simdjson: No simdjson found! Call need_simdjson() first.")
    endif()
  endif()
endfunction()

# ==============================================================================
# RocksDB
# ==============================================================================

set(DFTRACER_UTILS_ROCKSDB_VERSION
    "10.10.1"
    CACHE STRING "RocksDB version to find or build")
set(DFTRACER_UTILS_ROCKSDB_PREFIX
    "$ENV{DFTRACER_UTILS_ROCKSDB_PREFIX}"
    CACHE PATH "Install prefix of a prebuilt RocksDB to use instead of source")

# Consume a RocksDB install tree built by scripts/ci/build_rocksdb.sh.
function(_use_prebuilt_rocksdb PREFIX)
  find_package(RocksDB ${DFTRACER_UTILS_ROCKSDB_VERSION} REQUIRED CONFIG
               PATHS "${PREFIX}" NO_DEFAULT_PATH)
  dftracer_utils_ok("Using prebuilt RocksDB from ${PREFIX}")

  foreach(tool ldb sst_dump)
    if(EXISTS "${PREFIX}/bin/${tool}")
      file(COPY "${PREFIX}/bin/${tool}"
           DESTINATION "${CMAKE_BINARY_DIR}/bin"
           FILE_PERMISSIONS
             OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
             WORLD_READ WORLD_EXECUTE)
      install(PROGRAMS "${PREFIX}/bin/${tool}"
              DESTINATION ${CMAKE_INSTALL_BINDIR})
      if(SKBUILD)
        create_python_wrapper(${tool})
      endif()
    else()
      message(WARNING "Prebuilt RocksDB at ${PREFIX} has no bin/${tool}")
    endif()
  endforeach()
endfunction()

# Function to find or build RocksDB
function(need_rocksdb)
  # TSan needs RocksDB instrumented too, so an uninstrumented prebuilt tree
  # would report false positives; build from source instead.
  if(DFTRACER_UTILS_ROCKSDB_PREFIX AND DFTRACER_UTILS_ENABLE_TSAN)
    dftracer_utils_warn(
      "Ignoring DFTRACER_UTILS_ROCKSDB_PREFIX: TSan requires a source build")
  elseif(DFTRACER_UTILS_ROCKSDB_PREFIX)
    _use_prebuilt_rocksdb("${DFTRACER_UTILS_ROCKSDB_PREFIX}")
    set(RocksDB_FOUND TRUE PARENT_SCOPE)
    set(RocksDB_CPM FALSE PARENT_SCOPE)
    set(ROCKSDB_IS_STATIC TRUE PARENT_SCOPE)
    return()
  endif()

  if(DFTRACER_UTILS_LOCAL_PACKAGES)
    find_package(RocksDB ${DFTRACER_UTILS_ROCKSDB_VERSION} QUIET CONFIG)
    if(NOT RocksDB_FOUND)
      find_package(rocksdb ${DFTRACER_UTILS_ROCKSDB_VERSION} QUIET CONFIG)
    endif()
    if(NOT RocksDB_FOUND AND rocksdb_FOUND)
      set(RocksDB_FOUND TRUE)
    endif()
    if(NOT RocksDB_FOUND)
      find_package(RocksDB ${DFTRACER_UTILS_ROCKSDB_VERSION} QUIET)
    endif()
  endif()

  if(DFTRACER_UTILS_LOCAL_PACKAGES AND RocksDB_FOUND)
    dftracer_utils_ok("Found system RocksDB")

    if(NOT TARGET RocksDB::rocksdb)
      if(TARGET rocksdb)
        add_library(RocksDB::rocksdb ALIAS rocksdb)
      elseif(TARGET rocksdb-shared)
        add_library(RocksDB::rocksdb ALIAS rocksdb-shared)
      elseif(TARGET RocksDB::RocksDB)
        add_library(RocksDB::rocksdb ALIAS RocksDB::RocksDB)
      elseif(DEFINED RocksDB_LIBRARY AND DEFINED RocksDB_INCLUDE_DIR)
        add_library(RocksDB::rocksdb UNKNOWN IMPORTED)
        set_target_properties(
          RocksDB::rocksdb
          PROPERTIES IMPORTED_LOCATION "${RocksDB_LIBRARY}"
                     INTERFACE_INCLUDE_DIRECTORIES "${RocksDB_INCLUDE_DIR}")
      elseif(DEFINED ROCKSDB_LIBRARIES AND DEFINED ROCKSDB_INCLUDE_DIRS)
        add_library(RocksDB::rocksdb UNKNOWN IMPORTED)
        set_target_properties(
          RocksDB::rocksdb
          PROPERTIES IMPORTED_LOCATION ""
                     INTERFACE_LINK_LIBRARIES "${ROCKSDB_LIBRARIES}"
                     INTERFACE_INCLUDE_DIRECTORIES "${ROCKSDB_INCLUDE_DIRS}")
      endif()
    endif()

    if(NOT TARGET RocksDB::rocksdb)
      message(
        FATAL_ERROR
          "need_rocksdb: RocksDB was found but no usable target could be created."
      )
    endif()

    set(RocksDB_FOUND
        ${RocksDB_FOUND}
        PARENT_SCOPE)
    set(RocksDB_CPM
        FALSE
        PARENT_SCOPE)
  else()
    if(NOT rocksdb_ADDED)
      cpmaddpackage(
        NAME
        rocksdb
        GITHUB_REPOSITORY
        facebook/rocksdb
        VERSION
        ${DFTRACER_UTILS_ROCKSDB_VERSION}
        GIT_TAG
        v${DFTRACER_UTILS_ROCKSDB_VERSION}
        OPTIONS
        "ROCKSDB_BUILD_SHARED OFF"
        "PORTABLE 1"
        "WITH_TESTS OFF"
        "WITH_TOOLS OFF"
        "WITH_CORE_TOOLS ON"
        "WITH_TRACE_TOOLS OFF"
        "WITH_BENCHMARK_TOOLS OFF"
        "WITH_GFLAGS OFF"
        "WITH_SNAPPY OFF"
        "WITH_LZ4 ${DFTRACER_UTILS_ENABLE_LZ4}"
        "WITH_ZLIB ON"
        "WITH_ZSTD ${DFTRACER_UTILS_ENABLE_ZSTD}"
        "WITH_BZ2 OFF"
        "USE_RTTI ON"
        "FAIL_ON_WARNINGS OFF"
        FORCE
        YES)
    endif()

    if(TARGET rocksdb AND NOT TARGET RocksDB::rocksdb_static)
      add_library(RocksDB::rocksdb_static ALIAS rocksdb)
    endif()
    if(TARGET rocksdb-shared AND NOT TARGET RocksDB::rocksdb_shared)
      add_library(RocksDB::rocksdb_shared ALIAS rocksdb-shared)
    endif()
    if(NOT TARGET RocksDB::rocksdb)
      if(TARGET RocksDB::rocksdb_shared)
        add_library(RocksDB::rocksdb ALIAS rocksdb-shared)
      elseif(TARGET RocksDB::rocksdb_static)
        add_library(RocksDB::rocksdb ALIAS rocksdb)
      endif()
    endif()

    if(rocksdb_ADDED OR TARGET rocksdb OR TARGET rocksdb-shared)
      dftracer_utils_ok("Built RocksDB with CPM")

      set(ROCKSDB_LIBRARY_DIR "${CMAKE_BINARY_DIR}/lib")

      if(TARGET rocksdb)
        set_target_properties(
          rocksdb
          PROPERTIES POSITION_INDEPENDENT_CODE ON
                     ARCHIVE_OUTPUT_DIRECTORY "${ROCKSDB_LIBRARY_DIR}"
                     LIBRARY_OUTPUT_DIRECTORY "${ROCKSDB_LIBRARY_DIR}"
                     RUNTIME_OUTPUT_DIRECTORY "${ROCKSDB_LIBRARY_DIR}")
        target_compile_definitions(rocksdb PUBLIC ROCKSDB_USE_RTTI)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
          target_compile_options(rocksdb PRIVATE -frtti)
          target_compile_options(rocksdb PUBLIC -Wno-conversion)
        endif()
        install(
          TARGETS rocksdb
          EXPORT rocksdbTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
      endif()
      if(TARGET rocksdb-shared)
        set_target_properties(
          rocksdb-shared
          PROPERTIES POSITION_INDEPENDENT_CODE ON
                     ARCHIVE_OUTPUT_DIRECTORY "${ROCKSDB_LIBRARY_DIR}"
                     LIBRARY_OUTPUT_DIRECTORY "${ROCKSDB_LIBRARY_DIR}"
                     RUNTIME_OUTPUT_DIRECTORY "${ROCKSDB_LIBRARY_DIR}")
        target_compile_definitions(rocksdb-shared PUBLIC ROCKSDB_USE_RTTI)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
          target_compile_options(rocksdb-shared PRIVATE -frtti)
          target_compile_options(rocksdb-shared PUBLIC -Wno-conversion)
        endif()
        install(
          TARGETS rocksdb-shared
          EXPORT rocksdbTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
      endif()

      list(APPEND DEPENDENCY_LIBRARY_DIRS "${ROCKSDB_LIBRARY_DIR}")
      list(REMOVE_DUPLICATES DEPENDENCY_LIBRARY_DIRS)
      set(DEPENDENCY_LIBRARY_DIRS
          "${DEPENDENCY_LIBRARY_DIRS}"
          PARENT_SCOPE)

      list(APPEND CMAKE_BUILD_RPATH "${ROCKSDB_LIBRARY_DIR}")
      list(REMOVE_DUPLICATES CMAKE_BUILD_RPATH)
      set(CMAKE_BUILD_RPATH
          "${CMAKE_BUILD_RPATH}"
          PARENT_SCOPE)

      list(APPEND CMAKE_INSTALL_RPATH "${ROCKSDB_LIBRARY_DIR}")
      list(REMOVE_DUPLICATES CMAKE_INSTALL_RPATH)
      set(CMAKE_INSTALL_RPATH
          "${CMAKE_INSTALL_RPATH}"
          PARENT_SCOPE)

      # Stage rocksdb's ldb (and sst_dump) into bin/ and reuse the standard
      # $ORIGIN/../lib rpath helper so they find librocksdb.so without
      # LD_LIBRARY_PATH. Install alongside our own binaries and ship a
      # venv wrapper when building a Python wheel.
      foreach(tool ldb sst_dump)
        if(TARGET ${tool})
          set_target_properties(
            ${tool} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                               "${CMAKE_BINARY_DIR}/bin")
          target_add_rpath(${tool})
          install(TARGETS ${tool} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
          if(SKBUILD)
            create_python_wrapper(${tool})
          endif()
        endif()
      endforeach()

      set(RocksDB_FOUND
          TRUE
          PARENT_SCOPE)
      set(RocksDB_CPM
          TRUE
          PARENT_SCOPE)
      set(ROCKSDB_IS_STATIC
          TRUE
          PARENT_SCOPE)
    endif()
  endif()
endfunction()

function(link_rocksdb TARGET_NAME LIBRARY_TYPE)
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_rocksdb: TARGET_NAME is required")
  endif()

  if(NOT LIBRARY_TYPE MATCHES "^(STATIC|SHARED)$")
    message(
      FATAL_ERROR "link_rocksdb: LIBRARY_TYPE must be either STATIC or SHARED")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(FATAL_ERROR "link_rocksdb: Target '${TARGET_NAME}' does not exist")
  endif()

  if(NOT TARGET RocksDB::rocksdb AND NOT TARGET RocksDB::rocksdb_static
     AND NOT TARGET RocksDB::rocksdb_shared AND NOT TARGET rocksdb
     AND NOT TARGET rocksdb-shared)
    message(
      FATAL_ERROR
        "link_rocksdb: No RocksDB found! Call need_rocksdb() first or ensure system RocksDB is available."
    )
  endif()

  if(LIBRARY_TYPE STREQUAL "STATIC")
    if(TARGET RocksDB::rocksdb_static)
      target_link_libraries(${TARGET_NAME} PUBLIC RocksDB::rocksdb_static)
      dftracer_utils_ok("Linked ${TARGET_NAME} to RocksDB::rocksdb_static")
    elseif(TARGET rocksdb)
      target_link_libraries(${TARGET_NAME} PUBLIC rocksdb)
      dftracer_utils_ok("Linked ${TARGET_NAME} to rocksdb")
    elseif(TARGET RocksDB::rocksdb)
      target_link_libraries(${TARGET_NAME} PUBLIC RocksDB::rocksdb)
      dftracer_utils_ok("Linked ${TARGET_NAME} to RocksDB::rocksdb")
    else()
      message(FATAL_ERROR "Static RocksDB requested for ${TARGET_NAME}, but no static RocksDB target is available")
    endif()
  else()
    if(TARGET RocksDB::rocksdb_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC RocksDB::rocksdb_shared)
      dftracer_utils_ok("Linked ${TARGET_NAME} to RocksDB::rocksdb_shared")
    elseif(TARGET rocksdb-shared)
      target_link_libraries(${TARGET_NAME} PUBLIC rocksdb-shared)
      dftracer_utils_ok("Linked ${TARGET_NAME} to rocksdb-shared")
    elseif(TARGET RocksDB::rocksdb)
      target_link_libraries(${TARGET_NAME} PUBLIC RocksDB::rocksdb)
      dftracer_utils_ok("Linked ${TARGET_NAME} to RocksDB::rocksdb")
    elseif(TARGET RocksDB::rocksdb_static)
      target_link_libraries(${TARGET_NAME} PUBLIC RocksDB::rocksdb_static)
      dftracer_utils_ok("Linked ${TARGET_NAME} to RocksDB::rocksdb_static")
    elseif(TARGET rocksdb)
      target_link_libraries(${TARGET_NAME} PUBLIC rocksdb)
      dftracer_utils_ok("Linked ${TARGET_NAME} to rocksdb")
    endif()
  endif()
endfunction()

# ==============================================================================
# Compression Dependencies
# ==============================================================================

function(need_lz4)
  if(DEFINED CACHE{lz4_LIBRARIES} AND NOT EXISTS "${lz4_LIBRARIES}")
    unset(lz4_LIBRARIES CACHE)
  endif()
  if(DEFINED CACHE{lz4_INCLUDE_DIRS} AND NOT EXISTS "${lz4_INCLUDE_DIRS}")
    unset(lz4_INCLUDE_DIRS CACHE)
  endif()

  find_path(lz4_INCLUDE_DIRS NAMES lz4.h)
  find_library(lz4_LIBRARIES NAMES lz4)

  if(lz4_INCLUDE_DIRS AND lz4_LIBRARIES AND EXISTS "${lz4_LIBRARIES}")
    dftracer_utils_ok("Found system lz4: ${lz4_LIBRARIES}")

    if(NOT TARGET lz4::lz4)
      add_library(lz4::lz4 UNKNOWN IMPORTED)
      set_target_properties(
        lz4::lz4
        PROPERTIES IMPORTED_LOCATION "${lz4_LIBRARIES}"
                   INTERFACE_INCLUDE_DIRECTORIES "${lz4_INCLUDE_DIRS}")
    endif()

    set(lz4_FOUND
        TRUE
        PARENT_SCOPE)
    set(lz4_INCLUDE_DIRS
        ${lz4_INCLUDE_DIRS}
        PARENT_SCOPE)
    set(lz4_LIBRARIES
        ${lz4_LIBRARIES}
        PARENT_SCOPE)
    set(lz4_CPM
        FALSE
        PARENT_SCOPE)
    set(lz4_FOUND
        TRUE
        CACHE BOOL "lz4 availability" FORCE)
    set(lz4_INCLUDE_DIRS
        "${lz4_INCLUDE_DIRS}"
        CACHE PATH "lz4 include directories" FORCE)
    set(lz4_LIBRARIES
        "${lz4_LIBRARIES}"
        CACHE STRING "lz4 libraries" FORCE)
  else()
    if(NOT lz4_ADDED)
      cpmaddpackage(
        NAME
        lz4
        GITHUB_REPOSITORY
        lz4/lz4
        VERSION
        1.10.0
        GIT_TAG
        v1.10.0
        DOWNLOAD_ONLY
        YES)
    endif()

    if(lz4_ADDED)
      dftracer_utils_ok("Built lz4 with CPM")

      set(LZ4_TARGETS)
      set(LZ4_SOURCES
          ${lz4_SOURCE_DIR}/lib/lz4.c
          ${lz4_SOURCE_DIR}/lib/lz4frame.c
          ${lz4_SOURCE_DIR}/lib/lz4hc.c
          ${lz4_SOURCE_DIR}/lib/xxhash.c)
      set(LZ4_SHARED_OUTPUT
          "${CMAKE_BINARY_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}lz4${CMAKE_SHARED_LIBRARY_SUFFIX}"
      )
      set(LZ4_STATIC_OUTPUT
          "${CMAKE_BINARY_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}lz4${CMAKE_STATIC_LIBRARY_SUFFIX}"
      )
      set(LZ4_PREFERRED_OUTPUT "${LZ4_STATIC_OUTPUT}")
      if(DFTRACER_UTILS_BUILD_SHARED)
        set(LZ4_PREFERRED_OUTPUT "${LZ4_SHARED_OUTPUT}")
      endif()

      if(DFTRACER_UTILS_BUILD_STATIC)
        add_library(lz4_static STATIC ${LZ4_SOURCES})
        target_include_directories(
          lz4_static
          PUBLIC $<BUILD_INTERFACE:${lz4_SOURCE_DIR}/lib>
                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        set_target_properties(
          lz4_static
          PROPERTIES OUTPUT_NAME lz4
                     ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                     LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
        list(APPEND LZ4_TARGETS lz4_static)
      endif()

      if(DFTRACER_UTILS_BUILD_SHARED)
        add_library(lz4_shared SHARED ${LZ4_SOURCES})
        target_include_directories(
          lz4_shared
          PUBLIC $<BUILD_INTERFACE:${lz4_SOURCE_DIR}/lib>
                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        set_target_properties(
          lz4_shared
          PROPERTIES OUTPUT_NAME lz4
                     ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                     LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
        list(APPEND LZ4_TARGETS lz4_shared)
      endif()

      if(TARGET lz4_static AND NOT TARGET lz4::lz4_static)
        add_library(lz4::lz4_static UNKNOWN IMPORTED GLOBAL)
        set_target_properties(
          lz4::lz4_static
          PROPERTIES IMPORTED_LOCATION "${LZ4_STATIC_OUTPUT}"
                     INTERFACE_INCLUDE_DIRECTORIES "${lz4_SOURCE_DIR}/lib")
        add_dependencies(lz4::lz4_static lz4_static)
      endif()
      if(TARGET lz4_shared AND NOT TARGET lz4::lz4_shared)
        add_library(lz4::lz4_shared UNKNOWN IMPORTED GLOBAL)
        set_target_properties(
          lz4::lz4_shared
          PROPERTIES IMPORTED_LOCATION "${LZ4_SHARED_OUTPUT}"
                     INTERFACE_INCLUDE_DIRECTORIES "${lz4_SOURCE_DIR}/lib")
        add_dependencies(lz4::lz4_shared lz4_shared)
      endif()
      if(NOT TARGET lz4::lz4)
        add_library(lz4::lz4 UNKNOWN IMPORTED GLOBAL)
        if(TARGET lz4::lz4_shared)
          set_target_properties(
            lz4::lz4
            PROPERTIES IMPORTED_LOCATION "${LZ4_SHARED_OUTPUT}"
                       INTERFACE_INCLUDE_DIRECTORIES "${lz4_SOURCE_DIR}/lib")
          add_dependencies(lz4::lz4 lz4_shared)
        elseif(TARGET lz4::lz4_static)
          set_target_properties(
            lz4::lz4
            PROPERTIES IMPORTED_LOCATION "${LZ4_STATIC_OUTPUT}"
                       INTERFACE_INCLUDE_DIRECTORIES "${lz4_SOURCE_DIR}/lib")
          add_dependencies(lz4::lz4 lz4_static)
        endif()
      endif()

      install(FILES ${lz4_SOURCE_DIR}/lib/lz4.h ${lz4_SOURCE_DIR}/lib/lz4frame.h
                    ${lz4_SOURCE_DIR}/lib/lz4hc.h ${lz4_SOURCE_DIR}/lib/xxhash.h
              DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

      if(LZ4_TARGETS)
        install(
          TARGETS ${LZ4_TARGETS}
          EXPORT lz4Targets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(
          EXPORT lz4Targets
          FILE lz4Targets.cmake
          NAMESPACE lz4::
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lz4)
      endif()

      set(lz4_FOUND
          TRUE
          PARENT_SCOPE)
      set(lz4_INCLUDE_DIRS
          ${lz4_SOURCE_DIR}/lib
          PARENT_SCOPE)
      set(lz4_LIBRARIES
          ${LZ4_PREFERRED_OUTPUT}
          PARENT_SCOPE)
      set(lz4_CPM
          TRUE
          PARENT_SCOPE)

      # Seed the variables RocksDB's bundled Findlz4.cmake checks.
      set(lz4_FOUND
          TRUE
          CACHE BOOL "lz4 availability" FORCE)
      set(lz4_INCLUDE_DIRS
          "${lz4_SOURCE_DIR}/lib"
          CACHE PATH "lz4 include directories" FORCE)
      set(lz4_LIBRARIES
          "${LZ4_PREFERRED_OUTPUT}"
          CACHE STRING "lz4 libraries" FORCE)
    endif()
  endif()
endfunction()

function(_try_zlib_ng OUT_VAR)
  set(${OUT_VAR}
      FALSE
      PARENT_SCOPE)

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
    "ZLIB_COMPAT ON"
    "ZLIB_ENABLE_TESTS OFF"
    "ZLIBNG_ENABLE_TESTS OFF"
    "WITH_GTEST OFF"
    "WITH_OPTIM ON"
    "WITH_NEW_STRATEGIES ON"
    "WITH_NATIVE_INSTRUCTIONS OFF"
    "INSTALL_UTILS OFF"
    "SKIP_INSTALL_ALL ON")

  if(NOT zlib-ng_ADDED)
    # CPM reports ADDED=NO when the package was already added by an earlier
    # need_zlib() call (e.g. src/ adds it, then tests/ asks again). That is not
    # a failure: the targets already exist globally. Re-expose the dirs and
    # report success, but skip the one-time target/alias/install setup below
    # (re-running it would error on duplicate ALIAS / EXPORT definitions).
    if(TARGET zlib-ng OR TARGET zlib-ng-static)
      set(ZLIB_SOURCE_DIR
          ${zlib-ng_SOURCE_DIR}
          PARENT_SCOPE)
      set(ZLIB_BINARY_DIR
          ${zlib-ng_BINARY_DIR}
          PARENT_SCOPE)
      set(${OUT_VAR}
          TRUE
          PARENT_SCOPE)
      return()
    endif()
    message(WARNING "zlib-ng CPM add failed; will fall back to madler/zlib")
    return()
  endif()

  # zlib-ng compat mode: real targets are `zlib-ng` (shared) and
  # `zlib-ng-static` (static); `zlib`/`zlibstatic` are ALIAS-only and cannot
  # have properties or further aliases set on them.
  set(ZLIB_NG_TARGETS)
  if(DFTRACER_UTILS_BUILD_SHARED AND TARGET zlib-ng)
    get_target_property(_zng_type zlib-ng TYPE)
    if(_zng_type STREQUAL "SHARED_LIBRARY")
      set_target_properties(
        zlib-ng PROPERTIES OUTPUT_NAME dftracer_zlib LIBRARY_OUTPUT_DIRECTORY
                                                     ${CMAKE_BINARY_DIR}/lib)
      target_include_directories(
        zlib-ng PUBLIC $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
      add_library(dftracer_zlib_shared ALIAS zlib-ng)
      add_library(dftracer::zlib ALIAS zlib-ng)
      list(APPEND ZLIB_NG_TARGETS zlib-ng)
      dftracer_utils_ok("Using zlib-ng (compat, shared) as dftracer_zlib")
    endif()
  endif()

  if(DFTRACER_UTILS_BUILD_STATIC AND TARGET zlib-ng-static)
    set_target_properties(
      zlib-ng-static PROPERTIES OUTPUT_NAME dftracer_zlib
                                ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    target_include_directories(
      zlib-ng-static PUBLIC $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    add_library(dftracer_zlib_static ALIAS zlib-ng-static)
    add_library(dftracer::zlibstatic ALIAS zlib-ng-static)
    if(NOT TARGET dftracer::zlib)
      add_library(dftracer::zlib ALIAS zlib-ng-static)
    endif()
    list(APPEND ZLIB_NG_TARGETS zlib-ng-static)
    dftracer_utils_ok("Using zlib-ng (compat, static) as dftracer_zlib")
  endif()

  if(NOT ZLIB_NG_TARGETS)
    message(WARNING "zlib-ng targets not found after CPM add; falling back")
    return()
  endif()

  install(
    TARGETS ${ZLIB_NG_TARGETS}
    EXPORT ZlibTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(
    EXPORT ZlibTargets
    FILE ZlibTargets.cmake
    NAMESPACE dftracer::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlib)

  # Compat headers: zlib-ng generates zlib.h/zconf.h in its binary dir when
  # ZLIB_COMPAT=ON. Fall back to source dir if generated copy is absent.
  foreach(hdr zlib.h zconf.h)
    if(EXISTS "${zlib-ng_BINARY_DIR}/${hdr}")
      install(FILES "${zlib-ng_BINARY_DIR}/${hdr}"
              DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    elseif(EXISTS "${zlib-ng_SOURCE_DIR}/${hdr}")
      install(FILES "${zlib-ng_SOURCE_DIR}/${hdr}"
              DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    endif()
  endforeach()

  set(ZLIB_SOURCE_DIR
      ${zlib-ng_SOURCE_DIR}
      PARENT_SCOPE)
  set(ZLIB_BINARY_DIR
      ${zlib-ng_BINARY_DIR}
      PARENT_SCOPE)
  set(${OUT_VAR}
      TRUE
      PARENT_SCOPE)
endfunction()

function(need_zlib)
  if(DFTRACER_USE_ZLIB_NG)
    _try_zlib_ng(_ZLIB_NG_OK)
    if(_ZLIB_NG_OK)
      set(ZLIB_CPM
          TRUE
          PARENT_SCOPE)
      set(ZLIB_SOURCE_DIR
          ${ZLIB_SOURCE_DIR}
          PARENT_SCOPE)
      set(ZLIB_BINARY_DIR
          ${ZLIB_BINARY_DIR}
          PARENT_SCOPE)
      set(ZLIB_FOUND
          FALSE
          PARENT_SCOPE)
      return()
    endif()
  endif()

  find_package(ZLIB 1.2 QUIET)

  if(ZLIB_FOUND)
    dftracer_utils_ok("Found system ZLIB: ${ZLIB_LIBRARIES}")

    # Set variables in parent scope so they persist outside the function
    set(ZLIB_FOUND
        ${ZLIB_FOUND}
        PARENT_SCOPE)
    set(ZLIB_LIBRARIES
        ${ZLIB_LIBRARIES}
        PARENT_SCOPE)
    set(ZLIB_INCLUDE_DIRS
        ${ZLIB_INCLUDE_DIRS}
        PARENT_SCOPE)
    set(ZLIB_CPM
        FALSE
        PARENT_SCOPE)
  else()
    set(ZLIB_CPM
        FALSE
        PARENT_SCOPE)
    # Build with CPM
    cpmaddpackage(
      NAME
      ZLIB
      GITHUB_REPOSITORY
      madler/zlib
      VERSION
      1.3.1
      OPTIONS
      "ZLIB_BUILD_STATIC OFF"
      "ZLIB_BUILD_SHARED ON"
      "ZLIB_INSTALL OFF"
      "ZLIB_BUILD_EXAMPLES OFF"
      DOWNLOAD_ONLY
      YES)

    if(ZLIB_ADDED)
      dftracer_utils_ok("Built ZLIB with CPM")
      set(ZLIB_CPM
          TRUE
          PARENT_SCOPE)

      # Make sure the source and binary directories are available in parent
      # scope
      set(ZLIB_SOURCE_DIR
          ${ZLIB_SOURCE_DIR}
          PARENT_SCOPE)
      set(ZLIB_BINARY_DIR
          ${ZLIB_BINARY_DIR}
          PARENT_SCOPE)

      # Create our own zlib targets with proper install interface and fix macOS
      # issues
      set(ZLIB_TARGETS)

      if(APPLE)
        # Create patched source files for macOS to fix type conflicts
        set(ZLIB_SOURCES_PATCHED "")
        foreach(src_file adler32.c crc32.c)
          file(READ "${ZLIB_SOURCE_DIR}/${src_file}" SRC_CONTENT)
          # Replace z_off64_t parameter with z_off_t in function definitions to
          # match declarations
          string(REGEX REPLACE "z_off64_t len2\\)" "z_off_t len2)"
                               SRC_CONTENT_FIXED "${SRC_CONTENT}")
          string(REGEX REPLACE "z_off64_t len2\\s*\\{" "z_off_t len2 {"
                               SRC_CONTENT_FIXED "${SRC_CONTENT_FIXED}")
          file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${src_file}"
               "${SRC_CONTENT_FIXED}")
          list(APPEND ZLIB_SOURCES_PATCHED
               "${CMAKE_CURRENT_BINARY_DIR}/${src_file}")
        endforeach()

        # Add the rest of the files normally
        foreach(
          src_file
          compress.c
          deflate.c
          gzclose.c
          gzlib.c
          gzread.c
          gzwrite.c
          inflate.c
          infback.c
          inftrees.c
          inffast.c
          trees.c
          uncompr.c
          zutil.c)
          list(APPEND ZLIB_SOURCES_PATCHED "${ZLIB_SOURCE_DIR}/${src_file}")
        endforeach()

        if(DFTRACER_UTILS_BUILD_SHARED)
          add_library(dftracer_zlib_shared SHARED ${ZLIB_SOURCES_PATCHED})
          list(APPEND ZLIB_TARGETS dftracer_zlib_shared)
        endif()
        if(DFTRACER_UTILS_BUILD_STATIC)
          add_library(dftracer_zlib_static STATIC ${ZLIB_SOURCES_PATCHED})
          list(APPEND ZLIB_TARGETS dftracer_zlib_static)
        endif()
      else()
        set(ZLIB_SOURCES
            ${ZLIB_SOURCE_DIR}/adler32.c
            ${ZLIB_SOURCE_DIR}/compress.c
            ${ZLIB_SOURCE_DIR}/crc32.c
            ${ZLIB_SOURCE_DIR}/deflate.c
            ${ZLIB_SOURCE_DIR}/gzclose.c
            ${ZLIB_SOURCE_DIR}/gzlib.c
            ${ZLIB_SOURCE_DIR}/gzread.c
            ${ZLIB_SOURCE_DIR}/gzwrite.c
            ${ZLIB_SOURCE_DIR}/inflate.c
            ${ZLIB_SOURCE_DIR}/infback.c
            ${ZLIB_SOURCE_DIR}/inftrees.c
            ${ZLIB_SOURCE_DIR}/inffast.c
            ${ZLIB_SOURCE_DIR}/trees.c
            ${ZLIB_SOURCE_DIR}/uncompr.c
            ${ZLIB_SOURCE_DIR}/zutil.c)

        if(DFTRACER_UTILS_BUILD_SHARED)
          add_library(dftracer_zlib_shared SHARED ${ZLIB_SOURCES})
          list(APPEND ZLIB_TARGETS dftracer_zlib_shared)
        endif()
        if(DFTRACER_UTILS_BUILD_STATIC)
          add_library(dftracer_zlib_static STATIC ${ZLIB_SOURCES})
          list(APPEND ZLIB_TARGETS dftracer_zlib_static)
        endif()
      endif()

      # Fix type mismatch issues on macOS by ensuring consistent type
      # definitions
      foreach(zlib_target ${ZLIB_TARGETS})
        if(APPLE)
          target_compile_definitions(
            ${zlib_target} PRIVATE _LARGEFILE64_SOURCE=1 _FILE_OFFSET_BITS=64)
        else()
          target_compile_definitions(
            ${zlib_target} PRIVATE _LARGEFILE64_SOURCE=1 _FILE_OFFSET_BITS=64
                                   Z_HAVE_STDARG_H=1)
        endif()

        # Set proper include directories for build and install
        target_include_directories(
          ${zlib_target}
          PUBLIC $<BUILD_INTERFACE:${ZLIB_SOURCE_DIR}>
                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

        # Copy the generated zconf.h from the original zlib build
        if(EXISTS "${ZLIB_BINARY_DIR}/zconf.h")
          configure_file("${ZLIB_BINARY_DIR}/zconf.h"
                         "${CMAKE_CURRENT_BINARY_DIR}/zconf.h" COPYONLY)
          target_include_directories(
            ${zlib_target}
            PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
        endif()
      endforeach()

      # Set output names and create aliases
      if(DFTRACER_UTILS_BUILD_SHARED)
        set_target_properties(
          dftracer_zlib_shared
          PROPERTIES OUTPUT_NAME dftracer_zlib
                     LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                     ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
        add_library(dftracer::zlib ALIAS dftracer_zlib_shared)
        dftracer_utils_ok("Added dftracer_zlib shared library")
      endif()

      if(DFTRACER_UTILS_BUILD_STATIC)
        set_target_properties(
          dftracer_zlib_static
          PROPERTIES OUTPUT_NAME dftracer_zlib
                     LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                     ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
        add_library(dftracer::zlibstatic ALIAS dftracer_zlib_static)
        dftracer_utils_ok("Added dftracer_zlib static library")
        # If only static is built, make it the default alias
        if(NOT DFTRACER_UTILS_BUILD_SHARED)
          add_library(dftracer::zlib ALIAS dftracer_zlib_static)
        endif()
      endif()

      # Install our custom zlib targets
      if(ZLIB_TARGETS)
        install(
          TARGETS ${ZLIB_TARGETS}
          EXPORT ZlibTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
      endif()

      install(
        EXPORT ZlibTargets
        FILE ZlibTargets.cmake
        NAMESPACE dftracer::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlib)

      # Install zlib headers manually
      if(ZLIB_SOURCE_DIR AND ZLIB_BINARY_DIR)
        if(EXISTS "${ZLIB_SOURCE_DIR}/zlib.h")
          install(FILES "${ZLIB_SOURCE_DIR}/zlib.h"
                  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        endif()

        if(EXISTS "${ZLIB_BINARY_DIR}/zconf.h")
          install(FILES "${ZLIB_BINARY_DIR}/zconf.h"
                  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        endif()
      endif()

      # Make zlib available in parent scope for Arrow - let Arrow build its own
      if(DFTRACER_UTILS_BUILD_SHARED)
        set(ZLIB_LIBRARIES
            dftracer_zlib_shared
            PARENT_SCOPE)
      elseif(DFTRACER_UTILS_BUILD_STATIC)
        set(ZLIB_LIBRARIES
            dftracer_zlib_static
            PARENT_SCOPE)
      endif()
      set(ZLIB_INCLUDE_DIRS
          ${ZLIB_SOURCE_DIR} ${ZLIB_BINARY_DIR}
          PARENT_SCOPE)
      # Don't set ZLIB_FOUND to let Arrow build its own zlib
      set(ZLIB_FOUND
          FALSE
          PARENT_SCOPE)
    endif()
  endif()
endfunction()

function(link_zlib TARGET_NAME LIBRARY_TYPE)
  # Validate parameters
  if(NOT TARGET_NAME)
    message(FATAL_ERROR "link_zlib: TARGET_NAME is required")
  endif()

  if(NOT LIBRARY_TYPE MATCHES "^(STATIC|SHARED)$")
    message(
      FATAL_ERROR "link_zlib: LIBRARY_TYPE must be either STATIC or SHARED")
  endif()

  if(NOT TARGET ${TARGET_NAME})
    message(FATAL_ERROR "link_zlib: Target '${TARGET_NAME}' does not exist")
  endif()

  # Check if any zlib variant is available
  set(ZLIB_AVAILABLE FALSE)
  if(TARGET dftracer_zlib_static
     OR TARGET dftracer_zlib_shared
     OR ZLIB_FOUND)
    set(ZLIB_AVAILABLE TRUE)
  endif()

  if(NOT ZLIB_AVAILABLE)
    message(
      FATAL_ERROR
        "link_zlib: No zlib found! Call need_zlib() first or ensure system zlib is available."
    )
  endif()

  # Link appropriate zlib variant Use PUBLIC linkage since zlib headers may be
  # included in public headers
  if(LIBRARY_TYPE STREQUAL "STATIC")
    # For static libraries, prefer static zlib if available
    if(TARGET dftracer_zlib_static)
      target_link_libraries(${TARGET_NAME} PUBLIC dftracer::zlibstatic)
      dftracer_utils_ok("Linked ${TARGET_NAME} to dftracer zlibstatic")
    elseif(TARGET dftracer_zlib_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC dftracer::zlib)
      dftracer_utils_ok("Linked ${TARGET_NAME} to dftracer zlib (shared)")
    elseif(ZLIB_FOUND)
      target_link_libraries(${TARGET_NAME} PUBLIC ZLIB::ZLIB)
      dftracer_utils_ok("Linked ${TARGET_NAME} to system ZLIB::ZLIB")
    endif()
  else() # SHARED
    # For shared libraries, prefer shared zlib if available
    if(TARGET dftracer_zlib_shared)
      target_link_libraries(${TARGET_NAME} PUBLIC dftracer::zlib)
      dftracer_utils_ok("Linked ${TARGET_NAME} to dftracer zlib (shared)")
    elseif(TARGET dftracer_zlib_static)
      target_link_libraries(${TARGET_NAME} PUBLIC dftracer::zlibstatic)
      dftracer_utils_ok("Linked ${TARGET_NAME} to dftracer zlibstatic")
    elseif(ZLIB_FOUND)
      target_link_libraries(${TARGET_NAME} PUBLIC ZLIB::ZLIB)
      dftracer_utils_ok("Linked ${TARGET_NAME} to system ZLIB::ZLIB")
    endif()
  endif()
endfunction()

function(need_zstd)
  if(DFTRACER_UTILS_LOCAL_PACKAGES)
    find_package(zstd QUIET CONFIG)
    if(NOT zstd_FOUND)
      find_path(zstd_INCLUDE_DIRS NAMES zstd.h)
      find_library(zstd_LIBRARIES NAMES zstd)
      if(zstd_INCLUDE_DIRS AND zstd_LIBRARIES)
        set(zstd_FOUND TRUE)
      endif()
    endif()
  endif()

  if(DFTRACER_UTILS_LOCAL_PACKAGES AND zstd_FOUND)
    dftracer_utils_ok("Found system zstd")
    if(DEFINED zstd_LIBRARIES)
      # Provide the same target names as the CPM branch: zstd::libzstd_shared
      # for consumers (nanoarrow IPC) and zstd::zstd for RocksDB.
      foreach(_zstd_t zstd::libzstd_shared zstd::zstd)
        if(NOT TARGET ${_zstd_t})
          add_library(${_zstd_t} UNKNOWN IMPORTED)
          set_target_properties(
            ${_zstd_t}
            PROPERTIES IMPORTED_LOCATION "${zstd_LIBRARIES}"
                       INTERFACE_INCLUDE_DIRECTORIES "${zstd_INCLUDE_DIRS}")
        endif()
      endforeach()
    endif()
    set(zstd_FOUND
        TRUE
        PARENT_SCOPE)
    set(zstd_CPM
        FALSE
        PARENT_SCOPE)
  else()
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
        "ZSTD_BUILD_SHARED ${DFTRACER_UTILS_BUILD_SHARED}"
        "ZSTD_BUILD_STATIC ON")
    endif()

    if(zstd_ADDED)
      dftracer_utils_ok("Built zstd with CPM")

      set(_zstd_real)
      foreach(_zstd_t libzstd_shared libzstd_static)
        if(TARGET ${_zstd_t})
          set_target_properties(
            ${_zstd_t}
            PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
                       LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
                       RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
          if(DEFINED zstd_SOURCE_DIR)
            set_property(
              TARGET ${_zstd_t} APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
              "$<BUILD_INTERFACE:${zstd_SOURCE_DIR}/lib>")
          endif()
          install(
            TARGETS ${_zstd_t}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
          # zstd's build tree exposes only libzstd_shared / libzstd_static;
          # the zstd:: namespaced names exist only post-install. Other
          # consumers (nanoarrow IPC) link the namespaced names, so alias them.
          if(NOT TARGET zstd::${_zstd_t})
            add_library(zstd::${_zstd_t} ALIAS ${_zstd_t})
          endif()
          if(NOT _zstd_real)
            set(_zstd_real ${_zstd_t})
          endif()
        endif()
      endforeach()

      if(_zstd_real)
        if(_zstd_real STREQUAL libzstd_shared)
          set(_zstd_output
              "${CMAKE_BINARY_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}zstd${CMAKE_SHARED_LIBRARY_SUFFIX}"
          )
        else()
          set(_zstd_output
              "${CMAKE_BINARY_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}zstd${CMAKE_STATIC_LIBRARY_SUFFIX}"
          )
        endif()

        if(NOT TARGET zstd::zstd)
          add_library(zstd::zstd ALIAS ${_zstd_real})
        endif()

        if(DEFINED zstd_SOURCE_DIR)
          set(ZSTD_INCLUDE_DIRS
              "${zstd_SOURCE_DIR}/lib"
              CACHE PATH "zstd include directory (CPM)" FORCE)
          install(FILES "${zstd_SOURCE_DIR}/lib/zstd.h"
                  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        endif()
        set(ZSTD_LIBRARIES
            "${_zstd_output}"
            CACHE FILEPATH "zstd library (CPM)" FORCE)
      endif()

      set(zstd_FOUND
          TRUE
          PARENT_SCOPE)
      set(zstd_CPM
          TRUE
          PARENT_SCOPE)
    endif()
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
      install(
        FILES ${readerwriterqueue_SOURCE_DIR}/readerwriterqueue.h
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
      c68072129c8a5b4025122ca5a0c82ab14b30cb03
      DOWNLOAD_ONLY
      YES)

    if(concurrentqueue_ADDED)
      add_library(concurrentqueue INTERFACE)
      target_include_directories(
        concurrentqueue INTERFACE $<BUILD_INTERFACE:${concurrentqueue_SOURCE_DIR}>
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
    configure_file(
      ${nanoarrow_SOURCE_DIR}/src/nanoarrow/nanoarrow_config.h.in
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
        if(DFTRACER_UTILS_ENABLE_ZSTD)
          target_compile_definitions(nanoarrow_static
                                     PRIVATE NANOARROW_IPC_WITH_ZSTD)
          if(TARGET zstd::libzstd_static)
            target_link_libraries(nanoarrow_static PRIVATE zstd::libzstd_static)
          elseif(TARGET zstd::libzstd_shared)
            target_link_libraries(nanoarrow_static PRIVATE zstd::libzstd_shared)
          endif()
        endif()
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
        if(DFTRACER_UTILS_ENABLE_ZSTD)
          target_compile_definitions(nanoarrow_shared
                                     PRIVATE NANOARROW_IPC_WITH_ZSTD)
          if(TARGET zstd::libzstd_shared)
            target_link_libraries(nanoarrow_shared PRIVATE zstd::libzstd_shared)
          elseif(TARGET zstd::libzstd_static)
            target_link_libraries(nanoarrow_shared PRIVATE zstd::libzstd_static)
          endif()
        endif()
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

    # Suppress warnings from nanoarrow headers (redundant redeclarations,
    # shadow warnings in nanoarrow 0.8.0 internal headers)
    foreach(_na_target ${NANOARROW_TARGETS})
      get_target_property(_na_inc ${_na_target} INTERFACE_INCLUDE_DIRECTORIES)
      if(_na_inc)
        set_target_properties(${_na_target} PROPERTIES
          INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_na_inc}")
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
      FATAL_ERROR
        "link_nanoarrow: LIBRARY_TYPE must be either STATIC or SHARED")
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

  # CPMAddPackage only sets boost_math_SOURCE_DIR in the calling scope. Cache
  # it so link_boost_math() can find the include dir from anywhere in the tree.
  if(boost_math_SOURCE_DIR)
    set(boost_math_SOURCE_DIR
        "${boost_math_SOURCE_DIR}"
        CACHE INTERNAL "Boost.Math source tree from CPM")
    dftracer_utils_ok("Added Boost.Math (standalone) headers from ${boost_math_SOURCE_DIR}/include")
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
    message(FATAL_ERROR "link_boost_math: target '${TARGET_NAME}' does not exist")
  endif()
  if(NOT boost_math_SOURCE_DIR)
    message(FATAL_ERROR
      "link_boost_math: boost_math_SOURCE_DIR is unset; call need_boost_math() first")
  endif()

  target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
                             ${boost_math_SOURCE_DIR}/include)
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
    message(FATAL_ERROR
      "link_yaml_cpp: yaml-cpp::yaml-cpp target missing; call need_yaml_cpp() first")
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

macro(check_std_filesystem)
  # Probe once and cache; try_compile otherwise re-runs a compiler every
  # reconfigure. The status message below still prints each configure.
  if(NOT DEFINED DFTRACER_UTILS_HAS_STD_FILESYSTEM)
    try_compile(
      _dftracer_has_std_filesystem "${CMAKE_BINARY_DIR}/temp"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/has_filesystem.cpp"
      CXX_STANDARD ${CMAKE_CXX_STANDARD}
      CXX_STANDARD_REQUIRED ON
      LINK_LIBRARIES stdc++fs)
    set(DFTRACER_UTILS_HAS_STD_FILESYSTEM ${_dftracer_has_std_filesystem}
        CACHE INTERNAL "Compiler provides a usable std::filesystem")
  endif()
  if(DFTRACER_UTILS_HAS_STD_FILESYSTEM)
    dftracer_utils_ok("Compiler has std::filesystem support")
  else()
    dftracer_utils_warn(
      "Compiler does not have std::filesystem support. Use gulrak::filesystem"
    )
  endif(DFTRACER_UTILS_HAS_STD_FILESYSTEM)
endmacro()

function(add_stdfs_if_needed TARGET)
  if(DFTRACER_UTILS_HAS_STD_FILESYSTEM)
    target_link_libraries(${TARGET} PRIVATE stdc++fs)
  endif()
endfunction()

# Report which ObjectPool atomics path object_pool.h auto-selects under the
# active -march (DWCAS if the 16-byte CAS is lock-free, else the packed 64-bit
# CAS). We add no ISA flags on purpose: forcing -mcx16/+lse would raise the
# binary's CPU floor and could SIGILL on older hardware. Both paths are
# lock-free and correct everywhere; the fast path turns on automatically when
# the toolchain already targets a capable baseline (Apple Silicon, -march=native).
macro(check_dwcas)
  if(NOT DEFINED DFTRACER_UTILS_HAS_DWCAS)
    try_compile(
      _dftracer_has_dwcas "${CMAKE_BINARY_DIR}/temp"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/has_dwcas.cpp")
    set(DFTRACER_UTILS_HAS_DWCAS ${_dftracer_has_dwcas}
        CACHE INTERNAL "ObjectPool: 16-byte CAS is lock-free under active flags")
  endif()

  dftracer_utils_section("ObjectPool atomics")
  if(DFTRACER_UTILS_HAS_DWCAS)
    dftracer_utils_ok("DWCAS fast path (lock-free 16-byte CAS)")
  else()
    dftracer_utils_ok("Packed fallback (lock-free 64-bit CAS, portable)")
  endif()

  # libatomic safety net for targets that can't inline the atomic (e.g. 32-bit;
  # every 64-bit target we ship is inline lock-free and does not need it). Only
  # apply the directory-scope link when dftracer-utils owns the build, never
  # when it is consumed via add_subdirectory, so the link can't leak into a
  # parent project's targets.
  find_library(DFTRACER_UTILS_LIBATOMIC atomic)
  if(DFTRACER_UTILS_LIBATOMIC AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    link_libraries(${DFTRACER_UTILS_LIBATOMIC})
  endif()
endmacro()
