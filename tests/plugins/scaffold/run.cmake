# SPDX-License-Identifier: MIT
# For every kind `dftracer_plugin new` knows: write the scaffold, build it
# against the installed tree alone, then run `dftracer_plugin check` on it with
# the golden test the scaffold comes with. A kind whose scaffold does not
# compile, or whose own test does not pass, fails this script.
file(REMOVE_RECURSE "${WORK_DIR}")

foreach(kind fold udf aggregate format sink node)
  set(name "scaf_${kind}")
  set(source "${WORK_DIR}/src/${name}")
  set(build "${WORK_DIR}/build_${kind}")

  execute_process(
    COMMAND "${TOOL}" new "${kind}" "${name}" --dir "${WORK_DIR}/src"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out)
  if(rc)
    message(FATAL_ERROR "new ${kind} ${name} failed: ${rc}\n${out}")
  endif()

  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -S "${source}" -B "${build}" -G "${GENERATOR}"
      "-DCMAKE_PREFIX_PATH=${PREFIX}" "-DCMAKE_C_COMPILER=${C_COMPILER}"
      "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
      "-DCMAKE_EXE_LINKER_FLAGS=${LINKER_FLAGS}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out)
  if(rc)
    message(FATAL_ERROR "the ${kind} scaffold does not configure: ${rc}\n${out}")
  endif()

  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(rc)
    message(FATAL_ERROR "the ${kind} scaffold does not build: ${rc}\n${out}")
  endif()

  # The scaffold makes a MODULE library with PREFIX "" and the default module
  # suffix, so its one file name is build/<name>.so, as its README says.
  set(library "${build}/${name}.so")
  if(NOT EXISTS "${library}")
    message(FATAL_ERROR "the ${kind} scaffold built no ${library}")
  endif()

  execute_process(
    COMMAND "${TOOL}" check "${library}" --tests "${source}/tests"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out)
  if(rc)
    message(FATAL_ERROR "check failed for the ${kind} scaffold: ${rc}\n${out}")
  endif()
  message(STATUS "${kind}: the scaffold builds and its golden test passes")
endforeach()
