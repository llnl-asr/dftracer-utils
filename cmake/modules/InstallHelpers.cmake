# InstallHelpers.cmake - Utilities for creating pkg-config and CMake package
# configs

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

#[=[
Creates pkg-config (.pc) and CMake package configuration files for a target.

Usage:
  create_package_config(
    TARGET target_name
    [VERSION version_string]
    [DESCRIPTION "Package description"]
    [URL "https://example.com"]
    [REQUIRES "dep1 dep2"]
    [LIBS_PRIVATE "private_libs"]
    [CFLAGS_PRIVATE "private_cflags"]
  )

Arguments:
  TARGET - The target name (used for package name)
  VERSION - Package version (defaults to PROJECT_VERSION)
  DESCRIPTION - Package description
  URL - Package URL
  REQUIRES - Public dependencies for pkg-config
  LIBS_PRIVATE - Private libraries for pkg-config
  CFLAGS_PRIVATE - Private compile flags for pkg-config
#]=]
function(create_package_config)
  set(options "")
  set(oneValueArgs
      TARGET
      VERSION
      DESCRIPTION
      URL
      REQUIRES
      LIBS_PRIVATE
      CFLAGS_PRIVATE)
  set(multiValueArgs "")

  cmake_parse_arguments(PKG "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  if(NOT PKG_TARGET)
    message(FATAL_ERROR "TARGET is required")
  endif()

  if(NOT PKG_VERSION)
    set(PKG_VERSION ${PROJECT_VERSION})
  endif()

  if(NOT PKG_DESCRIPTION)
    set(PKG_DESCRIPTION "${PKG_TARGET} library")
  endif()

  get_target_property(TARGET_TYPE ${PKG_TARGET} TYPE)

  create_pkgconfig_file(
    TARGET
    ${PKG_TARGET}
    VERSION
    ${PKG_VERSION}
    DESCRIPTION
    ${PKG_DESCRIPTION}
    URL
    ${PKG_URL}
    REQUIRES
    ${PKG_REQUIRES}
    LIBS_PRIVATE
    ${PKG_LIBS_PRIVATE}
    CFLAGS_PRIVATE
    ${PKG_CFLAGS_PRIVATE})

  create_cmake_config_files(TARGET ${PKG_TARGET} VERSION ${PKG_VERSION})
endfunction()

#[=[
Internal function to create pkg-config file
#]=]
function(create_pkgconfig_file)
  set(options "")
  set(oneValueArgs
      TARGET
      NAME
      VERSION
      DESCRIPTION
      URL
      REQUIRES
      LIBS_PRIVATE
      CFLAGS_PRIVATE)
  set(multiValueArgs "")

  cmake_parse_arguments(PKG "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  # Use NAME if provided, otherwise use TARGET for both name and library
  if(NOT PKG_NAME)
    set(PKG_NAME ${PKG_TARGET})
  endif()

  # Generate pkg-config file content
  set(PC_CONTENT "")
  string(APPEND PC_CONTENT "prefix=${CMAKE_INSTALL_PREFIX}\n")
  string(APPEND PC_CONTENT "exec_prefix=\${prefix}\n")
  string(APPEND PC_CONTENT "libdir=\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}\n")
  string(APPEND PC_CONTENT
         "includedir=\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}\n")
  string(APPEND PC_CONTENT "\n")
  string(APPEND PC_CONTENT "Name: ${PKG_NAME}\n")
  string(APPEND PC_CONTENT "Description: ${PKG_DESCRIPTION}\n")
  string(APPEND PC_CONTENT "Version: ${PKG_VERSION}\n")

  if(PKG_URL)
    string(APPEND PC_CONTENT "URL: ${PKG_URL}\n")
  endif()

  if(PKG_REQUIRES)
    string(APPEND PC_CONTENT "Requires: ${PKG_REQUIRES}\n")
  endif()

  if(PKG_LIBS_PRIVATE)
    string(APPEND PC_CONTENT "Libs.private: ${PKG_LIBS_PRIVATE}\n")
  endif()

  if(PKG_CFLAGS_PRIVATE)
    string(APPEND PC_CONTENT "Cflags.private: ${PKG_CFLAGS_PRIVATE}\n")
  endif()

  string(APPEND PC_CONTENT "Libs: -L\${libdir} -l${PKG_TARGET}\n")
  string(APPEND PC_CONTENT "Cflags: -I\${includedir}\n")

  # Write pkg-config file using NAME for the filename
  set(PC_FILE "${CMAKE_CURRENT_BINARY_DIR}/${PKG_NAME}.pc")
  file(WRITE ${PC_FILE} ${PC_CONTENT})

  # Install pkg-config file
  install(FILES ${PC_FILE} DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endfunction()

#[=[
Internal function to create CMake package config files
#]=]
function(create_cmake_config_files)
  set(options "")
  set(oneValueArgs TARGET VERSION)
  set(multiValueArgs "")

  cmake_parse_arguments(PKG "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  # Create the config file template
  set(CONFIG_TEMPLATE
      "${CMAKE_CURRENT_BINARY_DIR}/${PKG_TARGET}Config.cmake.in")
  file(
    WRITE ${CONFIG_TEMPLATE}
    "
@PACKAGE_INIT@

include(CMakeFindDependencyMacro)

# Find dependencies - handle both CPM-built and system packages

# ZLIB dependency
find_library(ZLIB_LIBRARY_BUNDLED
    NAMES dftracer_zlib libdftracer_zlib
    PATHS \${_IMPORT_PREFIX}/lib
    NO_DEFAULT_PATH
)

if(ZLIB_LIBRARY_BUNDLED)
    # Found zlib that was built with this package
    find_path(ZLIB_INCLUDE_DIR_BUNDLED
        NAMES zlib.h
        PATHS \${_IMPORT_PREFIX}/include
        NO_DEFAULT_PATH
    )

    if(ZLIB_INCLUDE_DIR_BUNDLED AND NOT TARGET dftracer::zlib)
        add_library(dftracer::zlib UNKNOWN IMPORTED)
        set_target_properties(dftracer::zlib PROPERTIES
            IMPORTED_LOCATION \"\${ZLIB_LIBRARY_BUNDLED}\"
            INTERFACE_INCLUDE_DIRECTORIES \"\${ZLIB_INCLUDE_DIR_BUNDLED}\"
        )
    endif()

    # Also create ZLIB::ZLIB alias for compatibility
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS dftracer::zlib)
    endif()
else()
    # Fall back to system zlib (require minimum version 1.2)
    find_dependency(ZLIB 1.2 REQUIRED)
    if(NOT TARGET dftracer::zlib)
        add_library(dftracer::zlib ALIAS ZLIB::ZLIB)
    endif()
endif()

# SQLITE3 dependency
find_library(SQLITE3_LIBRARY_BUNDLED
    NAMES sqlite3 libsqlite3
    PATHS \${_IMPORT_PREFIX}/lib
    NO_DEFAULT_PATH
)

if(SQLITE3_LIBRARY_BUNDLED)
    # Found sqlite3 that was built with this package
    find_path(SQLITE3_INCLUDE_DIR_BUNDLED
        NAMES sqlite3.h
        PATHS \${_IMPORT_PREFIX}/include
        NO_DEFAULT_PATH
    )

    if(SQLITE3_INCLUDE_DIR_BUNDLED AND NOT TARGET SQLite::SQLite3)
        add_library(SQLite::SQLite3 UNKNOWN IMPORTED)
        set_target_properties(SQLite::SQLite3 PROPERTIES
            IMPORTED_LOCATION \"\${SQLITE3_LIBRARY_BUNDLED}\"
            INTERFACE_INCLUDE_DIRECTORIES \"\${SQLITE3_INCLUDE_DIR_BUNDLED}\"
        )
    endif()
else()
    # Fall back to system sqlite3 via pkg-config (require minimum version 3.35)
    find_dependency(PkgConfig REQUIRED)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3>=3.35)

    if(SQLITE3_FOUND AND NOT TARGET SQLite::SQLite3)
        add_library(SQLite::SQLite3 UNKNOWN IMPORTED)
        set_target_properties(SQLite::SQLite3 PROPERTIES
            IMPORTED_LOCATION \"\${SQLITE3_LIBRARIES}\"
            INTERFACE_INCLUDE_DIRECTORIES \"\${SQLITE3_INCLUDE_DIRS}\"
        )
    endif()
endif()

# XXHASH dependency
find_library(XXHASH_LIBRARY_BUNDLED
    NAMES xxhash libxxhash
    PATHS \${_IMPORT_PREFIX}/lib
    NO_DEFAULT_PATH
)

if(XXHASH_LIBRARY_BUNDLED)
    # Found xxhash that was built with this package
    find_path(XXHASH_INCLUDE_DIR_BUNDLED
        NAMES xxhash.h
        PATHS \${_IMPORT_PREFIX}/include
        NO_DEFAULT_PATH
    )

    if(XXHASH_INCLUDE_DIR_BUNDLED)
        # Create shared target if not exists
        if(NOT TARGET xxHash::xxhash)
            add_library(xxHash::xxhash UNKNOWN IMPORTED)
            set_target_properties(xxHash::xxhash PROPERTIES
                IMPORTED_LOCATION \"\${XXHASH_LIBRARY_BUNDLED}\"
                INTERFACE_INCLUDE_DIRECTORIES \"\${XXHASH_INCLUDE_DIR_BUNDLED}\"
            )
        endif()

        # Also look for static version
        find_library(XXHASH_STATIC_LIBRARY_BUNDLED
            NAMES xxhash_static libxxhash_static
            PATHS \${_IMPORT_PREFIX}/lib
            NO_DEFAULT_PATH
        )

        if(XXHASH_STATIC_LIBRARY_BUNDLED AND NOT TARGET xxHash::xxhash_static)
            add_library(xxHash::xxhash_static UNKNOWN IMPORTED)
            set_target_properties(xxHash::xxhash_static PROPERTIES
                IMPORTED_LOCATION \"\${XXHASH_STATIC_LIBRARY_BUNDLED}\"
                INTERFACE_INCLUDE_DIRECTORIES \"\${XXHASH_INCLUDE_DIR_BUNDLED}\"
            )
        endif()
    endif()
else()
    # Try to find system xxhash (no version check, 0.8+ is widely available)
    find_dependency(xxHash QUIET)
    if(NOT xxHash_FOUND)
        message(WARNING \"xxHash not found. Minimum version 0.8.0 is recommended.\")
    endif()
endif()

# YYJSON dependency
find_library(YYJSON_LIBRARY_BUNDLED
    NAMES yyjson libyyjson
    PATHS \${_IMPORT_PREFIX}/lib
    NO_DEFAULT_PATH
)

if(YYJSON_LIBRARY_BUNDLED)
    # Found yyjson that was built with this package
    find_path(YYJSON_INCLUDE_DIR_BUNDLED
        NAMES yyjson.h
        PATHS \${_IMPORT_PREFIX}/include
        NO_DEFAULT_PATH
    )

    if(YYJSON_INCLUDE_DIR_BUNDLED)
        # Create shared target if not exists
        if(NOT TARGET yyjson::yyjson)
            add_library(yyjson::yyjson UNKNOWN IMPORTED)
            set_target_properties(yyjson::yyjson PROPERTIES
                IMPORTED_LOCATION \"\${YYJSON_LIBRARY_BUNDLED}\"
                INTERFACE_INCLUDE_DIRECTORIES \"\${YYJSON_INCLUDE_DIR_BUNDLED}\"
            )
        endif()

        # Also look for static version
        find_library(YYJSON_STATIC_LIBRARY_BUNDLED
            NAMES yyjson_static libyyjson_static
            PATHS \${_IMPORT_PREFIX}/lib
            NO_DEFAULT_PATH
        )

        if(YYJSON_STATIC_LIBRARY_BUNDLED AND NOT TARGET yyjson::yyjson_static)
            add_library(yyjson::yyjson_static UNKNOWN IMPORTED)
            set_target_properties(yyjson::yyjson_static PROPERTIES
                IMPORTED_LOCATION \"\${YYJSON_STATIC_LIBRARY_BUNDLED}\"
                INTERFACE_INCLUDE_DIRECTORIES \"\${YYJSON_INCLUDE_DIR_BUNDLED}\"
            )
        endif()
    endif()
else()
    # Try to find system yyjson (require minimum version 0.10.0)
    find_dependency(yyjson 0.10.0 QUIET)
    if(NOT yyjson_FOUND)
        message(WARNING \"yyjson not found or version too old. Minimum version 0.10.0 is required.\")
    endif()
endif()

# GHC_FILESYSTEM dependency (header-only)
find_path(GHC_FILESYSTEM_INCLUDE_DIR_BUNDLED
    NAMES ghc/filesystem.hpp
    PATHS \${_IMPORT_PREFIX}/include
    NO_DEFAULT_PATH
)

if(GHC_FILESYSTEM_INCLUDE_DIR_BUNDLED AND NOT TARGET ghc_filesystem)
    add_library(ghc_filesystem INTERFACE IMPORTED)
    set_target_properties(ghc_filesystem PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES \"\${GHC_FILESYSTEM_INCLUDE_DIR_BUNDLED}\"
    )
else()
    # Try to find system ghc_filesystem
    find_dependency(ghc_filesystem QUIET)
endif()

# CPP-LOGGER dependency
find_library(CPP_LOGGER_LIBRARY_BUNDLED
    NAMES cpp-logger libcpp-logger
    PATHS \${_IMPORT_PREFIX}/lib
    NO_DEFAULT_PATH
)

if(CPP_LOGGER_LIBRARY_BUNDLED)
    # Found cpp-logger that was built with this package
    find_path(CPP_LOGGER_INCLUDE_DIR_BUNDLED
        NAMES cpp-logger/Logger.h
        PATHS \${_IMPORT_PREFIX}/include
        NO_DEFAULT_PATH
    )

    if(CPP_LOGGER_INCLUDE_DIR_BUNDLED AND NOT TARGET cpp-logger)
        add_library(cpp-logger UNKNOWN IMPORTED)
        set_target_properties(cpp-logger PROPERTIES
            IMPORTED_LOCATION \"\${CPP_LOGGER_LIBRARY_BUNDLED}\"
            INTERFACE_INCLUDE_DIRECTORIES \"\${CPP_LOGGER_INCLUDE_DIR_BUNDLED}\"
        )
    endif()
else()
    # Try to find system cpp-logger
    find_dependency(cpp-logger QUIET)
endif()

# Include the targets file
include(\"\${CMAKE_CURRENT_LIST_DIR}/${PKG_TARGET}Targets.cmake\")

check_required_components(${PKG_TARGET})
")

  # Configure the config file
  set(CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/${PKG_TARGET}Config.cmake")
  configure_package_config_file(
    ${CONFIG_TEMPLATE} ${CONFIG_FILE}
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PKG_TARGET})

  # Create version file
  set(VERSION_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/${PKG_TARGET}ConfigVersion.cmake")
  write_basic_package_version_file(
    ${VERSION_FILE}
    VERSION ${PKG_VERSION}
    COMPATIBILITY SameMajorVersion)

  # Install config files
  install(FILES ${CONFIG_FILE} ${VERSION_FILE}
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PKG_TARGET})

  # Export targets for installation Note: The install(EXPORT ...) is already
  # handled in src/CMakeLists.txt and the export() for build tree is also
  # handled there, so we don't duplicate it here
  install(
    EXPORT ${PKG_TARGET}Targets
    FILE ${PKG_TARGET}Targets.cmake
    NAMESPACE ${PKG_TARGET}::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PKG_TARGET})
endfunction()
