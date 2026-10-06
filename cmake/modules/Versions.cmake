# Two kinds of version. The release version names a tagged state of the repo:
# setuptools_scm hands it to scikit-build-core as SKBUILD_PROJECT_VERSION, and a
# plain CMake build asks git. The interface versions (api, abi, index, duql) are
# set by hand in the VERSION file and change only when that interface changes.
# duql is MAJOR.MINOR: a query that declares a version needs the same major and
# a minor no greater than the engine's.

# Sets DFTRACER_UTILS_VERSION and its _MAJOR, _MINOR and _PATCH parts.
macro(dftracer_utils_release_version)
  set(_release "")
  if(DEFINED SKBUILD_PROJECT_VERSION)
    set(_release "${SKBUILD_PROJECT_VERSION}")
  else()
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*"
                --abbrev=0
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _release
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
  endif()
  if(_release MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(DFTRACER_UTILS_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(DFTRACER_UTILS_VERSION_MINOR ${CMAKE_MATCH_2})
    set(DFTRACER_UTILS_VERSION_PATCH ${CMAKE_MATCH_3})
  else()
    # A source tree with no tag and no scikit-build version.
    set(DFTRACER_UTILS_VERSION_MAJOR 0)
    set(DFTRACER_UTILS_VERSION_MINOR 0)
    set(DFTRACER_UTILS_VERSION_PATCH 0)
  endif()
  set(DFTRACER_UTILS_VERSION
      "${DFTRACER_UTILS_VERSION_MAJOR}.${DFTRACER_UTILS_VERSION_MINOR}.${DFTRACER_UTILS_VERSION_PATCH}"
  )
endmacro()

function(_dftracer_utils_version_line file key pattern out)
  file(STRINGS "${file}" _line REGEX "^${key}:")
  list(LENGTH _line _count)
  if(NOT _count EQUAL 1)
    message(FATAL_ERROR "${file}: expected one '${key}:' line")
  endif()
  string(REGEX REPLACE "^${key}:[ \t]*" "" _value "${_line}")
  string(STRIP "${_value}" _value)
  if(NOT _value MATCHES "^${pattern}$")
    message(FATAL_ERROR "${file}: '${key}: ${_value}' does not match ${pattern}")
  endif()
  set(${out}
      "${_value}"
      PARENT_SCOPE)
endfunction()

# Reads VERSION and writes the interface versions into two generated headers:
# core/common/versions.h for the library and plugins/abi_version.h, which the
# plain-C plugin ABI includes.
function(dftracer_utils_interface_versions version_file out_include_dir)
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${version_file}")
  set(_xyz "[0-9]+\\.[0-9]+\\.[0-9]+")
  _dftracer_utils_version_line("${version_file}" api "${_xyz}" _api)
  _dftracer_utils_version_line("${version_file}" abi "${_xyz}" _abi)
  _dftracer_utils_version_line("${version_file}" index "[0-9]+" _index)
  _dftracer_utils_version_line("${version_file}" duql "[0-9]+\\.[0-9]+" _duql)

  string(REPLACE "." ";" _api_parts "${_api}")
  list(GET _api_parts 0 _api_major)
  list(GET _api_parts 1 _api_minor)
  list(GET _api_parts 2 _api_patch)
  string(REPLACE "." ";" _duql_parts "${_duql}")
  list(GET _duql_parts 0 _duql_major)
  list(GET _duql_parts 1 _duql_minor)
  string(REPLACE "." ";" _abi_parts "${_abi}")
  list(GET _abi_parts 0 _abi_major)
  list(GET _abi_parts 1 _abi_minor)
  list(GET _abi_parts 2 _abi_patch)
  # The plugin ABI version is packed into the uint32_t abi_version field.
  if(_abi_major GREATER 65535
     OR _abi_minor GREATER 255
     OR _abi_patch GREATER 255)
    message(
      FATAL_ERROR
        "${version_file}: abi ${_abi} does not fit major<65536, minor<256, patch<256"
    )
  endif()
  math(EXPR _abi_packed
       "(${_abi_major} << 16) | (${_abi_minor} << 8) | ${_abi_patch}"
       OUTPUT_FORMAT HEXADECIMAL)

  file(
    WRITE "${out_include_dir}/dftracer/utils/plugins/abi_version.h.tmp"
    "// Generated from VERSION by cmake/modules/Versions.cmake. Do not edit.
#ifndef DFTRACER_UTILS_PLUGINS_ABI_VERSION_H
#define DFTRACER_UTILS_PLUGINS_ABI_VERSION_H

#define DFTRACER_UTILS_PLUGIN_ABI_VERSION_MAJOR ${_abi_major}
#define DFTRACER_UTILS_PLUGIN_ABI_VERSION_MINOR ${_abi_minor}
#define DFTRACER_UTILS_PLUGIN_ABI_VERSION_PATCH ${_abi_patch}
#define DFTRACER_UTILS_PLUGIN_ABI_VERSION ${_abi_packed}u

#endif  // DFTRACER_UTILS_PLUGINS_ABI_VERSION_H
")
  file(
    WRITE "${out_include_dir}/dftracer/utils/core/common/versions.h.tmp"
    "// Generated from VERSION by cmake/modules/Versions.cmake. Do not edit.
#ifndef DFTRACER_UTILS_CORE_COMMON_VERSIONS_H
#define DFTRACER_UTILS_CORE_COMMON_VERSIONS_H

#include <dftracer/utils/plugins/abi_version.h>

#define DFTRACER_UTILS_API_VERSION_MAJOR ${_api_major}
#define DFTRACER_UTILS_API_VERSION_MINOR ${_api_minor}
#define DFTRACER_UTILS_API_VERSION_PATCH ${_api_patch}
#define DFTRACER_UTILS_API_VERSION_STRING \"${_api}\"
#define DFTRACER_UTILS_INDEX_FORMAT_VERSION ${_index}
#define DFTRACER_UTILS_DUQL_VERSION_MAJOR ${_duql_major}
#define DFTRACER_UTILS_DUQL_VERSION_MINOR ${_duql_minor}
#define DFTRACER_UTILS_DUQL_VERSION_STRING \"${_duql}\"

#endif  // DFTRACER_UTILS_CORE_COMMON_VERSIONS_H
")
  # Copy only on change, so an unchanged VERSION rebuilds nothing.
  foreach(_h plugins/abi_version.h core/common/versions.h)
    configure_file("${out_include_dir}/dftracer/utils/${_h}.tmp"
                   "${out_include_dir}/dftracer/utils/${_h}" COPYONLY)
    file(REMOVE "${out_include_dir}/dftracer/utils/${_h}.tmp")
  endforeach()
  dftracer_utils_item("Interface      "
                      "api=${_api} abi=${_abi} index=${_index} duql=${_duql}")
endfunction()
