# ##############################################################################
# Utilities Tests
# ##############################################################################

# This file contains tests for dftracer utilities organized by category.
# Each utility has its own test file for easy maintenance.

# +++++++++++++++++++++++++++++++++++++++++
# Utilities Test Sources
# +++++++++++++++++++++++++++++++++++++++++

set(UTILITIES_TEST_SOURCES
    # Call Tree
    call_tree/test_call_tree_internal.cpp

    # Composites
    composites/test_batch_processor.cpp
    composites/test_chunk_verifier.cpp
    composites/test_directory_file_processor.cpp
    composites/test_file_compressor.cpp
    composites/test_file_decompressor.cpp
    composites/test_file_merger.cpp
    composites/test_indexed_file_reader.cpp
    composites/test_line_batch_processor.cpp

    # DFT Composites
    composites/dft/test_chunk_extractor.cpp
    composites/dft/test_event_collector.cpp
    composites/dft/test_event_hasher.cpp
    composites/dft/test_index_builder.cpp
    composites/dft/test_metadata_collector.cpp

    # DFT Indexing Composites
    composites/dft/indexing/test_bloom_filter.cpp
    composites/dft/indexing/test_chunk_statistics.cpp
    composites/dft/indexing/test_chunk_indexer.cpp
    composites/dft/indexing/test_chunk_pruner.cpp
    composites/dft/indexing/test_manifest_indexer.cpp
    composites/dft/indexing/test_manifest_index_builder.cpp
    composites/dft/indexing/test_manifest_queries.cpp

    # DFT Reorganization Composites
    composites/dft/reorganize/test_reorganization_planner.cpp
    composites/dft/reorganize/test_reorganize_integration.cpp
    composites/dft/reorganize/test_reconstruction_planner.cpp
    composites/dft/reorganize/test_reconstruct_integration.cpp

    # DFT Views Composites
    composites/dft/views/test_view_definition.cpp
    composites/dft/views/test_view_builder.cpp
    composites/dft/views/test_view_reader.cpp

    # DFT Statistics Composites
    composites/dft/statistics/test_trace_statistics.cpp
    composites/dft/statistics/test_statistics_aggregator.cpp
    composites/dft/statistics/test_statistics_query.cpp
    common/statistics/test_log2_histogram.cpp
    common/statistics/test_timestamp_histogram.cpp
    composites/dft/statistics/test_detailed_statistics.cpp

    # Query language
    common/query/test_parser.cpp
    common/query/test_evaluator.cpp
    common/query/test_query.cpp

    # DFT Aggregator Composites
    common/statistics/test_ddsketch.cpp
    composites/dft/aggregators/test_aggregation_metrics.cpp
    composites/dft/aggregators/test_aggregation_key.cpp
    composites/dft/aggregators/test_aggregation_config.cpp
    composites/dft/aggregators/test_aggregation_serialization.cpp
    composites/dft/aggregators/test_aggregator_utility.cpp
    composites/dft/aggregators/test_chunk_aggregator_utility.cpp
    composites/dft/aggregators/test_event_aggregator_utility.cpp
    composites/dft/aggregators/test_aggregation_augmentation.cpp
    composites/dft/aggregators/test_system_metrics.cpp
    composites/dft/aggregators/test_system_metrics_merge_operator.cpp

    # DFT Comparator Composites
    composites/dft/comparator/test_comparison_result.cpp
    composites/dft/comparator/test_comparison_config.cpp
    composites/dft/comparator/test_is_data_transfer_op.cpp

    # Indexer
    indexer/test_rocksdb_storage.cpp
    indexer/test_scan_prefix.cpp
    indexer/test_index_database.cpp
    indexer/test_provenance_database.cpp
    indexer/test_index_builder.cpp
    indexer/test_sst_ingest_spike.cpp

    # Compression
    compression/zlib/test_streaming_compressor.cpp
    compression/zlib/test_streaming_decompressor.cpp

    # Filesystem
    filesystem/test_directory_scanner.cpp
    filesystem/test_pattern_directory_scanner.cpp

    # Hash
    hash/test_hasher_utility.cpp
    hash/test_mt_hasher.cpp
    hash/test_std_hasher.cpp

    # I/O
    fileio/test_file_reader.cpp
    fileio/test_streaming_file_reader.cpp
    fileio/test_streaming_file_writer.cpp

    # I/O Parallel
    fileio/parallel/test_layout_sizing.cpp
    fileio/parallel/test_striped_writer.cpp
    fileio/parallel/test_sharded_writer.cpp
    fileio/parallel/test_padded_striped_writer.cpp

    # I/O Lines
    fileio/lines/test_streaming_line_reader.cpp
    fileio/lines/sources/test_indexed_file_line_iterator.cpp
    fileio/lines/sources/test_indexed_file_bytes_iterator.cpp
    fileio/lines/sources/test_plain_file_line_iterator.cpp
    fileio/lines/sources/test_plain_file_bytes_iterator.cpp
    fileio/lines/sources/test_async_indexed_file_line_generator.cpp
    fileio/lines/sources/test_async_plain_file_line_generator.cpp
    fileio/lines/sources/test_async_indexed_file_bytes_generator.cpp
    fileio/lines/sources/test_async_plain_file_bytes_generator.cpp

    # Reader
    reader/test_trace_reader.cpp

    # Text
    text/test_line_filter.cpp
    text/test_line_splitter.cpp
)

