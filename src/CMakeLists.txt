# ##############################################################################
# Dependencies
# ##############################################################################

add_rpath()

need_zlib()
need_sqlite3()
need_argparse()
need_ghc_filesystem()
need_xxhash()
need_cpplogger()
need_yyjson()
need_nonstd_span()

# ##############################################################################
# Libraries
# ##############################################################################

# -----------------------------------------
# Core library sources
# -----------------------------------------

set(DFTRACER_UTILS_CORE_SOURCES
    # Common
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/common/constants.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/common/format_detector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/common/filesystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/common/span.cpp
    # Utilities
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/utils/timer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/utils/string.cpp
    # Pipeline - New Architecture
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/error.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/task_queue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/executor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/watchdog.cpp
    # Tasks
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/tasks/task.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/tasks/task_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/tasks/task_future.cpp
)

# -----------------------------------------
# Utilities library sources
# -----------------------------------------

set(DFTRACER_UTILS_UTILITIES_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/composites/dft/metadata_collector_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/composites/dft/chunk_extractor_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/composites/dft/chunk_manifest_mapper_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/composites/dft/event_id_extractor_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/composites/dft/event_collector_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/composites/file_merger_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/composites/dft/event_hasher_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/composites/dft/internal/utils.cpp

    # Indexer
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/indexer_c.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/checkpoint_size.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/error.cpp
    # Indexer factory
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/indexer_factory.cpp
    # GZIP indexer
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/delete_file_record.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/insert_checkpoint_record.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/insert_file_metadata_record.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/insert_file_record.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/query_checkpoint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/query_checkpoints.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/query_file_id.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/query_max_bytes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/query_num_lines.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/query_schema_validity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/query_stored_file_info.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/queries/query_checkpoint_size.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/constants.cpp
    # TAR indexer
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/tar_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/tar_indexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/constants.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/queries/insert_file_record.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/queries/insert_archive_record.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/queries/insert_archive_metadata_record.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/queries/insert_tar_file_record.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/queries/insert_tar_checkpoint_record.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/queries/query_archive_id.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/queries/query_tar_files.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/queries/query_tar_checkpoints.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/tar/queries/query_metadata.cpp
    # Reader
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/reader_c.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/stream_c.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/gzip_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/tar_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/reader_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/error.cpp
)

# Determine which dependencies to require based on what was found vs built with
# CPM
set(PKG_CONFIG_REQUIRES "")
set(PKG_CONFIG_LIBS_PRIVATE "")

# Add Threads (always required)
set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -lpthread")

# Only add sqlite3 to pkg-config requirements if it was found on system
if(SQLite3_FOUND AND NOT SQLite3_CPM)
  set(PKG_CONFIG_REQUIRES "${PKG_CONFIG_REQUIRES} sqlite3")
  set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -lsqlite3")
else()
  set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -lsqlite3")
endif()

# Only add zlib to pkg-config requirements if it was found on system
if(ZLIB_FOUND AND NOT ZLIB_CPM)
  set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -lz")
else()
  set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -ldftracer_zlib")
endif()

# Only add cpp-logger to pkg-config requirements if it was found on system
if(cpp-logger_FOUND AND NOT cpplogger_CPM)
  set(PKG_CONFIG_REQUIRES "${PKG_CONFIG_REQUIRES} cpp-logger")
  set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -lcpp-logger")
else()
  set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -lcpp-logger")
endif()

# Add other dependencies built with CPM
set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -lyyjson -lxxhash")

# +++++++++++++++++++++++++++++++++++++++++
# Core Library
# +++++++++++++++++++++++++++++++++++++++++

# Collect all dependency libraries
if(DFTRACER_UTILS_BUILD_SHARED OR DFTRACER_UTILS_BUILD_STATIC)
  # noop
endif()

