# LibraryHelpers.cmake - Utilities for creating libraries with automatic
# shared/static variants, exports, and package configs

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

#[=[
Creates a library with automatic shared/static variants, complete with:
- Shared library target (if BUILD_SHARED is ON)
- Static library target (if BUILD_STATIC is ON)
- Aliases with nice namespaces
- Export configuration
- CMake package config files (programmatically generated)
- pkg-config files

Usage:
  create_library(
    NAME base_name                    # e.g., "dftracer_utils_core"
    NAMESPACE namespace               # e.g., "dftracer_utils_core" -> creates dftracer_utils_core::shared
    SOURCES file1.cpp file2.cpp ...   # Source files
    [PUBLIC_HEADERS file1.h ...]      # Public headers to install
    [VERSION version]                 # Defaults to PROJECT_VERSION
    [DESCRIPTION "desc"]              # Package description
    [URL "https://..."]               # Package URL

    # Dependencies
    [PUBLIC_LINK_LIBRARIES lib1 lib2 ...]     # Public dependencies (both shared and static)
    [PRIVATE_LINK_LIBRARIES lib1 lib2 ...]    # Private dependencies (both shared and static)
    [INTERFACE_LINK_LIBRARIES lib1 lib2 ...]  # Interface dependencies (for INTERFACE libraries)
    [SHARED_LINK_LIBRARIES lib1 lib2 ...]     # Public dependencies ONLY for shared variant
    [STATIC_LINK_LIBRARIES lib1 lib2 ...]     # Public dependencies ONLY for static variant

    # Build options
    [BUILD_SHARED ON|OFF]             # Build shared library (defaults to parent option)
    [BUILD_STATIC ON|OFF]             # Build static library (defaults to parent option)
    [BUILD_INTERFACE ON|OFF]          # Build as INTERFACE library only

    # Include directories
    [PUBLIC_INCLUDE_DIRS dir1 dir2 ...] # Public include directories
    [PRIVATE_INCLUDE_DIRS dir1 ...]     # Private include directories

    # Compile options
    [PUBLIC_COMPILE_OPTIONS opt1 ...]   # Public compile options
    [PRIVATE_COMPILE_OPTIONS opt1 ...]  # Private compile options

    # Compile definitions
    [PUBLIC_COMPILE_DEFINITIONS def1 ...] # Public compile definitions
    [PRIVATE_COMPILE_DEFINITIONS def1 ...] # Private compile definitions

    # Properties
    [PROPERTIES prop1 val1 prop2 val2 ...] # Additional target properties

    # Package config
    [PKG_CONFIG_REQUIRES "dep1 dep2"]      # pkg-config Requires
    [PKG_CONFIG_LIBS_PRIVATE "libs"]       # pkg-config Libs.private
    [PKG_CONFIG_CFLAGS_PRIVATE "flags"]    # pkg-config Cflags.private

    # CMake config dependencies (for programmatic generation)
    [FIND_DEPENDENCIES dep1 dep2 ...]      # Dependencies to find_dependency() in config
  )

Returns (sets in parent scope):
  ${NAME}_SHARED_TARGET - Name of shared library target (if created)
  ${NAME}_STATIC_TARGET - Name of static library target (if created)
  ${NAME}_INTERFACE_TARGET - Name of interface library target (if created)
  ${NAME}_TARGETS - List of all created targets
