# ##############################################################################
# PrecompiledHeader.cmake
#
# use_precompiled_header(<target>) builds a precompiled header for one target
# from the includes of that target's own sources:
#
# * a standard library header (no "/" and no ".h") is kept when at least
#   MIN_COUNT sources include it
# * a third-party header is kept under the same count only when the target
#   links the library that provides it (DFTU_PCH_THIRD_PARTY below)
# * a header of this project is never kept
#
# MIN_COUNT overrides DFTRACER_UTILS_PCH_MIN_COUNT for one target. The default
# of 2 keeps nothing for a target with a handful of sources.
# DFTRACER_UTILS_ENABLE_PCH=OFF turns it off. Call it after the target has
# its link libraries, because the header list reads them.
# ##############################################################################

include_guard(GLOBAL)

# Header regex and the regex of the link library that provides it. Highway has
# no row: foreach_target.h must precede highway.h, so a precompiled highway.h
# breaks every multi-target kernel.
set(DFTU_PCH_THIRD_PARTY
    "^simdjson\\.h$|simdjson"
    "^concurrentqueue|concurrentqueue"
    "^ankerl/|unordered_dense"
    "^tl/expected|tl_expected|tl::expected")

# Collects the names of every library that TARGET links, directly or through
# the link interface of a library it links.
function(dftu_pch_link_closure TARGET OUTPUT_VAR)
  set(QUEUE ${TARGET})
  set(RESULT "")
  while(QUEUE)
    list(POP_FRONT QUEUE CURRENT)
    if(DFTU_PCH_SEEN_${CURRENT})
      continue()
    endif()
    set(DFTU_PCH_SEEN_${CURRENT} TRUE)
    list(APPEND RESULT "${CURRENT}")
    if(NOT TARGET ${CURRENT})
      continue()
    endif()
    get_target_property(ALIASED ${CURRENT} ALIASED_TARGET)
    if(ALIASED)
      list(APPEND QUEUE "${ALIASED}")
      continue()
    endif()
    get_target_property(TARGET_TYPE ${CURRENT} TYPE)
    set(LINKED "")
    if(NOT TARGET_TYPE STREQUAL "INTERFACE_LIBRARY")
      get_target_property(DIRECT ${CURRENT} LINK_LIBRARIES)
      if(DIRECT)
        list(APPEND LINKED ${DIRECT})
      endif()
    endif()
    get_target_property(INTERFACE_LINKED ${CURRENT} INTERFACE_LINK_LIBRARIES)
    if(INTERFACE_LINKED)
      list(APPEND LINKED ${INTERFACE_LINKED})
    endif()
    foreach(LIB ${LINKED})
      string(REGEX REPLACE "\\$<(BUILD_INTERFACE|LINK_ONLY):([^>]*)>" "\\2" LIB
                           "${LIB}")
      if(LIB AND NOT LIB MATCHES "\\$<")
        list(APPEND QUEUE "${LIB}")
      endif()
    endforeach()
  endwhile()
  set(${OUTPUT_VAR}
      "${RESULT}"
      PARENT_SCOPE)
endfunction()