# Compile options for coverage
set(COVERAGE_COMPILE_OPTIONS "")
set(COVERAGE_LINK_OPTIONS "")
if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(COVERAGE_COMPILE_OPTIONS --coverage -fprofile-arcs -ftest-coverage)
  set(COVERAGE_LINK_OPTIONS --coverage)
endif()

# +++++++++++++++++++++++++++++++++++++++++
# Configure Precompiled Headers
# +++++++++++++++++++++++++++++++++++++++++

# Set global PCH configuration (used by use_precompiled_header function)
if(DFTRACER_UTILS_ENABLE_PCH)
  set(DFTRACER_UTILS_PCH_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  set(DFTRACER_UTILS_PCH_MIN_COUNT 2)
endif()

create_library(
  NAME
    dftracer_utils_core
  NAMESPACE
    dftracer_utils_core
  SOURCES
    ${DFTRACER_UTILS_CORE_SOURCES}
  VERSION
    ${PROJECT_VERSION}
  DESCRIPTION
    "DFTracer utils core library (indexer, reader, pipeline)"
  URL
    "https://github.com/LLNL/dftracer-utils"
  BUILD_SHARED
    ${DFTRACER_UTILS_BUILD_SHARED}
  BUILD_STATIC
    ${DFTRACER_UTILS_BUILD_STATIC}
  PUBLIC_LINK_LIBRARIES
    ghc_filesystem
    nonstd::span-lite
    Threads::Threads
  PUBLIC_INCLUDE_DIRS
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE_COMPILE_OPTIONS
    ${COVERAGE_COMPILE_OPTIONS}
  PKG_CONFIG_REQUIRES
    "${PKG_CONFIG_REQUIRES}"
  PKG_CONFIG_LIBS_PRIVATE
    "${PKG_CONFIG_LIBS_PRIVATE}")

# Post-process core library targets to add dependency linking and warnings
foreach(variant shared static)
  string(TOUPPER ${variant} VARIANT_UPPER)
  if(TARGET dftracer_utils_core_${variant})
    # Link dependencies using helper functions
    link_cpp_logger(dftracer_utils_core_${variant} ${VARIANT_UPPER})
    link_yyjson(dftracer_utils_core_${variant} ${VARIANT_UPPER})
    link_xxhash(dftracer_utils_core_${variant} ${VARIANT_UPPER})
    link_sqlite3(dftracer_utils_core_${variant} ${VARIANT_UPPER})
    link_zlib(dftracer_utils_core_${variant} ${VARIANT_UPPER})

    # Add stdfs if needed
    add_stdfs_if_needed(dftracer_utils_core_${variant})

    # Set warnings
    target_set_warnings(dftracer_utils_core_${variant})

    # Apply precompiled headers (automatically handles detection and reuse)
    if(DFTRACER_UTILS_ENABLE_PCH)
      use_precompiled_header(dftracer_utils_core_${variant})
    endif()

    # Link coverage if needed
    if(COVERAGE_LINK_OPTIONS)
      target_link_libraries(dftracer_utils_core_${variant}
                            PRIVATE ${COVERAGE_LINK_OPTIONS})
    endif()
  endif()
endforeach()

# +++++++++++++++++++++++++++++++++++++++++
# Utilities Library
# +++++++++++++++++++++++++++++++++++++++++

create_library(
  NAME
    dftracer_utils_utilities
  NAMESPACE
    dftracer_utils_utilities
  SOURCES
    ${DFTRACER_UTILS_UTILITIES_SOURCES}
  VERSION
    ${PROJECT_VERSION}
  DESCRIPTION
    "DFTracer utils utilities library (hash, io, compression, composites)"
  URL
    "https://github.com/LLNL/dftracer-utils"
  BUILD_SHARED
    ${DFTRACER_UTILS_BUILD_SHARED}
  BUILD_STATIC
    ${DFTRACER_UTILS_BUILD_STATIC}
  SHARED_LINK_LIBRARIES
    dftracer_utils_core::shared
  STATIC_LINK_LIBRARIES
    dftracer_utils_core::static
  PUBLIC_INCLUDE_DIRS
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE_COMPILE_OPTIONS
    ${COVERAGE_COMPILE_OPTIONS}
  PKG_CONFIG_REQUIRES
    "dftracer_utils_core"
  FIND_DEPENDENCIES
    dftracer_utils_core)

# Post-process utilities library targets
foreach(variant shared static)
  if(TARGET dftracer_utils_utilities_${variant})
    # No need to link dependencies here - they're inherited from core

    # Set warnings
    target_set_warnings(dftracer_utils_utilities_${variant})

    # Apply precompiled headers (automatically reuses from core library)
    if(DFTRACER_UTILS_ENABLE_PCH)
      use_precompiled_header(dftracer_utils_utilities_${variant})
    endif()

    # Link coverage if needed
    if(COVERAGE_LINK_OPTIONS)
      target_link_libraries(dftracer_utils_utilities_${variant}
                            PRIVATE ${COVERAGE_LINK_OPTIONS})
    endif()
  endif()
endforeach()

# Create unified configs for core and utilities libraries This allows discovery
# via "dftracer_utils_core" instead of "dftracer_utils_core_shared"
create_unified_library_config(
  NAME
    dftracer_utils_core
  VERSION
    ${PROJECT_VERSION}
  DESCRIPTION
    "DFTracer utils core library (indexer, reader, pipeline)"
  URL
    "https://github.com/LLNL/dftracer-utils"
  BUILD_SHARED
    ${DFTRACER_UTILS_BUILD_SHARED}
  BUILD_STATIC
    ${DFTRACER_UTILS_BUILD_STATIC})

create_unified_library_config(
  NAME
    dftracer_utils_utilities
  VERSION
    ${PROJECT_VERSION}
  DESCRIPTION
    "DFTracer utils utilities library (hash, io, compression, composites)"
  URL
    "https://github.com/LLNL/dftracer-utils"
  PKG_CONFIG_REQUIRES
    "dftracer_utils_core"
  FIND_DEPENDENCIES
    dftracer_utils_core
  BUILD_SHARED
    ${DFTRACER_UTILS_BUILD_SHARED}
  BUILD_STATIC
    ${DFTRACER_UTILS_BUILD_STATIC})

# +++++++++++++++++++++++++++++++++++++++++
# Unified Interface Library
# +++++++++++++++++++++++++++++++++++++++++

# Create interface libraries that link both core and utilities
if(DFTRACER_UTILS_BUILD_SHARED)
  add_library(dftracer_utils_shared INTERFACE)
  target_link_libraries(
    dftracer_utils_shared INTERFACE dftracer_utils_core::shared
                                    dftracer_utils_utilities::shared)

  # Create aliases
  add_library(dftracer_utils::shared ALIAS dftracer_utils_shared)
  add_library(dftracer_utils ALIAS dftracer_utils_shared)
  add_library(dftracer::utils ALIAS dftracer_utils_shared)
  add_library(dftracer_utils::dftracer_utils ALIAS dftracer_utils_shared)

  # Export the interface library
  install(TARGETS dftracer_utils_shared EXPORT dftracer_utilsTargets)
endif()

if(DFTRACER_UTILS_BUILD_STATIC)
  add_library(dftracer_utils_static INTERFACE)
  target_link_libraries(
    dftracer_utils_static INTERFACE dftracer_utils_core::static
                                    dftracer_utils_utilities::static)

  # Create aliases
  add_library(dftracer_utils::static ALIAS dftracer_utils_static)

  # If shared is not built, create default aliases
  if(NOT DFTRACER_UTILS_BUILD_SHARED)
    add_library(dftracer_utils ALIAS dftracer_utils_static)
    add_library(dftracer::utils ALIAS dftracer_utils_static)
    add_library(dftracer_utils::dftracer_utils ALIAS dftracer_utils_static)
  endif()

  # Export the interface library
  install(TARGETS dftracer_utils_static EXPORT dftracer_utilsTargets)
endif()

# Create unified package config for dftracer_utils
if(DFTRACER_UTILS_BUILD_SHARED)
  set(DFTRACER_UTILS_PREFERRED_TYPE "shared")
elseif(DFTRACER_UTILS_BUILD_STATIC)
  set(DFTRACER_UTILS_PREFERRED_TYPE "static")
endif()

if(DFTRACER_UTILS_PREFERRED_TYPE)
  # Create the unified CMake Config file programmatically
  set(UNIFIED_CONFIG_CONTENT "")
  string(APPEND UNIFIED_CONFIG_CONTENT "@PACKAGE_INIT@\n\n")
  string(APPEND UNIFIED_CONFIG_CONTENT "include(CMakeFindDependencyMacro)\n\n")
  string(APPEND UNIFIED_CONFIG_CONTENT "# Find core and utilities libraries\n")
  string(APPEND UNIFIED_CONFIG_CONTENT "find_dependency(dftracer_utils_core)\n")
  string(APPEND UNIFIED_CONFIG_CONTENT
         "find_dependency(dftracer_utils_utilities)\n\n")
  string(APPEND UNIFIED_CONFIG_CONTENT "# Include the unified targets file\n")
  string(
    APPEND UNIFIED_CONFIG_CONTENT
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/dftracer_utilsTargets.cmake\")\n\n")

  # Add convenient aliases
  string(APPEND UNIFIED_CONFIG_CONTENT "# Create convenient aliases\n")
  if(DFTRACER_UTILS_BUILD_SHARED)
    string(APPEND UNIFIED_CONFIG_CONTENT
           "if(TARGET dftracer_utils::dftracer_utils_shared)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "  # dftracer_utils::shared\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET dftracer_utils::shared)\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(dftracer_utils::shared ALIAS dftracer_utils::dftracer_utils_shared)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  # dftracer_utils_shared (without namespace)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET dftracer_utils_shared)\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(dftracer_utils_shared ALIAS dftracer_utils::dftracer_utils_shared)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "endif()\n\n")
  endif()

  if(DFTRACER_UTILS_BUILD_STATIC)
    string(APPEND UNIFIED_CONFIG_CONTENT
           "if(TARGET dftracer_utils::dftracer_utils_static)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "  # dftracer_utils::static\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET dftracer_utils::static)\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(dftracer_utils::static ALIAS dftracer_utils::dftracer_utils_static)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  # dftracer_utils_static (without namespace)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET dftracer_utils_static)\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(dftracer_utils_static ALIAS dftracer_utils::dftracer_utils_static)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "endif()\n\n")
  endif()

  string(APPEND UNIFIED_CONFIG_CONTENT
         "check_required_components(dftracer_utils)\n")

  # Write the config template
  set(UNIFIED_CONFIG_TEMPLATE
      "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utilsConfig.cmake.in")
  file(WRITE ${UNIFIED_CONFIG_TEMPLATE} ${UNIFIED_CONFIG_CONTENT})

  # Configure the config file
  include(CMakePackageConfigHelpers)
  set(UNIFIED_CONFIG_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utilsConfig.cmake")
  configure_package_config_file(
    ${UNIFIED_CONFIG_TEMPLATE} ${UNIFIED_CONFIG_FILE}
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils)

  # Create version file
  set(UNIFIED_VERSION_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utilsConfigVersion.cmake")
  write_basic_package_version_file(
    ${UNIFIED_VERSION_FILE}
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

  # Install config files
  install(FILES ${UNIFIED_CONFIG_FILE} ${UNIFIED_VERSION_FILE}
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils)

  # Install export targets
  install(
    EXPORT dftracer_utilsTargets
    FILE dftracer_utilsTargets.cmake
    NAMESPACE dftracer_utils::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils)

  # Create a unified pkg-config file
  create_pkgconfig_file(
    TARGET
    dftracer_utils
    NAME
    dftracer_utils
    VERSION
    ${PROJECT_VERSION}
    DESCRIPTION
    "DFTracer utils library (core + utilities)"
    URL
    "https://github.com/LLNL/dftracer-utils"
    REQUIRES
    "dftracer_utils_core dftracer_utils_utilities")

  # ==========================================================================
  # Create variant-specific wrapper configs for dftracer_utils_static and
  # dftracer_utils_shared These allow discovery via
  # "find_package(dftracer_utils_static)" and
  # "find_package(dftracer_utils_shared)"
  # ==========================================================================

  # dftracer_utils_shared wrapper config
  if(DFTRACER_UTILS_BUILD_SHARED)
    set(VARIANT_CONFIG_CONTENT "")
    string(APPEND VARIANT_CONFIG_CONTENT "@PACKAGE_INIT@\n\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "# dftracer_utils_shared wrapper config\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "# This simply includes the main dftracer_utils config\n\n")
    string(
      APPEND
      VARIANT_CONFIG_CONTENT
      "include(\${CMAKE_CURRENT_LIST_DIR}/../dftracer_utils/dftracer_utilsConfig.cmake)\n\n"
    )
    string(APPEND VARIANT_CONFIG_CONTENT "# Ensure the target is available\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "if(NOT TARGET dftracer_utils_shared)\n")
    string(
      APPEND
      VARIANT_CONFIG_CONTENT
      "  message(FATAL_ERROR \"dftracer_utils_shared target not found. Shared library may not have been built.\")\n"
    )
    string(APPEND VARIANT_CONFIG_CONTENT "endif()\n\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "check_required_components(dftracer_utils_shared)\n")

    set(VARIANT_CONFIG_TEMPLATE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_sharedConfig.cmake.in")
    file(WRITE ${VARIANT_CONFIG_TEMPLATE} ${VARIANT_CONFIG_CONTENT})

    set(VARIANT_CONFIG_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_sharedConfig.cmake")
    configure_package_config_file(
      ${VARIANT_CONFIG_TEMPLATE} ${VARIANT_CONFIG_FILE}
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils_shared)

    set(VARIANT_VERSION_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_sharedConfigVersion.cmake")
    write_basic_package_version_file(
      ${VARIANT_VERSION_FILE}
      VERSION ${PROJECT_VERSION}
      COMPATIBILITY SameMajorVersion)

    install(FILES ${VARIANT_CONFIG_FILE} ${VARIANT_VERSION_FILE}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils_shared)
  endif()

  # dftracer_utils_static wrapper config
  if(DFTRACER_UTILS_BUILD_STATIC)
    set(VARIANT_CONFIG_CONTENT "")
    string(APPEND VARIANT_CONFIG_CONTENT "@PACKAGE_INIT@\n\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "# dftracer_utils_static wrapper config\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "# This simply includes the main dftracer_utils config\n\n")
    string(
      APPEND
      VARIANT_CONFIG_CONTENT
      "include(\${CMAKE_CURRENT_LIST_DIR}/../dftracer_utils/dftracer_utilsConfig.cmake)\n\n"
    )
    string(APPEND VARIANT_CONFIG_CONTENT "# Ensure the target is available\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "if(NOT TARGET dftracer_utils_static)\n")
    string(
      APPEND
      VARIANT_CONFIG_CONTENT
      "  message(FATAL_ERROR \"dftracer_utils_static target not found. Static library may not have been built.\")\n"
    )
    string(APPEND VARIANT_CONFIG_CONTENT "endif()\n\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "check_required_components(dftracer_utils_static)\n")

    set(VARIANT_CONFIG_TEMPLATE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_staticConfig.cmake.in")
    file(WRITE ${VARIANT_CONFIG_TEMPLATE} ${VARIANT_CONFIG_CONTENT})

    set(VARIANT_CONFIG_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_staticConfig.cmake")
    configure_package_config_file(
      ${VARIANT_CONFIG_TEMPLATE} ${VARIANT_CONFIG_FILE}
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils_static)

    set(VARIANT_VERSION_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_staticConfigVersion.cmake")
    write_basic_package_version_file(
      ${VARIANT_VERSION_FILE}
      VERSION ${PROJECT_VERSION}
      COMPATIBILITY SameMajorVersion)

    install(FILES ${VARIANT_CONFIG_FILE} ${VARIANT_VERSION_FILE}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils_static)
  endif()
endif()

# Install headers
install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../include/dftracer/utils/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dftracer/utils
  FILES_MATCHING
  PATTERN "*.h")

install(
  DIRECTORY ${CMAKE_BINARY_DIR}/include/dftracer/utils/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dftracer/utils
  FILES_MATCHING
  PATTERN "*.h")

# ##############################################################################
# Executables
# ##############################################################################

if(DFTRACER_UTILS_BUILD_BINARIES)
  set(DFTRACER_BINARIES
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_reader.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_tar.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_event_count.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_pgzip.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_merge.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_split.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_info.cpp)

  foreach(bin ${DFTRACER_BINARIES})
    string(REPLACE ".cpp" "" bin_exec ${bin})
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/" ""
                   bin_exec ${bin_exec})

    add_executable(${bin_exec} ${bin})
    set_target_properties(
      ${bin_exec} PROPERTIES OUTPUT_NAME "${bin_exec}" RUNTIME_OUTPUT_DIRECTORY
                                                       ${CMAKE_BINARY_DIR}/bin)

    # Link to unified library (which includes both core and utilities)
    target_link_libraries(${bin_exec} PRIVATE dftracer_utils argparse::argparse)

    target_include_directories(${bin_exec}
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

    target_set_warnings(${bin_exec})

    # Apply coverage flags if enabled
    if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${bin_exec} PRIVATE --coverage -fprofile-arcs
                                                 -ftest-coverage)
      target_link_libraries(${bin_exec} PRIVATE --coverage)
    endif()

    # Install executable
    install(TARGETS ${bin_exec} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Create Python wrapper for venv/bin that calls the binary in site-packages
    if(SKBUILD)
      create_python_wrapper(${bin_exec})
    endif()
  endforeach()
endif()

# ##############################################################################
# Python binding
# ##############################################################################

if(CMAKE_VERSION VERSION_LESS 3.18)
  set(DEV_MODULE Development)
else()
  set(DEV_MODULE Development.Module)
endif()

find_package(
  Python 3.8
  COMPONENTS Interpreter ${DEV_MODULE}
  OPTIONAL_COMPONENTS Development.SABIModule)

if(DFTRACER_UTILS_BUILD_PYTHON)
  if(NOT Python_FOUND)
    message(
      FATAL_ERROR "Python was not found, but DFTRACER_UTILS_BUILD_PYTHON is ON")
  endif()

  if(NOT DFTRACER_UTILS_BUILD_SHARED)
    message(
      FATAL_ERROR
        "Python bindings require shared library build (DFTRACER_UTILS_BUILD_SHARED must be ON)"
    )
  endif()

  message(STATUS "Building Python bindings")

  # Python C API binding sources
  python_add_library(
    dftracer_utils_ext
    MODULE
    WITH_SOABI
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/dftracer_utils_ext.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/indexer.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/indexer.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/indexer_checkpoint.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/indexer_checkpoint.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/reader.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/reader.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/json.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/json.cpp)

  # Link to unified library to test if this fixes the bus error
  target_link_libraries(dftracer_utils_ext PRIVATE dftracer_utils::shared)

  set_target_properties(
    dftracer_utils_ext PROPERTIES LIBRARY_OUTPUT_DIRECTORY
                                  "${CMAKE_BINARY_DIR}/dftracer/utils")

  install(TARGETS dftracer_utils_ext LIBRARY DESTINATION "dftracer/utils")

endif()
