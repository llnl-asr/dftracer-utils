# SPDX-License-Identifier: MIT
# Runs native_probe under the launcher and checks what the job did: every rank
# printed its line with ok, the context named the pending native requests, and
# the job ended with a non-zero status before the timeout, which is the
# MPI_Abort path.
execute_process(
  COMMAND ${MPIEXEC} ${NUMPROC_FLAG} ${NP} ${PREFLAGS} ${PROBE} ${MODE}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  TIMEOUT 100)
message(STATUS "exit status: ${rc}")
message(STATUS "stdout:\n${out}")
message(STATUS "stderr:\n${err}")
if(rc EQUAL 0)
  message(FATAL_ERROR "the job exited with status 0; MPI_Abort did not end it")
endif()
if(rc STREQUAL "Process terminated due to timeout")
  message(FATAL_ERROR "the job hung until the timeout")
endif()
math(EXPR last "${NP} - 1")
foreach(rank RANGE 0 ${last})
  if(NOT out MATCHES "PROBE rank=${rank} ok ")
    message(FATAL_ERROR "rank ${rank} did not report ok")
  endif()
endforeach()
if(out MATCHES "PROBE rank=[0-9]+ bad ")
  message(FATAL_ERROR "a rank reported bad")
endif()
if(NOT "${out}${err}" MATCHES "still pending at shutdown")
  message(FATAL_ERROR "no line named the native requests that ended the job")
endif()
