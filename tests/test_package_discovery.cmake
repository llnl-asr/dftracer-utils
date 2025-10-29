# Default library name to dftracer_utils if not specified
if(NOT LIBRARY_NAME)
  set(LIBRARY_NAME "dftracer_utils")
endif()

# Convert library name to uppercase for variable names (e.g., dftracer_utils -> DFTRACER_UTILS)
string(TOUPPER "${LIBRARY_NAME}" LIBRARY_NAME_UPPER)
string(REPLACE "-" "_" LIBRARY_NAME_UPPER "${LIBRARY_NAME_UPPER}")

# Test different discovery methods based on TEST_TYPE
if(TEST_TYPE STREQUAL "pkgconfig")
  # Test pkg-config discovery
  message(STATUS "Testing pkg-config discovery for ${LIBRARY_NAME}...")

  # Set PKG_CONFIG_PATH environment variable
  set(ENV{PKG_CONFIG_PATH} "${PKG_CONFIG_PATH}")

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(${LIBRARY_NAME_UPPER} ${LIBRARY_NAME})

  if(NOT ${LIBRARY_NAME_UPPER}_FOUND)
    message(
      FATAL_ERROR
        "❌ pkg-config discovery failed: ${LIBRARY_NAME} not found")
  endif()

  message(STATUS "✅ pkg-config discovery successful for ${LIBRARY_NAME}")
  message(STATUS "   Version: ${${LIBRARY_NAME_UPPER}_VERSION}")
  message(STATUS "   Include dirs: ${${LIBRARY_NAME_UPPER}_INCLUDE_DIRS}")
  message(STATUS "   Libraries: ${${LIBRARY_NAME_UPPER}_LIBRARIES}")

elseif(TEST_TYPE STREQUAL "cmake")
  # Test CMake find_package discovery
  message(STATUS "Testing CMake find_package discovery for ${LIBRARY_NAME}...")

  # Check if config file exists
  find_file(
    ${LIBRARY_NAME_UPPER}_CONFIG
    NAMES ${LIBRARY_NAME}Config.cmake
    PATHS ${CMAKE_PREFIX_PATH}
    PATH_SUFFIXES lib/cmake/${LIBRARY_NAME}
    NO_DEFAULT_PATH)

  if(NOT ${LIBRARY_NAME_UPPER}_CONFIG)
    message(
      FATAL_ERROR
        "❌ CMake discovery failed: ${LIBRARY_NAME}Config.cmake not found"
    )
  endif()

  message(STATUS "✅ CMake find_package discovery successful for ${LIBRARY_NAME}")
  message(STATUS "   Config file: ${${LIBRARY_NAME_UPPER}_CONFIG}")

