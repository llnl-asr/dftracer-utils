# SPDX-License-Identifier: MIT
# Compares what the plugin ABI manifest program prints with the recorded
# manifest, cmake/plugin_abi_manifest.txt.
#
# Usage:
#   cmake -DBUILD_DIR=<build> -DTARGET=plugin_abi_manifest_gen -DBINARY=<exe>
#         -DMANIFEST=<repo>/cmake/plugin_abi_manifest.txt -DWORK_DIR=<dir>
#         -P check_plugin_abi_manifest.cmake
#   add -DREGENERATE=ON to write the manifest instead of comparing.
#
# -DBUILD_DIR and -DTARGET rebuild the program first, so an edited header is
# seen without a full build. They are optional.

if(NOT DEFINED BINARY OR NOT DEFINED MANIFEST)
  message(FATAL_ERROR "check_plugin_abi_manifest.cmake requires -DBINARY=... -DMANIFEST=...")
endif()

if(DEFINED BUILD_DIR AND DEFINED TARGET)
  execute_process(COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} --target
                          ${TARGET}
                  RESULT_VARIABLE build_result OUTPUT_VARIABLE build_output
                  ERROR_VARIABLE build_output)
  if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "plugin_abi_manifest: building ${TARGET} failed:\n${build_output}")
  endif()
endif()

execute_process(COMMAND ${BINARY}
                RESULT_VARIABLE run_result OUTPUT_VARIABLE actual
                ERROR_VARIABLE run_output)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "plugin_abi_manifest: ${BINARY} failed:\n${run_output}")
endif()

if(REGENERATE)
  file(WRITE ${MANIFEST} "${actual}")
  message(STATUS "plugin_abi_manifest: wrote ${MANIFEST}")
elseif(NOT EXISTS ${MANIFEST})
  message(
    FATAL_ERROR
      "plugin_abi_manifest: no ${MANIFEST}. Record it with `cmake --build <build dir> --target plugin_abi_manifest_update`."
  )
else()
  file(READ ${MANIFEST} expected)
  if(actual STREQUAL expected)
    message(STATUS "plugin_abi_manifest: ok")
  else()
    if(NOT DEFINED WORK_DIR)
      set(WORK_DIR ${CMAKE_CURRENT_BINARY_DIR})
    endif()
    set(actual_file ${WORK_DIR}/plugin_abi_manifest.actual.txt)
    file(WRITE ${actual_file} "${actual}")
    find_program(DFTRACER_UTILS_DIFF diff)
    if(DFTRACER_UTILS_DIFF)
      execute_process(COMMAND ${DFTRACER_UTILS_DIFF} -u ${MANIFEST}
                              ${actual_file} OUTPUT_VARIABLE diff_output
                      ERROR_VARIABLE diff_output)
    else()
      set(diff_output "recorded:\n${expected}\nactual:\n${actual}")
    endif()
    message(
      FATAL_ERROR
        "plugin_abi_manifest: cmake/plugin_abi_manifest.txt does not match the plugin ABI headers ('-' is the recorded manifest, '+' is the headers now).\n"
        "${diff_output}\n"
        "A change to the plugin ABI must record the new manifest in the same commit:\n"
        "  cmake --build <build dir> --target plugin_abi_manifest_update\n"
        "and, if a line was removed or changed, raise the abi major in VERSION.\n"
        "Actual output kept in ${actual_file}")
  endif()
endif()