#]=]
function(create_library)
  set(options BUILD_INTERFACE)
  set(oneValueArgs
      NAME
      NAMESPACE
      VERSION
      DESCRIPTION
      URL
      BUILD_SHARED
      BUILD_STATIC
      PKG_CONFIG_REQUIRES
      PKG_CONFIG_LIBS_PRIVATE
      PKG_CONFIG_CFLAGS_PRIVATE)
  set(multiValueArgs
      SOURCES
      PUBLIC_HEADERS
      PUBLIC_LINK_LIBRARIES
      PRIVATE_LINK_LIBRARIES
      INTERFACE_LINK_LIBRARIES
      SHARED_LINK_LIBRARIES # Specific to shared variant only
      STATIC_LINK_LIBRARIES # Specific to static variant only
      PUBLIC_INCLUDE_DIRS
      PRIVATE_INCLUDE_DIRS
      PUBLIC_COMPILE_OPTIONS
      PRIVATE_COMPILE_OPTIONS
      PUBLIC_COMPILE_DEFINITIONS
      PRIVATE_COMPILE_DEFINITIONS
      PROPERTIES
      FIND_DEPENDENCIES)

  cmake_parse_arguments(LIB "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  # Validate required arguments
  if(NOT LIB_NAME)
    message(FATAL_ERROR "create_library: NAME is required")
  endif()

  if(NOT LIB_NAMESPACE)
    set(LIB_NAMESPACE ${LIB_NAME})
  endif()

  # Set defaults
  if(NOT DEFINED LIB_VERSION)
    set(LIB_VERSION ${PROJECT_VERSION})
  endif()

  if(NOT DEFINED LIB_DESCRIPTION)
    set(LIB_DESCRIPTION "${LIB_NAME} library")
  endif()

  # Determine what to build
  if(LIB_BUILD_INTERFACE)
    set(BUILD_INTERFACE_LIB ON)
    set(BUILD_SHARED_LIB OFF)
    set(BUILD_STATIC_LIB OFF)
  else()
    if(NOT DEFINED LIB_BUILD_SHARED)
      # Use parent variable if available
      string(TOUPPER ${LIB_NAME} LIB_NAME_UPPER)
      if(DEFINED ${LIB_NAME_UPPER}_BUILD_SHARED)
        set(LIB_BUILD_SHARED ${${LIB_NAME_UPPER}_BUILD_SHARED})
      else()
        set(LIB_BUILD_SHARED ON)
      endif()
    endif()

    if(NOT DEFINED LIB_BUILD_STATIC)
      string(TOUPPER ${LIB_NAME} LIB_NAME_UPPER)
      if(DEFINED ${LIB_NAME_UPPER}_BUILD_STATIC)
        set(LIB_BUILD_STATIC ${${LIB_NAME_UPPER}_BUILD_STATIC})
      else()
        set(LIB_BUILD_STATIC ON)
      endif()
    endif()

    set(BUILD_SHARED_LIB ${LIB_BUILD_SHARED})
    set(BUILD_STATIC_LIB ${LIB_BUILD_STATIC})
    set(BUILD_INTERFACE_LIB OFF)
  endif()

  set(CREATED_TARGETS "")

  # Helper macro to set common properties
  macro(setup_target_common TARGET_NAME TARGET_TYPE VARIANT)
    # Link libraries
    if(LIB_PUBLIC_LINK_LIBRARIES)
      target_link_libraries(${TARGET_NAME} PUBLIC ${LIB_PUBLIC_LINK_LIBRARIES})
    endif()

    if(LIB_PRIVATE_LINK_LIBRARIES AND NOT ${TARGET_TYPE} STREQUAL "INTERFACE")
      target_link_libraries(${TARGET_NAME}
                            PRIVATE ${LIB_PRIVATE_LINK_LIBRARIES})
    endif()

    if(LIB_INTERFACE_LINK_LIBRARIES)
      target_link_libraries(${TARGET_NAME}
                            INTERFACE ${LIB_INTERFACE_LINK_LIBRARIES})
    endif()

    # Variant-specific libraries
    if("${VARIANT}" STREQUAL "shared" AND LIB_SHARED_LINK_LIBRARIES)
      target_link_libraries(${TARGET_NAME} PUBLIC ${LIB_SHARED_LINK_LIBRARIES})
    endif()

    if("${VARIANT}" STREQUAL "static" AND LIB_STATIC_LINK_LIBRARIES)
      target_link_libraries(${TARGET_NAME} PUBLIC ${LIB_STATIC_LINK_LIBRARIES})
    endif()

    # Include directories
    if(LIB_PUBLIC_INCLUDE_DIRS)
      target_include_directories(${TARGET_NAME}
                                 PUBLIC ${LIB_PUBLIC_INCLUDE_DIRS})
    endif()

    if(LIB_PRIVATE_INCLUDE_DIRS AND NOT ${TARGET_TYPE} STREQUAL "INTERFACE")
      target_include_directories(${TARGET_NAME}
                                 PRIVATE ${LIB_PRIVATE_INCLUDE_DIRS})
    endif()

    # Compile options
    if(LIB_PUBLIC_COMPILE_OPTIONS)
      target_compile_options(${TARGET_NAME}
                             PUBLIC ${LIB_PUBLIC_COMPILE_OPTIONS})
    endif()

    if(LIB_PRIVATE_COMPILE_OPTIONS AND NOT ${TARGET_TYPE} STREQUAL "INTERFACE")
      target_compile_options(${TARGET_NAME}
                             PRIVATE ${LIB_PRIVATE_COMPILE_OPTIONS})
    endif()

    # Compile definitions
    if(LIB_PUBLIC_COMPILE_DEFINITIONS)
      target_compile_definitions(${TARGET_NAME}
                                 PUBLIC ${LIB_PUBLIC_COMPILE_DEFINITIONS})
    endif()

    if(LIB_PRIVATE_COMPILE_DEFINITIONS AND NOT ${TARGET_TYPE} STREQUAL
                                           "INTERFACE")
      target_compile_definitions(${TARGET_NAME}
                                 PRIVATE ${LIB_PRIVATE_COMPILE_DEFINITIONS})
    endif()

    # Additional properties
    if(LIB_PROPERTIES)
      set_target_properties(${TARGET_NAME} PROPERTIES ${LIB_PROPERTIES})
    endif()
  endmacro()

  # ============================================================================
  # Create INTERFACE library
  # ============================================================================
  if(BUILD_INTERFACE_LIB)
    set(INTERFACE_TARGET_NAME ${LIB_NAME})
    add_library(${INTERFACE_TARGET_NAME} INTERFACE)
    add_library(${LIB_NAMESPACE}::${LIB_NAME} ALIAS ${INTERFACE_TARGET_NAME})

    setup_target_common(${INTERFACE_TARGET_NAME} "INTERFACE" "interface")

    list(APPEND CREATED_TARGETS ${INTERFACE_TARGET_NAME})
    set(${LIB_NAME}_INTERFACE_TARGET
        ${INTERFACE_TARGET_NAME}
        PARENT_SCOPE)

    # Export and install
    install(
      TARGETS ${INTERFACE_TARGET_NAME}
      EXPORT ${LIB_NAME}Targets
      INCLUDES
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Create package configs
    _create_library_package_configs(
      NAME
      ${LIB_NAME}
      VERSION
      ${LIB_VERSION}
      DESCRIPTION
      ${LIB_DESCRIPTION}
      URL
      ${LIB_URL}
      PKG_CONFIG_REQUIRES
      ${LIB_PKG_CONFIG_REQUIRES}
      PKG_CONFIG_LIBS_PRIVATE
      ${LIB_PKG_CONFIG_LIBS_PRIVATE}
      PKG_CONFIG_CFLAGS_PRIVATE
      ${LIB_PKG_CONFIG_CFLAGS_PRIVATE}
      FIND_DEPENDENCIES
      ${LIB_FIND_DEPENDENCIES}
      OUTPUT_NAME
      ${LIB_NAME})
  endif()

  # ============================================================================
  # Create SHARED library
  # ============================================================================
  if(BUILD_SHARED_LIB)
    set(SHARED_TARGET_NAME ${LIB_NAME}_shared)
    add_library(${SHARED_TARGET_NAME} SHARED ${LIB_SOURCES})
    add_library(${LIB_NAMESPACE}::shared ALIAS ${SHARED_TARGET_NAME})

    # Set output name to base name (without _shared suffix)
    get_filename_component(BASE_NAME ${LIB_NAME} NAME)
    set_target_properties(
      ${SHARED_TARGET_NAME}
      PROPERTIES OUTPUT_NAME ${BASE_NAME}
                 VERSION ${LIB_VERSION}
                 SOVERSION ${PROJECT_VERSION_MAJOR}
                 LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
                 ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

    setup_target_common(${SHARED_TARGET_NAME} "SHARED" "shared")

    list(APPEND CREATED_TARGETS ${SHARED_TARGET_NAME})
    set(${LIB_NAME}_SHARED_TARGET
        ${SHARED_TARGET_NAME}
        PARENT_SCOPE)

    # Export and install
    install(
      TARGETS ${SHARED_TARGET_NAME}
      EXPORT ${LIB_NAME}_sharedTargets
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      INCLUDES
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Create package configs for shared
    _create_library_package_configs(
      NAME
      ${LIB_NAME}_shared
      VERSION
      ${LIB_VERSION}
      DESCRIPTION
      "${LIB_DESCRIPTION} (shared)"
      URL
      ${LIB_URL}
      PKG_CONFIG_REQUIRES
      ${LIB_PKG_CONFIG_REQUIRES}
      PKG_CONFIG_LIBS_PRIVATE
      ${LIB_PKG_CONFIG_LIBS_PRIVATE}
      PKG_CONFIG_CFLAGS_PRIVATE
      ${LIB_PKG_CONFIG_CFLAGS_PRIVATE}
      FIND_DEPENDENCIES
      ${LIB_FIND_DEPENDENCIES}
      OUTPUT_NAME
      ${BASE_NAME})
  endif()

  # ============================================================================
  # Create STATIC library
  # ============================================================================
  if(BUILD_STATIC_LIB)
    set(STATIC_TARGET_NAME ${LIB_NAME}_static)
    add_library(${STATIC_TARGET_NAME} STATIC ${LIB_SOURCES})
    add_library(${LIB_NAMESPACE}::static ALIAS ${STATIC_TARGET_NAME})

    # Set output name to base name (without _static suffix)
    get_filename_component(BASE_NAME ${LIB_NAME} NAME)
    set_target_properties(
      ${STATIC_TARGET_NAME}
      PROPERTIES OUTPUT_NAME ${BASE_NAME} ARCHIVE_OUTPUT_DIRECTORY
                                          ${CMAKE_BINARY_DIR}/lib)

    setup_target_common(${STATIC_TARGET_NAME} "STATIC" "static")

    list(APPEND CREATED_TARGETS ${STATIC_TARGET_NAME})
    set(${LIB_NAME}_STATIC_TARGET
        ${STATIC_TARGET_NAME}
        PARENT_SCOPE)

    # Export and install
    install(
      TARGETS ${STATIC_TARGET_NAME}
      EXPORT ${LIB_NAME}_staticTargets
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      INCLUDES
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Create package configs for static
    _create_library_package_configs(
      NAME
      ${LIB_NAME}_static
      VERSION
      ${LIB_VERSION}
      DESCRIPTION
      "${LIB_DESCRIPTION} (static)"
      URL
      ${LIB_URL}
      PKG_CONFIG_REQUIRES
      ${LIB_PKG_CONFIG_REQUIRES}
      PKG_CONFIG_LIBS_PRIVATE
      ${LIB_PKG_CONFIG_LIBS_PRIVATE}
      PKG_CONFIG_CFLAGS_PRIVATE
      ${LIB_PKG_CONFIG_CFLAGS_PRIVATE}
      FIND_DEPENDENCIES
      ${LIB_FIND_DEPENDENCIES}
      OUTPUT_NAME
      ${BASE_NAME})
  endif()

  # Install public headers if specified
  if(LIB_PUBLIC_HEADERS)
    install(FILES ${LIB_PUBLIC_HEADERS}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${LIB_NAME})
  endif()

  # Return list of all created targets
  set(${LIB_NAME}_TARGETS
      ${CREATED_TARGETS}
      PARENT_SCOPE)
endfunction()

#[=[
Internal function to create CMake and pkg-config package configuration files
This generates the config files programmatically without template files
#]=]
function(_create_library_package_configs)
  set(options "")
  set(oneValueArgs
      NAME
      VERSION
      DESCRIPTION
      URL
      PKG_CONFIG_REQUIRES
      PKG_CONFIG_LIBS_PRIVATE
      PKG_CONFIG_CFLAGS_PRIVATE
      OUTPUT_NAME)
  set(multiValueArgs FIND_DEPENDENCIES)

  cmake_parse_arguments(PKG "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  # ============================================================================
  # Create pkg-config file
  # ============================================================================
  set(PC_CONTENT "")
  string(APPEND PC_CONTENT "prefix=${CMAKE_INSTALL_PREFIX}\n")
  string(APPEND PC_CONTENT "exec_prefix=\${prefix}\n")
  string(APPEND PC_CONTENT "libdir=\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}\n")
  string(APPEND PC_CONTENT
         "includedir=\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}\n\n")
  string(APPEND PC_CONTENT "Name: ${PKG_NAME}\n")
  string(APPEND PC_CONTENT "Description: ${PKG_DESCRIPTION}\n")
  string(APPEND PC_CONTENT "Version: ${PKG_VERSION}\n")

  if(PKG_URL)
    string(APPEND PC_CONTENT "URL: ${PKG_URL}\n")
  endif()

  if(PKG_PKG_CONFIG_REQUIRES)
    string(APPEND PC_CONTENT "Requires: ${PKG_PKG_CONFIG_REQUIRES}\n")
  endif()

  if(PKG_PKG_CONFIG_LIBS_PRIVATE)
    string(APPEND PC_CONTENT "Libs.private: ${PKG_PKG_CONFIG_LIBS_PRIVATE}\n")
  endif()

  if(PKG_PKG_CONFIG_CFLAGS_PRIVATE)
    string(APPEND PC_CONTENT
           "Cflags.private: ${PKG_PKG_CONFIG_CFLAGS_PRIVATE}\n")
  endif()

  string(APPEND PC_CONTENT "Libs: -L\${libdir} -l${PKG_OUTPUT_NAME}\n")
  string(APPEND PC_CONTENT "Cflags: -I\${includedir}\n")

  set(PC_FILE "${CMAKE_CURRENT_BINARY_DIR}/${PKG_NAME}.pc")
  file(WRITE ${PC_FILE} ${PC_CONTENT})
  install(FILES ${PC_FILE} DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

  # ============================================================================
  # Create CMake config file programmatically
  # ============================================================================
  set(CONFIG_CONTENT "")
  string(APPEND CONFIG_CONTENT "@PACKAGE_INIT@\n\n")
  string(APPEND CONFIG_CONTENT "include(CMakeFindDependencyMacro)\n\n")

  # Add find_dependency calls for each dependency
  if(PKG_FIND_DEPENDENCIES)
    string(APPEND CONFIG_CONTENT "# Find dependencies\n")
    foreach(dep ${PKG_FIND_DEPENDENCIES})
      string(APPEND CONFIG_CONTENT "find_dependency(${dep})\n")
    endforeach()
    string(APPEND CONFIG_CONTENT "\n")
  endif()

  # Include the targets file
  string(APPEND CONFIG_CONTENT "# Include the targets file\n"
         "include(\"\${CMAKE_CURRENT_LIST_DIR}/${PKG_NAME}Targets.cmake\")\n\n"
         "check_required_components(${PKG_NAME})\n")

  # Write the config template
  set(CONFIG_TEMPLATE "${CMAKE_CURRENT_BINARY_DIR}/${PKG_NAME}Config.cmake.in")
  file(WRITE ${CONFIG_TEMPLATE} ${CONFIG_CONTENT})

  # Configure the config file
  set(CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/${PKG_NAME}Config.cmake")
  configure_package_config_file(
    ${CONFIG_TEMPLATE} ${CONFIG_FILE}
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PKG_NAME})

  # Create version file
  set(VERSION_FILE "${CMAKE_CURRENT_BINARY_DIR}/${PKG_NAME}ConfigVersion.cmake")
  write_basic_package_version_file(
    ${VERSION_FILE}
    VERSION ${PKG_VERSION}
    COMPATIBILITY SameMajorVersion)

  # Install config files
  install(FILES ${CONFIG_FILE} ${VERSION_FILE}
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PKG_NAME})

  # Install export targets
  install(
    EXPORT ${PKG_NAME}Targets
    FILE ${PKG_NAME}Targets.cmake
    NAMESPACE ${PKG_NAME}::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PKG_NAME})
endfunction()

#[=[
Creates a unified wrapper config for a library that has shared/static variants.
This allows users to discover the library without specifying the variant.

Usage:
  create_unified_library_config(
    NAME library_name                 # e.g., "dftracer_utils_core"
    VERSION version                   # Package version
    [DESCRIPTION "desc"]              # Package description
    [URL "https://..."]              # Package URL
    [PKG_CONFIG_REQUIRES "deps"]     # pkg-config dependencies
    [FIND_DEPENDENCIES dep1 dep2]    # CMake dependencies to find
    [BUILD_SHARED ON|OFF]            # Whether shared variant exists
    [BUILD_STATIC ON|OFF]            # Whether static variant exists
  )

This creates:
- A unified CMake config file (NAME/NAMEConfig.cmake)
- A unified pkg-config file (NAME.pc)
- Interface library targets that aggregate the variants
#]=]
function(create_unified_library_config)
  set(options "")
  set(oneValueArgs
      NAME
      VERSION
      DESCRIPTION
      URL
      PKG_CONFIG_REQUIRES
      BUILD_SHARED
      BUILD_STATIC)
  set(multiValueArgs FIND_DEPENDENCIES)

  cmake_parse_arguments(CFG "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  if(NOT CFG_NAME)
    message(FATAL_ERROR "NAME is required for create_unified_library_config")
  endif()

  if(NOT CFG_VERSION)
    set(CFG_VERSION ${PROJECT_VERSION})
  endif()

  if(NOT CFG_DESCRIPTION)
    set(CFG_DESCRIPTION "${CFG_NAME} library")
  endif()

  # Determine which variants exist
  if(NOT DEFINED CFG_BUILD_SHARED)
    set(CFG_BUILD_SHARED ON)
  endif()
  if(NOT DEFINED CFG_BUILD_STATIC)
    set(CFG_BUILD_STATIC ON)
  endif()

  # Determine preferred type (shared takes precedence if both exist)
  if(CFG_BUILD_SHARED)
    set(PREFERRED_TYPE "shared")
  elseif(CFG_BUILD_STATIC)
    set(PREFERRED_TYPE "static")
  else()
    message(
      FATAL_ERROR "At least one of BUILD_SHARED or BUILD_STATIC must be ON")
  endif()

  # ==========================================================================
  # Create unified CMake config
  # ==========================================================================
  set(UNIFIED_CONFIG_CONTENT "")
  string(APPEND UNIFIED_CONFIG_CONTENT "@PACKAGE_INIT@\n\n")
  string(APPEND UNIFIED_CONFIG_CONTENT
         "# ${CFG_NAME} unified package config\n\n")

  # Add find_dependency calls if specified
  if(CFG_FIND_DEPENDENCIES)
    string(APPEND UNIFIED_CONFIG_CONTENT
           "include(CMakeFindDependencyMacro)\n\n")
    foreach(dep ${CFG_FIND_DEPENDENCIES})
      string(APPEND UNIFIED_CONFIG_CONTENT "find_dependency(${dep})\n")
    endforeach()
    string(APPEND UNIFIED_CONFIG_CONTENT "\n")
  endif()

  # Include variant-specific configs
  string(APPEND UNIFIED_CONFIG_CONTENT "# Include variant-specific configs\n")
  if(CFG_BUILD_SHARED)
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "include(\${CMAKE_CURRENT_LIST_DIR}/../${CFG_NAME}_shared/${CFG_NAME}_sharedConfig.cmake OPTIONAL)\n"
    )
  endif()
  if(CFG_BUILD_STATIC)
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "include(\${CMAKE_CURRENT_LIST_DIR}/../${CFG_NAME}_static/${CFG_NAME}_staticConfig.cmake OPTIONAL)\n"
    )
  endif()
  string(APPEND UNIFIED_CONFIG_CONTENT "\n")

  # Create standard aliases
  string(APPEND UNIFIED_CONFIG_CONTENT
         "# Create standard aliases for the specific library types\n")
  if(CFG_BUILD_SHARED)
    string(APPEND UNIFIED_CONFIG_CONTENT
           "if(TARGET ${CFG_NAME}_shared::${CFG_NAME}_shared)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET ${CFG_NAME}::shared)\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(${CFG_NAME}::shared ALIAS ${CFG_NAME}_shared::${CFG_NAME}_shared)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "endif()\n\n")
  endif()

  if(CFG_BUILD_STATIC)
    string(APPEND UNIFIED_CONFIG_CONTENT
           "if(TARGET ${CFG_NAME}_static::${CFG_NAME}_static)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET ${CFG_NAME}::static)\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(${CFG_NAME}::static ALIAS ${CFG_NAME}_static::${CFG_NAME}_static)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "endif()\n\n")
  endif()

  # Create unified alias pointing to preferred variant
  string(
    APPEND
    UNIFIED_CONFIG_CONTENT
    "# Create unified alias that points to the preferred library type (${PREFERRED_TYPE})\n"
  )
  if(CFG_BUILD_SHARED)
    string(APPEND UNIFIED_CONFIG_CONTENT
           "if(TARGET ${CFG_NAME}_shared::${CFG_NAME}_shared)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET ${CFG_NAME}::${CFG_NAME})\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(${CFG_NAME}::${CFG_NAME} ALIAS ${CFG_NAME}_shared::${CFG_NAME}_shared)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "endif()\n")
  elseif(CFG_BUILD_STATIC)
    string(APPEND UNIFIED_CONFIG_CONTENT
           "if(TARGET ${CFG_NAME}_static::${CFG_NAME}_static)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET ${CFG_NAME}::${CFG_NAME})\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(${CFG_NAME}::${CFG_NAME} ALIAS ${CFG_NAME}_static::${CFG_NAME}_static)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "endif()\n")
  endif()

  string(APPEND UNIFIED_CONFIG_CONTENT
         "\ncheck_required_components(${CFG_NAME})\n")

  # Write and configure the config file
  set(UNIFIED_CONFIG_TEMPLATE
      "${CMAKE_CURRENT_BINARY_DIR}/${CFG_NAME}Config.cmake.in")
  file(WRITE ${UNIFIED_CONFIG_TEMPLATE} ${UNIFIED_CONFIG_CONTENT})

  set(UNIFIED_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/${CFG_NAME}Config.cmake")
  configure_package_config_file(
    ${UNIFIED_CONFIG_TEMPLATE} ${UNIFIED_CONFIG_FILE}
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${CFG_NAME})

  # Create version file
  set(UNIFIED_VERSION_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/${CFG_NAME}ConfigVersion.cmake")
  write_basic_package_version_file(
    ${UNIFIED_VERSION_FILE}
    VERSION ${CFG_VERSION}
    COMPATIBILITY SameMajorVersion)

  # Install config files
  install(FILES ${UNIFIED_CONFIG_FILE} ${UNIFIED_VERSION_FILE}
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${CFG_NAME})

  # ==========================================================================
  # Create unified pkg-config file
  # ==========================================================================
  create_pkgconfig_file(
    TARGET
    ${CFG_NAME}
    NAME
    ${CFG_NAME}
    VERSION
    ${CFG_VERSION}
    DESCRIPTION
    "${CFG_DESCRIPTION}"
    URL
    "${CFG_URL}"
    REQUIRES
    "${CFG_PKG_CONFIG_REQUIRES}")

endfunction()
