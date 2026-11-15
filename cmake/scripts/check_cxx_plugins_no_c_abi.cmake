# SPDX-License-Identifier: MIT
# Fails when a C++ plugin authored against the plugin SDK writes a dftu_* type,
# a dftu_* call or a DFTU_* constant. The SDK translates to the C ABI inside, so
# a C++ author names only C++ entities; the entry macro DFTU_PLUGIN( is the one
# DFTU_ name a plugin file spells. A C plugin (*.c) and a fixture that exercises
# the C ABI on purpose stay raw; each of those is listed below with its reason.
#
# Checked: examples/plugins/*.cpp, src/dftracer/utils/cli/plugin_templates/*/
# plugin.cpp and tests/plugins/*_plugin.cpp.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_cxx_plugins_no_c_abi.cmake
#        cmake -DSOURCE_DIR=<repo root> -DSELF_TEST=ON -P check_cxx_plugins_no_c_abi.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "check_cxx_plugins_no_c_abi.cmake requires -DSOURCE_DIR=...")
endif()

# Relative to the repository root. Each fixture here drives the C ABI itself,
# so what it asserts is the raw handle, not the SDK's spelling of it.
set(ALLOW_FILES
    # Registers through the raw dftu_svc_ops struct and returns a hand-built
    # descriptor with no fold; that plugin shape and its borrowed column are the
    # subject.
    tests/plugins/borrowed_frame_plugin.cpp
    # Hands the host malformed and wrongly-sized C frame and column handles; the
    # host frame checks are the subject.
    tests/plugins/broken_frame_plugin.cpp
    # Registers providers through the raw dftu_svc_providers vtable and asserts
    # which names the host refuses.
    tests/plugins/provider_fixture_plugin.cpp
    # Registers a raw provider vtable to feed a raw dftu_dataframe into the node
    # under test.
    tests/plugins/sessionize_input_plugin.cpp)

file(GLOB examples "${SOURCE_DIR}/examples/plugins/*.cpp")
file(GLOB templates "${SOURCE_DIR}/src/dftracer/utils/cli/plugin_templates/*/plugin.cpp")
file(GLOB fixtures "${SOURCE_DIR}/tests/plugins/*_plugin.cpp")
set(files ${examples} ${templates} ${fixtures})
list(REMOVE_DUPLICATES files)
list(SORT files)

set(allows_self_test FALSE)
if(SELF_TEST)
  set(allows_self_test TRUE)
endif()

function(_check_file file out_hits)
  set(hits "")
  if(NOT allows_self_test)
    foreach(allowed IN LISTS ALLOW_FILES)
      string(FIND "${file}" "${SOURCE_DIR}/${allowed}" at)
      if(at EQUAL 0)
        return()
      endif()
    endforeach()
  endif()
  file(READ "${file}" text)
  # The entry macro is the one DFTU_ name a plugin file spells. A name past it
  # is a call of the SDK's own DFTU_PLUGIN_INFO or DFTU_PLUGIN_EXPORT, which no
  # C++ plugin writes.
  set(kept "")
  set(rest "${text}")
  while(TRUE)
    string(FIND "${rest}" "DFTU_PLUGIN" at)
    if(at EQUAL -1)
      string(APPEND kept "${rest}")
      break()
    endif()
    math(EXPR after "${at} + 11")
    string(SUBSTRING "${rest}" 0 ${at} head)
    string(SUBSTRING "${rest}" ${after} -1 tail)
    string(REGEX MATCH "^[A-Za-z0-9_]" next "${tail}")
    if(next STREQUAL "")
      string(APPEND kept "${head}")
    else()
      string(APPEND kept "${head}DFTU_PLUGIN")
    endif()
    set(rest "${tail}")
  endwhile()
  set(text "${kept}")
  set(found "")
  foreach(prefix IN ITEMS "dftu_" "DFTU_")
    set(rest "${text}")
    while(TRUE)
      string(FIND "${rest}" "${prefix}" at)
      if(at EQUAL -1)
        break()
      endif()
      string(LENGTH "${prefix}" prefix_len)
      math(EXPR after "${at} + ${prefix_len}")
      string(SUBSTRING "${rest}" ${after} 200 tail)
      string(REGEX MATCH "^[A-Za-z0-9_]*" suffix "${tail}")
      list(APPEND found "${prefix}${suffix}")
      math(EXPR next "${at} + 1")
      string(SUBSTRING "${rest}" ${next} -1 rest)
    endwhile()
  endforeach()
  if(found)
    list(REMOVE_DUPLICATES found)
    list(SORT found)
    string(REPLACE "${SOURCE_DIR}/" "" relative "${file}")
    foreach(name IN LISTS found)
      list(APPEND hits "${relative}: ${name}")
    endforeach()
  endif()
  set(${out_hits} "${hits}" PARENT_SCOPE)
endfunction()

set(hits "")
foreach(file IN LISTS files)
  _check_file("${file}" file_hits)
  list(APPEND hits ${file_hits})
endforeach()

if(hits)
  list(REMOVE_DUPLICATES hits)
  list(SORT hits)
  list(LENGTH hits count)
  message(
    "a C++ plugin writes the C ABI: ${count} name(s). A C++ plugin uses the SDK\n"
    "(DFTU_PLUGIN, PluginBuilder, dstd::Condition, dstd::CStringView) and never a\n"
    "dftu_* type, call or constant:")
  foreach(hit IN LISTS hits)
    message("  ${hit}")
  endforeach()
  message(FATAL_ERROR "cxx_plugins_no_c_abi: ${count} hit(s)")
endif()

message(STATUS "no C++ plugin writes the C ABI (${files})")
