# SPDX-License-Identifier: MIT
# ##############################################################################
# Trace Library Tests
# ##############################################################################

set(TRACE_TEST_SOURCES
    # Core
    test_parse_inflated.cpp
    test_schema.cpp
    test_chunk_verifier.cpp
    test_trace_config.cpp
    test_chunk_extractor.cpp
    index/build/test_batch_builder.cpp
    test_metadata_collector.cpp
    test_time_metric.cpp
    test_index_root_stat.cpp

    # Indexing
    index/extensions/test_bloom_filter.cpp
    index/extensions/test_scalable_bloom_filter.cpp
    index/schemas/dft/test_chunk_statistics.cpp
    index/build/test_chunk_indexer.cpp
    index/plan/test_chunk_pruner.cpp
    index/store/test_shard_manifest.cpp

    # Views
    views/test_view_definition.cpp
    views/test_view_planner.cpp
    views/test_view_index_freshness.cpp
    views/test_view_index_grow.cpp
    views/test_live_view.cpp
    views/test_view_scanner.cpp
    views/test_view_scan.cpp
    views/test_view_duql.cpp
    views/test_view_duql_joins.cpp
    views/test_view_duql_sources.cpp
    views/test_view_lazy.cpp
    views/test_view_stream_rows.cpp
    views/test_row_build.cpp
    views/test_dynamic_prune.cpp
    views/test_metadata_query.cpp
    views/test_field_filter.cpp
    views/test_view_aggregate.cpp
    views/test_view_partials.cpp
    views/test_view_index_parity.cpp
    views/test_time_window.cpp
    views/test_generic_view.cpp
    views/test_schema_view.cpp
    views/test_any_view.cpp
    views/test_typed_rows.cpp
    views/test_rowset_view.cpp
    views/test_view_agg_parity.cpp
    views/test_view_agg_keys.cpp
    views/test_view_agg_scan.cpp
    views/test_view_mv.cpp
    views/test_coverage.cpp
    views/test_fold.cpp
    views/test_fold_shapes.cpp
    views/test_fold_ondemand.cpp
    views/test_pipeline_plan.cpp
    views/test_view_fatal.cpp
    views/test_stream_early_out.cpp
    views/test_agg_transform_memory.cpp
    views/test_vectorized_fold.cpp
    index/cache/test_rollup_store.cpp
    index/cache/test_query_cache.cpp
    index/cache/test_lookup_store.cpp
    index/extensions/test_rowset_fold.cpp
    index/extensions/test_bloom_fold.cpp
    index/extensions/test_catalog_fold.cpp
    index/extensions/test_extension_registry.cpp
    views/test_raw_gzip_fuse.cpp
    views/test_restart_scan.cpp
    views/test_event_source.cpp
    views/test_sharded_view.cpp
    views/test_result_join.cpp
    views/test_view_join.cpp
    views/test_compare_view.cpp
    views/test_c_api.cpp
    views/test_duql_paths.cpp
    views/test_duql_wildcards.cpp
    views/test_drop_rename_views.cpp
    views/test_schemaless.cpp

    # Aggregators
    index/schemas/dft/agg/test_aggregation_metrics.cpp
    index/schemas/dft/agg/test_aggregation_config.cpp
    index/schemas/dft/agg/test_system_metrics.cpp
    index/schemas/dft/agg/test_quantile_store.cpp
    index/schemas/dft/agg/test_agg_partial.cpp
    test_agg_query.cpp

    # Internal
    internal/test_is_data_transfer_op.cpp

    # Index
    index/test_index_builder.cpp
    index/test_hash_table_aggregation.cpp

    # Reader
    reader/test_trace_reader.cpp
    reader/test_member_decode_cache.cpp
    reader/test_member_cache_integration.cpp
    reader/test_member_cache_exit.cpp
    reader/test_reader_fatal.cpp
    reader/test_large_gzip_members.cpp
    reader/test_gzip_restart_points.cpp
    reader/test_sigill_regression.cpp
    reader/test_indexed_file_reader.cpp
    reader/test_async_indexed_file_line_generator.cpp
)

# These read counters that exist only when DFTRACER_UTILS_TEST_PROBES is ON.
if(DFTRACER_UTILS_TEST_PROBES)
  list(APPEND TRACE_TEST_SOURCES test_bootstrap_parallelism.cpp
       test_bootstrap_peak.cpp)
endif()

# +++++++++++++++++++++++++++++++++++++++++
# Arrow Tests (optional, requires nanoarrow)
# +++++++++++++++++++++++++++++++++++++++++

