# Check the layering of runtime: a file includes only its own part or a part
# of lower rank. Equal ranks do not include each other. There is no exception
# and no allow list. Ranks: executor, mpi, distributed 0; coro 1; io, tasks 2;
# task_graph, scheduler 3; top level 4; pipeline 5.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_runtime_layers.cmake
#
# With -DPLANT_DIR=<dir> the script copies the runtime tree into <dir>, plants
# one upward include there and passes only when the check fails on the copy.

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "check_runtime_layers.cmake requires -DSOURCE_DIR=...")
endif()

set(RANK_executor 0)
set(RANK_mpi 0)
set(RANK_distributed 0)
set(RANK_coro 1)
set(RANK_io 2)
set(RANK_tasks 2)
set(RANK_task_graph 3)
set(RANK_scheduler 3)
set(RANK_top 4)
set(RANK_pipeline 5)

if(DEFINED PLANT_DIR)
  set(planted_edge "runtime/executor/executor.h|runtime/pipeline/pipeline.h")
  file(REMOVE_RECURSE "${PLANT_DIR}")
  file(MAKE_DIRECTORY "${PLANT_DIR}/include/dftracer/utils"
       "${PLANT_DIR}/src/dftracer/utils")
  file(COPY "${SOURCE_DIR}/include/dftracer/utils/runtime"
       DESTINATION "${PLANT_DIR}/include/dftracer/utils")
  file(COPY "${SOURCE_DIR}/src/dftracer/utils/runtime"
       DESTINATION "${PLANT_DIR}/src/dftracer/utils")
  file(APPEND "${PLANT_DIR}/include/dftracer/utils/runtime/executor/executor.h"
       "#include <dftracer/utils/runtime/pipeline/pipeline.h>\n")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PLANT_DIR} -P
            ${CMAKE_CURRENT_LIST_FILE}
    RESULT_VARIABLE planted_result
    OUTPUT_VARIABLE planted_out
    ERROR_VARIABLE planted_out)
  file(REMOVE_RECURSE "${PLANT_DIR}")
  string(FIND "${planted_out}" "${planted_edge}" at)
  if(planted_result EQUAL 0 OR at EQUAL -1)
    message(FATAL_ERROR
            "runtime layering check missed the planted edge ${planted_edge}:\n${planted_out}")
  endif()
  message(STATUS "runtime layering check rejects the planted edge")
  return()
endif()

function(part_of rel out)
  if(rel MATCHES "^runtime/([^/]+)/")
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  else()
    set(${out} "top" PARENT_SCOPE)
  endif()
endfunction()

file(GLOB_RECURSE runtime_files
     "${SOURCE_DIR}/include/dftracer/utils/runtime/*"
     "${SOURCE_DIR}/src/dftracer/utils/runtime/*")
list(SORT runtime_files)

set(errors "")
foreach(path ${runtime_files})
  string(REGEX REPLACE "^.*/dftracer/utils/" "" rel "${path}")
  part_of("${rel}" src_part)
  if(NOT DEFINED RANK_${src_part})
    string(APPEND errors "  unranked part: ${rel}\n")
    continue()
  endif()
  file(STRINGS "${path}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*<dftracer/utils/runtime/")
  foreach(line ${includes})
    string(REGEX REPLACE "^.*<dftracer/utils/(runtime/[^>]+)>.*$" "\\1" inc "${line}")
    part_of("${inc}" dst_part)
    if(dst_part STREQUAL src_part)
      continue()
    endif()
    if(NOT DEFINED RANK_${dst_part})
      string(APPEND errors "  unranked part: ${rel} includes ${inc}\n")
    elseif(NOT RANK_${dst_part} LESS RANK_${src_part})
      string(APPEND errors
             "  ${rel}|${inc} (${src_part} rank ${RANK_${src_part}} includes ${dst_part} rank ${RANK_${dst_part}})\n")
    endif()
  endforeach()
endforeach()

if(errors)
  message(FATAL_ERROR "runtime layering violations:\n${errors}")
endif()
message(STATUS "runtime layering ok")
