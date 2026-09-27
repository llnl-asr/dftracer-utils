# Check the index module's layering: a file in a lower layer must not include
# a higher layer or the query engine (trace/views), except for the edges in
# ALLOWED, each of which names the plan stage that removes it. An ALLOWED edge
# that no longer exists also fails, so fixes cannot leave stale entries. See
# docs/plans/2026-09-24-extensible-index.md.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_index_layers.cmake

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "check_index_layers.cmake requires -DSOURCE_DIR=...")
endif()

# Rank per layer; build, plan and cache are peers.
set(LAYERS store gzip extensions schemas build plan cache)
set(RANK_store 0)
set(RANK_gzip 1)
set(RANK_extensions 2)
set(RANK_schemas 3)
set(RANK_build 4)
set(RANK_plan 4)
set(RANK_cache 4)

# Modules below the index; the trace format headers move into the dftracer
# decoder in stage 5. plugins/abi/ is the plugin C ABI: declarations only.
set(BASE_PREFIXES
    core/ json/ dataframe/ plugins/abi/ duql/ utilities/common/ utilities/fileio/
    utilities/filesystem/ utilities/hash/ trace/args_map.h trace/event.h
    trace/internal/ trace/parse_inflated.h trace/schema.h)

set(ALLOWED
  # stage 4 (index core v2) replaces the fixed storage layout
  "index/build/chunk_indexer.cpp|utilities/reader/internal/stream_config.h"
  "index/build/index_fold_driver.h|trace/views/fold.h"
  "index/build/index_fold_driver.h|trace/views/fold_event.h"
  "index/extensions/bloom_fold.cpp|index/build/index_write_lock.h"
  "index/extensions/bloom_fold.h|index/schemas/dft/bloom_core.h"
  "index/extensions/bloom_fold.h|trace/views/fold.h"
  "index/extensions/rowset_fold.cpp|trace/views/event_source.h"
  "index/extensions/rowset_fold.h|trace/views/fold.h"
  "index/gzip/gzip_indexer.cpp|index/build/index_visitor.h"
  "index/gzip/gzip_indexer.h|index/build/index_visitor.h"
  "index/gzip/checkpoint_indexer.h|index/build/index_visitor.h"
  "index/schemas/dft/bloom_core.h|index/build/chunk_indexer.h"
  "index/store/index_database.cpp|index/schemas/dft/agg/aggregation_merge_operator.h"
  "index/store/index_database.cpp|index/schemas/dft/agg/system_metrics_merge_operator.h"
  "index/store/index_database_sst_writer_context.cpp|index/schemas/dft/agg/aggregation_merge_operator.h"
  "index/store/index_database_sst_writer_context.cpp|index/schemas/dft/agg/system_metrics_merge_operator.h"
  "index/store/index_write.h|index/gzip/gzip_member_record.h"
  "index/store/index_write.h|index/schemas/dft/chunk_statistics.h"
  "index/store/internal/index_encoding.h|index/gzip/gzip_member_record.h"
  "index/store/internal/index_encoding.h|index/schemas/dft/chunk_statistics.h"
  "index/store/queries.h|index/extensions/chunk_dimension_stats.h"
  "index/store/queries.h|index/schemas/dft/chunk_statistics.h"
  "index/store/types.h|index/gzip/member.h"
  "index/store/types.h|index/schemas/dft/statistics.h"
  # stage 10 moves the aggregation tier and cache onto extensions
  "index/cache/lookup_store.cpp|trace/views/view_plan.h"
  "index/cache/mv_store.cpp|trace/views/view_plan.h"
  "index/cache/mv_store.h|trace/views/view.h"
  "index/cache/rollup_store.cpp|trace/views/view_agg_engine.h"
  "index/cache/rollup_store.cpp|trace/views/view_plan.h"
  "index/cache/rollup_store.h|trace/views/view_aggregate.h"
  "index/schemas/dft/agg/aggregation_fold.h|trace/views/fold.h"
  "index/schemas/dft/agg/aggregation_fold.h|trace/views/fold_event.h"
  "index/schemas/dft/agg/aggregation_runner.cpp|index/build/batch_builder.h"
  "index/schemas/dft/agg/aggregation_runner.cpp|index/build/resolver.h"
  "index/schemas/dft/agg/aggregator.cpp|index/build/batch_builder.h"
  "index/schemas/dft/agg/aggregator.cpp|index/build/resolver.h"
  "index/schemas/dft/agg/view_agg_tier.cpp|trace/views/view_agg_engine.h"
  "index/schemas/dft/agg/view_agg_tier.cpp|trace/views/view_plan.h"
  "index/schemas/dft/agg/view_agg_tier.cpp|trace/views/view_scan.h"
  "index/schemas/dft/agg/view_agg_tier.h|trace/views/view_aggregate.h"
)

function(layer_of rel out)
  set(${out} "" PARENT_SCOPE)
  if(NOT rel MATCHES "^index/")
    return()
  endif()
  foreach(l ${LAYERS})
    if(rel MATCHES "^index/${l}/")
      set(${out} "${l}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out} "root" PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE index_files
     "${SOURCE_DIR}/include/dftracer/utils/index/*"
     "${SOURCE_DIR}/src/dftracer/utils/index/*")
list(SORT index_files)

set(errors "")
set(seen "")
foreach(path ${index_files})
  string(REGEX REPLACE "^.*/dftracer/utils/" "" rel "${path}")
  layer_of("${rel}" src_layer)
  if(src_layer STREQUAL "root")
    continue()
  endif()
  file(STRINGS "${path}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*<dftracer/utils/")
  foreach(line ${includes})
    string(REGEX REPLACE "^.*<dftracer/utils/([^>]+)>.*$" "\\1" inc "${line}")
    layer_of("${inc}" dst_layer)
    set(bad FALSE)
    if(dst_layer STREQUAL "")
      set(bad TRUE)
      foreach(prefix ${BASE_PREFIXES})
        string(FIND "${inc}" "${prefix}" at)
        if(at EQUAL 0)
          set(bad FALSE)
        endif()
      endforeach()
    elseif(NOT dst_layer STREQUAL "root"
           AND RANK_${dst_layer} GREATER RANK_${src_layer})
      set(bad TRUE)
    endif()
    if(bad)
      set(edge "${rel}|${inc}")
      list(FIND ALLOWED "${edge}" idx)
      if(idx EQUAL -1)
        string(APPEND errors "  not allowed: ${rel} includes ${inc}\n")
      else()
        list(APPEND seen "${edge}")
      endif()
    endif()
  endforeach()
endforeach()

foreach(edge ${ALLOWED})
  list(FIND seen "${edge}" idx)
  if(idx EQUAL -1)
    string(APPEND errors "  stale allowlist entry: ${edge}\n")
  endif()
endforeach()

if(errors)
  message(FATAL_ERROR "index layering violations:\n${errors}")
endif()
message(STATUS "index layering ok")
