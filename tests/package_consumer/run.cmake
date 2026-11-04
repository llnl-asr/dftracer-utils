# SPDX-License-Identifier: MIT
# Configures, builds and runs the consumer project against the installed tree.
# VARIANTS lists the variants this build installed; each must give both programs.
file(REMOVE_RECURSE "${WORK_DIR}")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${WORK_DIR}" -G "${GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${PREFIX}" "-DCMAKE_C_COMPILER=${C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=${LINKER_FLAGS}"
  RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "the consumer project does not configure: ${rc}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK_DIR}"
                RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "the consumer project does not build: ${rc}")
endif()
foreach(variant ${VARIANTS})
  foreach(program filter_consumer_${variant} bundle_consumer_${variant})
    if(NOT EXISTS "${WORK_DIR}/${program}")
      message(FATAL_ERROR "the installed packages gave no ${program}")
    endif()
    execute_process(COMMAND "${WORK_DIR}/${program}" RESULT_VARIABLE rc)
    if(rc)
      message(FATAL_ERROR "${program} failed: ${rc}")
    endif()
  endforeach()
endforeach()