if(DFTRACER_UTILS_ENABLE_ARROW)
  foreach(ARROW_TEST_SOURCE arrow/test_arrow_column_builder.cpp
                            arrow/test_schema_reconcile.cpp)
    string(REPLACE ".cpp" "" arrow_bin_exec ${ARROW_TEST_SOURCE})
    string(REPLACE "/" "_" arrow_target "trace_${arrow_bin_exec}")

    add_executable(${arrow_target} ${ARROW_TEST_SOURCE})
    target_include_directories(${arrow_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(${arrow_target} PRIVATE doctest::doctest dftracer_utils testing_utilities)
    link_nanoarrow(${arrow_target} STATIC)
    target_set_warnings(${arrow_target})

    get_filename_component(arrow_bin_dir ${arrow_bin_exec} DIRECTORY)
    get_filename_component(arrow_bin_name ${arrow_bin_exec} NAME)
    if(arrow_bin_dir)
      set_target_properties(${arrow_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${arrow_bin_dir}")
    endif()
    set_target_properties(${arrow_target} PROPERTIES OUTPUT_NAME ${arrow_bin_name})

    if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${arrow_target} PRIVATE --coverage -fprofile-arcs -ftest-coverage)
      target_link_libraries(${arrow_target} PRIVATE --coverage)
    endif()

    set(arrow_workdir "${CMAKE_CURRENT_BINARY_DIR}/workdirs/${arrow_target}")
    file(MAKE_DIRECTORY "${arrow_workdir}")
    add_test(NAME trace/${arrow_bin_exec} COMMAND ${arrow_target})
    set_tests_properties(trace/${arrow_bin_exec} PROPERTIES
                         WORKING_DIRECTORY "${arrow_workdir}")
  endforeach()
endif()

# +++++++++++++++++++++++++++++++++++++++++
# Arrow IPC Tests (optional, requires nanoarrow IPC)
# +++++++++++++++++++++++++++++++++++++++++

if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
  # IPC Writer tests
  set(ARROW_IPC_TEST_SOURCE arrow/test_arrow_ipc_writer.cpp)
  string(REPLACE ".cpp" "" arrow_ipc_bin_exec ${ARROW_IPC_TEST_SOURCE})
  string(REPLACE "/" "_" arrow_ipc_target "trace_${arrow_ipc_bin_exec}")

  add_executable(${arrow_ipc_target} ${ARROW_IPC_TEST_SOURCE})
  target_include_directories(${arrow_ipc_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${arrow_ipc_target} PRIVATE doctest::doctest dftracer_utils testing_utilities)
  link_nanoarrow(${arrow_ipc_target} STATIC)
  target_set_warnings(${arrow_ipc_target})

  get_filename_component(arrow_ipc_bin_dir ${arrow_ipc_bin_exec} DIRECTORY)
  get_filename_component(arrow_ipc_bin_name ${arrow_ipc_bin_exec} NAME)
  if(arrow_ipc_bin_dir)
    set_target_properties(${arrow_ipc_target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${arrow_ipc_bin_dir}")
  endif()
  set_target_properties(${arrow_ipc_target} PROPERTIES OUTPUT_NAME ${arrow_ipc_bin_name})

  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${arrow_ipc_target} PRIVATE --coverage -fprofile-arcs -ftest-coverage)
    target_link_libraries(${arrow_ipc_target} PRIVATE --coverage)
  endif()

  set(arrow_ipc_workdir "${CMAKE_CURRENT_BINARY_DIR}/workdirs/${arrow_ipc_target}")
  file(MAKE_DIRECTORY "${arrow_ipc_workdir}")
  add_test(NAME trace/${arrow_ipc_bin_exec} COMMAND ${arrow_ipc_target})
  set_tests_properties(trace/${arrow_ipc_bin_exec} PROPERTIES
                       WORKING_DIRECTORY "${arrow_ipc_workdir}")

  # IPC Reader tests
  set(ARROW_IPC_READER_SOURCE arrow/test_arrow_ipc_reader.cpp)
  string(REPLACE ".cpp" "" arrow_ipc_reader_bin ${ARROW_IPC_READER_SOURCE})
  string(REPLACE "/" "_" arrow_ipc_reader_target "trace_${arrow_ipc_reader_bin}")

  add_executable(${arrow_ipc_reader_target} ${ARROW_IPC_READER_SOURCE})
  target_include_directories(${arrow_ipc_reader_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${arrow_ipc_reader_target} PRIVATE doctest::doctest dftracer_utils testing_utilities)
  link_nanoarrow(${arrow_ipc_reader_target} STATIC)
  target_set_warnings(${arrow_ipc_reader_target})
  target_enable_coroutine(${arrow_ipc_reader_target})

  get_filename_component(arrow_ipc_reader_dir ${arrow_ipc_reader_bin} DIRECTORY)
  get_filename_component(arrow_ipc_reader_name ${arrow_ipc_reader_bin} NAME)
  if(arrow_ipc_reader_dir)
    set_target_properties(${arrow_ipc_reader_target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${arrow_ipc_reader_dir}")
  endif()
  set_target_properties(${arrow_ipc_reader_target} PROPERTIES OUTPUT_NAME ${arrow_ipc_reader_name})

  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${arrow_ipc_reader_target} PRIVATE --coverage -fprofile-arcs -ftest-coverage)
    target_link_libraries(${arrow_ipc_reader_target} PRIVATE --coverage)
  endif()

  set(arrow_ipc_reader_workdir "${CMAKE_CURRENT_BINARY_DIR}/workdirs/${arrow_ipc_reader_target}")
  file(MAKE_DIRECTORY "${arrow_ipc_reader_workdir}")
  add_test(NAME trace/${arrow_ipc_reader_bin} COMMAND ${arrow_ipc_reader_target})
  set_tests_properties(trace/${arrow_ipc_reader_bin} PROPERTIES
                       WORKING_DIRECTORY "${arrow_ipc_reader_workdir}")
endif()

foreach(test_file ${TRACE_TEST_SOURCES})
  string(REPLACE ".cpp" "" bin_exec ${test_file})
  string(REPLACE "/" "_" target_name "trace_${bin_exec}")

  add_executable(${target_name} ${test_file})
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${target_name} PRIVATE doctest::doctest dftracer_utils testing_utilities)
  target_set_warnings(${target_name})
  target_enable_coroutine(${target_name})

  get_filename_component(bin_dir ${bin_exec} DIRECTORY)
  get_filename_component(bin_name ${bin_exec} NAME)
  if(bin_dir)
    set_target_properties(${target_name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${bin_dir}")
  endif()
  set_target_properties(${target_name} PROPERTIES OUTPUT_NAME ${bin_name})

  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE --coverage -fprofile-arcs
                                                   -ftest-coverage)
    target_link_libraries(${target_name} PRIVATE --coverage)
  endif()

  set(test_working_directory
      "${CMAKE_CURRENT_BINARY_DIR}/workdirs/${target_name}")
  file(MAKE_DIRECTORY "${test_working_directory}")

  add_test(NAME trace/${bin_exec} COMMAND ${target_name})
  set_tests_properties(trace/${bin_exec} PROPERTIES
                       WORKING_DIRECTORY "${test_working_directory}")
  # Hundreds of queries over several traces, which is slower still under
  # coverage.
  if(bin_exec STREQUAL "views/test_view_duql")
    set(duql_timeout 600)
    if(DFTRACER_UTILS_COVERAGE OR DFTRACER_UTILS_ENABLE_TSAN
       OR DFTRACER_UTILS_ENABLE_ASAN)
      set(duql_timeout 1800)
    endif()
    set_tests_properties(trace/${bin_exec} PROPERTIES TIMEOUT ${duql_timeout})
  endif()
endforeach()

# duql joins give the same rows on any number of workers.
foreach(threads 1 2 8)
  add_test(NAME trace/views/test_view_duql_joins_t${threads}
           COMMAND trace_views_test_view_duql_joins)
  set_tests_properties(trace/views/test_view_duql_joins_t${threads} PROPERTIES
    WORKING_DIRECTORY
      "${CMAKE_CURRENT_BINARY_DIR}/workdirs/trace_views_test_view_duql_joins"
    ENVIRONMENT "DFTRACER_UTILS_THREADS=${threads}")
endforeach()

# With more workers than units, every unit is claimed before narrow() lands.
add_test(NAME trace/views/test_dynamic_prune_t224
         COMMAND trace_views_test_dynamic_prune)
set_tests_properties(trace/views/test_dynamic_prune_t224 PROPERTIES
  WORKING_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/workdirs/trace_views_test_dynamic_prune"
  ENVIRONMENT "DFTRACER_UTILS_THREADS=224")


# The allocator counts its allocations only in debug mode, which is fixed
# before the first allocation.
set_tests_properties(trace/index/schemas/dft/agg/test_quantile_store PROPERTIES
  ENVIRONMENT "DFTRACER_UTILS_MEMORY_DEBUG=1")
set_tests_properties(trace/index/schemas/dft/agg/test_agg_partial PROPERTIES
  ENVIRONMENT "DFTRACER_UTILS_MEMORY_DEBUG=1")
