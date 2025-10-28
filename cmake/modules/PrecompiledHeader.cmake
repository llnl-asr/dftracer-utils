# ##############################################################################
# PrecompiledHeader.cmake
#
# This module provides functions to automatically detect and configure
# precompiled headers for CMake targets.
#
# Functions:
#   - detect_common_headers: Analyzes source files to find commonly used headers
#   - use_precompiled_header: Applies precompiled headers to a target
#
# Usage:
#   include(PrecompiledHeader)
#   use_precompiled_header(my_target)
# ##############################################################################

include_guard(GLOBAL)

# ##############################################################################
# detect_common_headers
#
# Analyzes source files in a directory to find the most commonly included
# standard library and system headers.
#
# Parameters:
#   SOURCE_DIR - Directory to scan for source files
#   OUTPUT_VAR - Variable name to store the list of detected headers
#   MIN_COUNT  - Minimum number of occurrences to include a header (default: 3)
#
# Example:
#   detect_common_headers(
#     SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src
#     OUTPUT_VAR COMMON_HEADERS
#     MIN_COUNT 5
#   )
# ##############################################################################
function(detect_common_headers)
  set(options "")
  set(oneValueArgs SOURCE_DIR OUTPUT_VAR MIN_COUNT)
  set(multiValueArgs "")
  cmake_parse_arguments(
    ARG
    "${options}"
    "${oneValueArgs}"
    "${multiValueArgs}"
    ${ARGN})

  if(NOT ARG_SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required for detect_common_headers")
  endif()

  if(NOT ARG_OUTPUT_VAR)
    message(FATAL_ERROR "OUTPUT_VAR is required for detect_common_headers")
  endif()

  if(NOT ARG_MIN_COUNT)
    set(ARG_MIN_COUNT 3)
  endif()

  # Find all C++ source and header files
  file(
    GLOB_RECURSE
    ALL_SOURCES
    "${ARG_SOURCE_DIR}/*.cpp"
    "${ARG_SOURCE_DIR}/*.cc"
    "${ARG_SOURCE_DIR}/*.cxx"
    "${ARG_SOURCE_DIR}/*.h"
    "${ARG_SOURCE_DIR}/*.hpp")

  if(NOT ALL_SOURCES)
    message(
      WARNING
        "No source files found in ${ARG_SOURCE_DIR} for PCH detection")
    set(${ARG_OUTPUT_VAR}
        ""
        PARENT_SCOPE)
    return()
  endif()

  # Filter out files from directories that may have optional dependencies
  # (e.g., Python bindings that are only built conditionally)
  set(FILTERED_SOURCES "")
  foreach(SOURCE_FILE ${ALL_SOURCES})
    # Exclude Python binding files (only built when DFTRACER_UTILS_BUILD_PYTHON is ON)
    if(NOT SOURCE_FILE MATCHES "/python/")
      list(APPEND FILTERED_SOURCES "${SOURCE_FILE}")
    endif()
  endforeach()

  set(ALL_SOURCES "${FILTERED_SOURCES}")

  # Use a map (list of key-value pairs) to count header occurrences
  set(HEADER_COUNTS "")

  # Read each file and extract #include <...> directives
  foreach(SOURCE_FILE ${ALL_SOURCES})
    file(STRINGS "${SOURCE_FILE}" FILE_CONTENTS)

    foreach(LINE ${FILE_CONTENTS})
      # Match #include <header> (system/standard library headers)
      if(LINE MATCHES "^[ \t]*#[ \t]*include[ \t]*<([^>]+)>")
        set(HEADER "${CMAKE_MATCH_1}")

        # Filter out project-specific headers (containing /)
        # and specific third-party headers that might not be available everywhere
        if(NOT HEADER MATCHES "/"
           AND NOT HEADER MATCHES "^Python\\.h$"
           AND NOT HEADER MATCHES "^argparse/")
          # Increment count for this header
          set(FOUND FALSE)
          set(NEW_COUNTS "")

          foreach(ENTRY ${HEADER_COUNTS})
            string(REGEX MATCH "^([^:]+):([0-9]+)$" MATCH_RESULT "${ENTRY}")
            if(MATCH_RESULT)
              set(ENTRY_HEADER "${CMAKE_MATCH_1}")
              set(ENTRY_COUNT "${CMAKE_MATCH_2}")

              if(ENTRY_HEADER STREQUAL HEADER)
                math(EXPR ENTRY_COUNT "${ENTRY_COUNT} + 1")
                list(APPEND NEW_COUNTS "${ENTRY_HEADER}:${ENTRY_COUNT}")
                set(FOUND TRUE)
              else()
                list(APPEND NEW_COUNTS "${ENTRY}")
              endif()
            endif()
          endforeach()

          if(NOT FOUND)
            list(APPEND NEW_COUNTS "${HEADER}:1")
          endif()

          set(HEADER_COUNTS "${NEW_COUNTS}")
        endif()
      endif()
    endforeach()
  endforeach()

  # Filter headers by minimum count and sort by frequency
  set(FILTERED_HEADERS "")
  set(SORTED_ENTRIES "")

  foreach(ENTRY ${HEADER_COUNTS})
    string(REGEX MATCH "^([^:]+):([0-9]+)$" MATCH_RESULT "${ENTRY}")
    if(MATCH_RESULT)
      set(HEADER "${CMAKE_MATCH_1}")
      set(COUNT "${CMAKE_MATCH_2}")

      if(COUNT GREATER_EQUAL ARG_MIN_COUNT)
        # Pad count for sorting (assumes count < 10000)
        string(LENGTH "${COUNT}" COUNT_LEN)
        math(EXPR PAD_LEN "5 - ${COUNT_LEN}")
        set(PADDED_COUNT "${COUNT}")
        foreach(I RANGE 1 ${PAD_LEN})
          set(PADDED_COUNT "0${PADDED_COUNT}")
        endforeach()

        list(APPEND SORTED_ENTRIES "${PADDED_COUNT}:${HEADER}")
      endif()
    endif()
  endforeach()

  # Sort by count (descending)
  list(SORT SORTED_ENTRIES ORDER DESCENDING)

  # Extract headers from sorted list
  foreach(ENTRY ${SORTED_ENTRIES})
    string(REGEX MATCH "^[0-9]+:(.+)$" MATCH_RESULT "${ENTRY}")
    if(MATCH_RESULT)
      list(APPEND FILTERED_HEADERS "<${CMAKE_MATCH_1}>")
    endif()
  endforeach()

  # Return the list
  set(${ARG_OUTPUT_VAR}
      "${FILTERED_HEADERS}"
      PARENT_SCOPE)

  # Optional: Print detected headers for debugging
  if(FILTERED_HEADERS)
    list(LENGTH FILTERED_HEADERS HEADER_COUNT)
    message(
      STATUS
        "Detected ${HEADER_COUNT} common headers for PCH (min count: ${ARG_MIN_COUNT})"
    )
  else()
    message(
      WARNING
        "No common headers detected for PCH with min count ${ARG_MIN_COUNT}")
  endif()
endfunction()

# ##############################################################################
# use_precompiled_header
#
# Applies precompiled headers to a target using a global cache of detected
# headers. This function automatically handles PCH reuse across targets:
# - First call: detects common headers and creates PCH for the first target
# - Subsequent calls: reuse PCH from compatible targets (same variant)
#
# The function maintains separate PCH for shared and static library variants
# because they have different compile definitions.
#
# Parameters:
#   TARGET          - The CMake target to apply PCH to
#
# Global variables used (set by detect_common_headers or earlier calls):
#   DFTRACER_UTILS_ENABLE_PCH     - Enable/disable PCH globally
#   DFTRACER_UTILS_PCH_HEADERS    - Cached list of detected headers
#   DFTRACER_UTILS_PCH_SOURCE_DIR - Source directory for header detection
#   DFTRACER_UTILS_PCH_MIN_COUNT  - Minimum occurrence count (default: 3)
#
# Examples:
#   # Simple usage - handles everything automatically
#   use_precompiled_header(my_target_shared)
#   use_precompiled_header(my_target_static)
#   use_precompiled_header(another_target_shared)  # Reuses from my_target_shared
# ##############################################################################
function(use_precompiled_header TARGET)
  # Check if precompiled headers are enabled globally
  if(NOT DFTRACER_UTILS_ENABLE_PCH)
    return()
  endif()

  # Check if target exists
  if(NOT TARGET ${TARGET})
    message(FATAL_ERROR "Target ${TARGET} does not exist")
  endif()

  # Determine if this is a shared or static library variant
  get_target_property(TARGET_TYPE ${TARGET} TYPE)
  set(VARIANT_KEY "")

  if(TARGET_TYPE STREQUAL "SHARED_LIBRARY")
    set(VARIANT_KEY "SHARED")
  elseif(TARGET_TYPE STREQUAL "STATIC_LIBRARY")
    set(VARIANT_KEY "STATIC")
  elseif(TARGET_TYPE STREQUAL "EXECUTABLE")
    set(VARIANT_KEY "EXECUTABLE")
  else()
    set(VARIANT_KEY "OTHER")
  endif()

  message(DEBUG "PCH: Target ${TARGET} has type ${TARGET_TYPE}, variant key: ${VARIANT_KEY}")

  # Get or detect headers if not already cached
  get_property(
    PCH_HEADERS GLOBAL
    PROPERTY DFTRACER_UTILS_PCH_HEADERS_CACHED)

  if(NOT PCH_HEADERS)
    # Detect headers from source directory
    if(NOT DFTRACER_UTILS_PCH_SOURCE_DIR)
      message(
        FATAL_ERROR
          "DFTRACER_UTILS_PCH_SOURCE_DIR must be set before calling use_precompiled_header"
      )
    endif()

    if(NOT DFTRACER_UTILS_PCH_MIN_COUNT)
      set(DFTRACER_UTILS_PCH_MIN_COUNT 3)
    endif()

    detect_common_headers(
      SOURCE_DIR "${DFTRACER_UTILS_PCH_SOURCE_DIR}"
      OUTPUT_VAR PCH_HEADERS
      MIN_COUNT ${DFTRACER_UTILS_PCH_MIN_COUNT})

    if(NOT PCH_HEADERS)
      message(
        WARNING
          "No common headers detected for PCH. PCH will not be applied.")
      return()
    endif()

    # Cache the detected headers globally
    set_property(GLOBAL PROPERTY DFTRACER_UTILS_PCH_HEADERS_CACHED
                                  "${PCH_HEADERS}")

    list(LENGTH PCH_HEADERS HEADER_COUNT)
    message(STATUS "Detected ${HEADER_COUNT} common headers for PCH")
  endif()

  # Apply PCH to this target (each target gets its own PCH to avoid compile definition issues)
  target_precompile_headers(${TARGET} PRIVATE ${PCH_HEADERS})
  message(STATUS "Target ${TARGET}: Applied PCH (${VARIANT_KEY})")
endfunction()

# ##############################################################################
# Helper function to check if PCH is supported
# ##############################################################################
function(check_pch_support OUTPUT_VAR)
  if(CMAKE_VERSION VERSION_LESS 3.16)
    set(${OUTPUT_VAR}
        FALSE
        PARENT_SCOPE)
    message(
      WARNING
        "Precompiled headers require CMake 3.16 or later. Current version: ${CMAKE_VERSION}"
    )
  else()
    set(${OUTPUT_VAR}
        TRUE
        PARENT_SCOPE)
  endif()
endfunction()
