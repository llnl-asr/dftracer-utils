# Check that only the aggregation tier's store (agg/agg_store.cpp) reads or
# writes the aggregation and system_metrics column families. The index store
# may name them where it defines them: the family names, the column family
# options with the merge operators, and the SST writer's merge setup.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_agg_families.cmake

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "check_agg_families.cmake requires -DSOURCE_DIR=...")
endif()

set(ALLOWED
    src/dftracer/utils/index/schemas/dft/agg/agg_store.cpp
    src/dftracer/utils/index/store/layout.cpp
    src/dftracer/utils/index/store/index_database.cpp
    src/dftracer/utils/index/store/index_database_sst_writer_context.cpp)

file(GLOB_RECURSE sources "${SOURCE_DIR}/src/*.h" "${SOURCE_DIR}/src/*.cpp"
     "${SOURCE_DIR}/include/*.h")
list(SORT sources)

set(errors "")
foreach(path ${sources})
  file(RELATIVE_PATH rel "${SOURCE_DIR}" "${path}")
  list(FIND ALLOWED "${rel}" idx)
  if(NOT idx EQUAL -1)
    continue()
  endif()
  file(STRINGS "${path}" hits
       REGEX "(cf|Family)::(AGGREGATION|SYSTEM_METRICS)")
  foreach(line ${hits})
    string(STRIP "${line}" line)
    if(NOT line MATCHES "^//")
      string(APPEND errors "  ${rel}: ${line}\n")
    endif()
  endforeach()
endforeach()

if(errors)
  message(FATAL_ERROR
          "aggregation families named outside agg/agg_store.cpp:\n${errors}")
endif()
message(STATUS "aggregation families ok")