# +++++++++++++++++++++++++++++++++++++++++
# Arrow Tests (optional, requires nanoarrow)
# +++++++++++++++++++++++++++++++++++++++++

if(DFTRACER_UTILS_ENABLE_ARROW)
  set(ARROW_TEST_SOURCE common/arrow/test_arrow_column_builder.cpp)
  string(REPLACE ".cpp" "" arrow_bin_exec ${ARROW_TEST_SOURCE})
  string(REPLACE "/" "_" arrow_target "utilities_${arrow_bin_exec}")

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
  add_test(NAME utilities/${arrow_bin_exec} COMMAND ${arrow_target})
  set_tests_properties(utilities/${arrow_bin_exec} PROPERTIES
                       WORKING_DIRECTORY "${arrow_workdir}")
endif()

# +++++++++++++++++++++++++++++++++++++++++
# Arrow IPC Tests (optional, requires nanoarrow IPC)
# +++++++++++++++++++++++++++++++++++++++++

if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
  # IPC Writer tests
  set(ARROW_IPC_TEST_SOURCE common/arrow/test_arrow_ipc_writer.cpp)
  string(REPLACE ".cpp" "" arrow_ipc_bin_exec ${ARROW_IPC_TEST_SOURCE})
  string(REPLACE "/" "_" arrow_ipc_target "utilities_${arrow_ipc_bin_exec}")

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
  add_test(NAME utilities/${arrow_ipc_bin_exec} COMMAND ${arrow_ipc_target})
  set_tests_properties(utilities/${arrow_ipc_bin_exec} PROPERTIES
                       WORKING_DIRECTORY "${arrow_ipc_workdir}")

  # IPC Reader tests
  set(ARROW_IPC_READER_SOURCE common/arrow/test_arrow_ipc_reader.cpp)
  string(REPLACE ".cpp" "" arrow_ipc_reader_bin ${ARROW_IPC_READER_SOURCE})
  string(REPLACE "/" "_" arrow_ipc_reader_target "utilities_${arrow_ipc_reader_bin}")

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
  add_test(NAME utilities/${arrow_ipc_reader_bin} COMMAND ${arrow_ipc_reader_target})
  set_tests_properties(utilities/${arrow_ipc_reader_bin} PROPERTIES
                       WORKING_DIRECTORY "${arrow_ipc_reader_workdir}")
endif()

# +++++++++++++++++++++++++++++++++++++++++
# Build Configuration
# +++++++++++++++++++++++++++++++++++++++++

foreach(test_file ${UTILITIES_TEST_SOURCES})
  # Extract path and filename
  string(REPLACE ".cpp" "" bin_exec ${test_file})

  # Create valid target name (replace / with _)
  string(REPLACE "/" "_" target_name "utilities_${bin_exec}")

  add_executable(${target_name} ${test_file})
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${target_name} PRIVATE doctest::doctest dftracer_utils testing_utilities)
  target_set_warnings(${target_name})
  target_enable_coroutine(${target_name})

  # Set output directory to preserve folder structure
  get_filename_component(bin_dir ${bin_exec} DIRECTORY)
  get_filename_component(bin_name ${bin_exec} NAME)

  if(bin_dir)
    set_target_properties(${target_name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${bin_dir}")
  endif()

  set_target_properties(${target_name} PROPERTIES OUTPUT_NAME ${bin_name})

  # Apply coverage flags if enabled
  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE --coverage -fprofile-arcs
                                                   -ftest-coverage)
    target_link_libraries(${target_name} PRIVATE --coverage)
  endif()

  set(test_working_directory
      "${CMAKE_CURRENT_BINARY_DIR}/workdirs/${target_name}")
  file(MAKE_DIRECTORY "${test_working_directory}")

  add_test(NAME utilities/${bin_exec} COMMAND ${target_name})
  set_tests_properties(utilities/${bin_exec} PROPERTIES
                       WORKING_DIRECTORY "${test_working_directory}")
endforeach()
