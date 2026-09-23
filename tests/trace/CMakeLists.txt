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
    test_event_collector.cpp
    test_index_builder.cpp
    test_metadata_collector.cpp
    test_time_metric.cpp

    # Indexing
    indexing/test_bloom_filter.cpp
    indexing/test_scalable_bloom_filter.cpp
    indexing/test_chunk_statistics.cpp
    indexing/test_chunk_indexer.cpp
    indexing/test_chunk_pruner.cpp
    indexing/test_sub_chunk_prune.cpp
    indexing/test_resolved_field_rewriter.cpp
    indexing/test_shard_manifest.cpp

    # Views
    views/test_view_definition.cpp
    views/test_view_planner.cpp
    views/test_view_scanner.cpp
    views/test_view_scan.cpp
    views/test_view_lazy.cpp
    views/test_view_stream_rows.cpp
    views/test_dynamic_prune.cpp
    views/test_metadata_query.cpp
    views/test_field_filter.cpp
    views/test_view_aggregate.cpp
    views/test_view_agg_parity.cpp
    views/test_view_agg_keys.cpp
    views/test_view_agg_scan.cpp
    views/test_stats_view.cpp
    views/test_detail_stats_view.cpp
    views/test_view_mv.cpp
    views/test_coverage.cpp
    views/test_fold.cpp
    views/test_pipeline_plan.cpp
    views/test_stream_early_out.cpp
    views/test_vectorized_fold.cpp
    views/test_rollup_store.cpp
    views/test_dict_fold.cpp
    views/test_bloom_fold.cpp
    views/test_raw_gzip_fuse.cpp
    views/test_event_source.cpp
    views/test_sharded_view.cpp
    views/test_result_join.cpp
    views/test_view_join.cpp
    views/test_compare_view.cpp
    views/test_view_c_abi.cpp

    # Statistics
    statistics/test_trace_statistics.cpp
    statistics/test_statistics_query.cpp
    statistics/test_detailed_statistics.cpp

    # Aggregators
    aggregators/test_aggregation_metrics.cpp
    aggregators/test_aggregation_rollup.cpp
    aggregators/test_aggregation_key.cpp
    aggregators/test_aggregation_config.cpp
    aggregators/test_aggregation_serialization.cpp
    aggregators/test_aggregator_utility.cpp
    aggregators/test_event_aggregator_utility.cpp
    aggregators/test_aggregation_augmentation.cpp
    aggregators/test_system_metrics.cpp
    aggregators/test_system_metrics_merge_operator.cpp

    # Comparator
    comparator/test_comparison_result.cpp
    comparator/test_comparison_config.cpp
    comparator/test_is_data_transfer_op.cpp
    comparator/test_tree_table_formatter.cpp
)

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
endforeach()