elseif(TEST_TYPE STREQUAL "target")
  # Test specific target alias by checking config files
  message(STATUS "Testing target alias: ${TARGET_ALIAS}")

  # Derive library name from target alias (e.g., dftracer_utils_core::shared -> dftracer_utils_core)
  string(REGEX REPLACE "::.*$" "" BASE_LIBRARY_NAME "${TARGET_ALIAS}")
  string(TOUPPER "${BASE_LIBRARY_NAME}" BASE_LIBRARY_NAME_UPPER)
  string(REPLACE "-" "_" BASE_LIBRARY_NAME_UPPER "${BASE_LIBRARY_NAME_UPPER}")

  # Check if config file exists
  find_file(
    ${BASE_LIBRARY_NAME_UPPER}_CONFIG
    NAMES ${BASE_LIBRARY_NAME}Config.cmake
    PATHS ${CMAKE_PREFIX_PATH}
    PATH_SUFFIXES lib/cmake/${BASE_LIBRARY_NAME}
    NO_DEFAULT_PATH)

  if(NOT ${BASE_LIBRARY_NAME_UPPER}_CONFIG)
    message(
      FATAL_ERROR
        "❌ Target alias test failed: "
        "${BASE_LIBRARY_NAME}Config.cmake not found")
  endif()

  # Read config file to check if the target alias exists
  file(READ ${${BASE_LIBRARY_NAME_UPPER}_CONFIG} CONFIG_CONTENT)
  string(FIND "${CONFIG_CONTENT}" "${TARGET_ALIAS}" CONFIG_ALIAS_FOUND)

  # Also check library-specific targets files
  find_file(
    ${BASE_LIBRARY_NAME_UPPER}_SHARED_TARGETS
    NAMES ${BASE_LIBRARY_NAME}_sharedTargets.cmake
    PATHS ${CMAKE_PREFIX_PATH}
    PATH_SUFFIXES lib/cmake/${BASE_LIBRARY_NAME}_shared
    NO_DEFAULT_PATH)

  find_file(
    ${BASE_LIBRARY_NAME_UPPER}_STATIC_TARGETS
    NAMES ${BASE_LIBRARY_NAME}_staticTargets.cmake
    PATHS ${CMAKE_PREFIX_PATH}
    PATH_SUFFIXES lib/cmake/${BASE_LIBRARY_NAME}_static
    NO_DEFAULT_PATH)

  # Also check unified targets file (for dftracer_utils only)
  find_file(
    ${BASE_LIBRARY_NAME_UPPER}_TARGETS
    NAMES ${BASE_LIBRARY_NAME}Targets.cmake
    PATHS ${CMAKE_PREFIX_PATH}
    PATH_SUFFIXES lib/cmake/${BASE_LIBRARY_NAME}
    NO_DEFAULT_PATH)

  set(TARGETS_ALIAS_FOUND -1)
  if(${BASE_LIBRARY_NAME_UPPER}_SHARED_TARGETS)
    file(READ ${${BASE_LIBRARY_NAME_UPPER}_SHARED_TARGETS} SHARED_TARGETS_CONTENT)
    string(FIND "${SHARED_TARGETS_CONTENT}" "${TARGET_ALIAS}"
           SHARED_ALIAS_FOUND)
    if(SHARED_ALIAS_FOUND GREATER -1)
      set(TARGETS_ALIAS_FOUND ${SHARED_ALIAS_FOUND})
      set(FOUND_TARGETS_FILE ${${BASE_LIBRARY_NAME_UPPER}_SHARED_TARGETS})
    endif()
  endif()

  if(${BASE_LIBRARY_NAME_UPPER}_STATIC_TARGETS AND TARGETS_ALIAS_FOUND EQUAL -1)
    file(READ ${${BASE_LIBRARY_NAME_UPPER}_STATIC_TARGETS} STATIC_TARGETS_CONTENT)
    string(FIND "${STATIC_TARGETS_CONTENT}" "${TARGET_ALIAS}"
           STATIC_ALIAS_FOUND)
    if(STATIC_ALIAS_FOUND GREATER -1)
      set(TARGETS_ALIAS_FOUND ${STATIC_ALIAS_FOUND})
      set(FOUND_TARGETS_FILE ${${BASE_LIBRARY_NAME_UPPER}_STATIC_TARGETS})
    endif()
  endif()

  if(${BASE_LIBRARY_NAME_UPPER}_TARGETS AND TARGETS_ALIAS_FOUND EQUAL -1)
    file(READ ${${BASE_LIBRARY_NAME_UPPER}_TARGETS} TARGETS_CONTENT)
    string(FIND "${TARGETS_CONTENT}" "${TARGET_ALIAS}"
           TARGETS_FOUND)
    if(TARGETS_FOUND GREATER -1)
      set(TARGETS_ALIAS_FOUND ${TARGETS_FOUND})
      set(FOUND_TARGETS_FILE ${${BASE_LIBRARY_NAME_UPPER}_TARGETS})
    endif()
  endif()

  if(CONFIG_ALIAS_FOUND EQUAL -1 AND TARGETS_ALIAS_FOUND EQUAL -1)
    message(
      FATAL_ERROR
        "❌ Target alias test failed: ${TARGET_ALIAS} not found in "
        "config or targets files")
  endif()

  message(STATUS "✅ Target alias test successful: ${TARGET_ALIAS}")
  if(CONFIG_ALIAS_FOUND GREATER -1)
    message(STATUS "   Found in config file: ${${BASE_LIBRARY_NAME_UPPER}_CONFIG}")
  endif()
  if(TARGETS_ALIAS_FOUND GREATER -1)
    message(STATUS "   Found in targets file: ${FOUND_TARGETS_FILE}")
  endif()

else()
  message(FATAL_ERROR "Unknown TEST_TYPE: ${TEST_TYPE}")
endif()