function(use_precompiled_header TARGET)
  cmake_parse_arguments(ARG "" "MIN_COUNT" "" ${ARGN})
  if(NOT DFTRACER_UTILS_ENABLE_PCH)
    return()
  endif()

  if(NOT TARGET ${TARGET})
    message(FATAL_ERROR "Target ${TARGET} does not exist")
  endif()

  if(ARG_MIN_COUNT)
    set(MIN_COUNT ${ARG_MIN_COUNT})
  elseif(DFTRACER_UTILS_PCH_MIN_COUNT)
    set(MIN_COUNT ${DFTRACER_UTILS_PCH_MIN_COUNT})
  else()
    set(MIN_COUNT 2)
  endif()

  get_target_property(TARGET_SOURCES ${TARGET} SOURCES)
  get_target_property(TARGET_SOURCE_DIR ${TARGET} SOURCE_DIR)
  if(NOT TARGET_SOURCES)
    message(WARNING "PCH: ${TARGET} has no sources. PCH will not be applied.")
    return()
  endif()

  dftu_pch_link_closure(${TARGET} LINKED_LIBS)

  set(HEADERS "")
  foreach(SOURCE ${TARGET_SOURCES})
    if(SOURCE MATCHES "\\$<" OR NOT SOURCE MATCHES "\\.(cpp|cc|cxx)$")
      continue()
    endif()
    cmake_path(ABSOLUTE_PATH SOURCE BASE_DIRECTORY "${TARGET_SOURCE_DIR}"
               NORMALIZE)
    if(NOT EXISTS "${SOURCE}")
      continue()
    endif()
    file(STRINGS "${SOURCE}" INCLUDE_LINES
         REGEX "^[ \t]*#[ \t]*include[ \t]*<[^>]+>")
    set(SEEN "")
    foreach(LINE ${INCLUDE_LINES})
      string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*<([^>]+)>.*$" "\\1"
                           HEADER "${LINE}")
      list(APPEND SEEN "${HEADER}")
    endforeach()
    list(REMOVE_DUPLICATES SEEN)
    foreach(HEADER ${SEEN})
      if(NOT DEFINED DFTU_PCH_COUNT_${HEADER})
        list(APPEND HEADERS "${HEADER}")
        set(DFTU_PCH_COUNT_${HEADER} 0)
      endif()
      math(EXPR DFTU_PCH_COUNT_${HEADER} "${DFTU_PCH_COUNT_${HEADER}} + 1")
    endforeach()
  endforeach()

  set(ENTRIES "")
  foreach(HEADER ${HEADERS})
    set(COUNT ${DFTU_PCH_COUNT_${HEADER}})
    unset(DFTU_PCH_COUNT_${HEADER})
    if(COUNT LESS MIN_COUNT)
      continue()
    endif()
    if(HEADER MATCHES "^dftracer/")
      continue()
    endif()
    if(NOT HEADER MATCHES "/" AND NOT HEADER MATCHES "\\.h")
      set(KEEP TRUE)
    else()
      set(KEEP FALSE)
      foreach(RULE ${DFTU_PCH_THIRD_PARTY})
        string(REPLACE "|" ";" RULE_PARTS "${RULE}")
        list(POP_FRONT RULE_PARTS HEADER_REGEX)
        if(HEADER MATCHES "${HEADER_REGEX}")
          foreach(LIB ${LINKED_LIBS})
            foreach(LIB_REGEX ${RULE_PARTS})
              if(LIB MATCHES "${LIB_REGEX}")
                set(KEEP TRUE)
              endif()
            endforeach()
          endforeach()
        endif()
      endforeach()
    endif()
    if(KEEP)
      string(LENGTH "${COUNT}" COUNT_LEN)
      math(EXPR PAD_LEN "5 - ${COUNT_LEN}")
      string(REPEAT "0" ${PAD_LEN} PAD)
      list(APPEND ENTRIES "${PAD}${COUNT}:${HEADER}")
    endif()
  endforeach()
  list(SORT ENTRIES ORDER DESCENDING)

  set(PCH_HEADERS "")
  foreach(ENTRY ${ENTRIES})
    string(REGEX REPLACE "^[0-9]+:" "" HEADER "${ENTRY}")
    list(APPEND PCH_HEADERS "<${HEADER}>")
  endforeach()

  if(NOT PCH_HEADERS)
    message(WARNING "PCH: no common headers for ${TARGET}. PCH not applied.")
    return()
  endif()

  list(LENGTH PCH_HEADERS HEADER_COUNT)
  string(REPLACE ";" " " HEADER_TEXT "${PCH_HEADERS}")
  message(STATUS "PCH ${TARGET}: ${HEADER_COUNT} headers: ${HEADER_TEXT}")

  target_precompile_headers(${TARGET} PRIVATE ${PCH_HEADERS})

  # Clang should ignore pch timestamp
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${TARGET} PRIVATE -Xclang -fno-pch-timestamp)
  endif()
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
