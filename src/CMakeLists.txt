# ##############################################################################
# Dependencies
# ##############################################################################

add_rpath()

need_zlib()
need_libdeflate()
if(DFTRACER_UTILS_ENABLE_LZ4)
  need_lz4()
endif()
if(DFTRACER_UTILS_ENABLE_ZSTD)
  need_zstd()
endif()
need_rocksdb()
need_argparse()
need_ghc_filesystem()
need_simdjson()
need_readerwriterqueue()
need_concurrentqueue()
need_tl_expected()
need_unordered_dense()
need_boost_math()
need_yaml_cpp()

if(DFTRACER_UTILS_ENABLE_ARROW)
  need_nanoarrow()
endif()
need_highway()

# ##############################################################################
# Libraries
# ##############################################################################

# -----------------------------------------
# Core library sources
# -----------------------------------------

set(DFTRACER_UTILS_CORE_SOURCES
    # Common
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/common/logging.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/common/symbolize.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/common/format_detector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/common/filesystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/common/filesystem_info.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/common/scratch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/common/memory_budget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/env.cpp
    # Utilities
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/utils/timer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/utils/string.cpp
    # Pipeline - New Architecture
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/error.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/executor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/run_loop.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/thread_pool_executor.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/pipeline/watchdog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/runtime.cpp
    # Coro
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/coro/coro.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/coro/yield.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/coro/task_abi.cpp
    # Async I/O
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/io/io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/io/io_awaitable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/io/io_thread_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/io/thread_pool_file_ops.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/io/thread_pool_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/io/io_backend_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/io/io_backend_sync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/io/io_completion_thread.cpp
    # Tasks
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/tasks/task.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/tasks/task_result.cpp
    # Utilities
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/utilities/monitor.cpp
    # RocksDB
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/rocksdb/database.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/rocksdb/filesystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/rocksdb/key_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/rocksdb/db_manager.cpp
)

# Conditionally add io_uring backend sources
if(DFTRACER_UTILS_HAVE_IO_URING)
  list(APPEND DFTRACER_UTILS_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/io/io_uring_backend.cpp
  )
endif()

# Linux: add epoll+threadpool backend
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND DFTRACER_UTILS_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/io/epoll_thread_pool_backend.cpp
  )
endif()

# macOS/BSD: add kqueue+threadpool backend
if(DFTRACER_UTILS_HAVE_KQUEUE)
  list(APPEND DFTRACER_UTILS_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/io/kqueue_thread_pool_backend.cpp
  )
endif()

# -----------------------------------------
# Utilities library sources
# -----------------------------------------

set(DFTRACER_UTILS_UTILITIES_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/host_ops.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/metadata_collector_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/fileio/chunk_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/fileio/compress/libdeflate_gzip.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/fileio/parallel/layout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/fileio/parallel/striped_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/fileio/parallel/sharded_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/fileio/parallel/padded_striped_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/fileio/parallel/merge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/chunk_extractor_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/chunk_manifest_mapper_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/event_collector_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/internal/utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/trace_config.cpp
    # Common utilities (shared across modules)
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/statistics/log2_histogram.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/statistics/timestamp_histogram.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/statistics/distributions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/statistics/mixture.cpp
    # DLIO config generation
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/dlio/barrier_simulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/dlio/trace_loader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/dlio/optimizer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/dlio/yaml_emit.cpp
)

list(APPEND DFTRACER_UTILS_UTILITIES_SOURCES
    # DFT Aggregators
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/aggregation_drain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/agg_db_open.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/aggregation_metrics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/aggregation_runner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/aggregation_intern.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/aggregation_serialization.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/aggregation_merge_operator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/system_metrics_serialization.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/system_metrics_merge_operator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/aggregation_logic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/aggregation_augmentation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/event_aggregator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/association_tracker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/dftracer_trace_writer_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/aggregators/aggregator_utility.cpp

    # DFT Indexing (bloom filters, statistics, queries)
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/indexing/bloom_filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/indexing/scalable_bloom_filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/indexing/chunk_statistics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/indexing/chunk_dimension_stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/indexing/chunk_indexer_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/indexing/chunk_pruner_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/indexing/resolved_field_rewriter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/indexing/index_resolver_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/indexing/resolve_and_build.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/indexing/shard_manifest.cpp

    # DFT Statistics (trace statistics, aggregation, querying)
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/statistics/trace_statistics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/statistics/statistics_query_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/statistics/log2_histogram.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/statistics/timestamp_histogram.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/statistics/detailed_statistics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/statistics/detail_stats_view.cpp

    # DFT Views (filtered lenses over trace files)
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_definition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_scanner_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_planner_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_source.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_aggregate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_agg_engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_agg_tier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_resolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_scan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/native_row_fold.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/stream_row_fold.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/containment_fold.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/hash_table_dictionary.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/dict_fold.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/index_write_lock.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/bloom_fold.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/aggregation_fold.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/fold.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/batch_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_counter_format.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/rollup_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/mv_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_executor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/sharded_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_export_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/result_join.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/abi.cpp

    # Plugins (C ABI fold adapter + dlopen host + config tree builder)
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/plugins/build_host.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/plugins/config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/plugins/fold_adapter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/plugins/fold_adapter/agg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/plugins/fold_adapter/result.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/plugins/fold_adapter/ops.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/plugins/fold_adapter/state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/plugins/fold_adapter/providers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/plugins/fold_adapter/nodes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/plugins/state_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/plugins/plugins.cpp

    # DFT Comparator
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/comparator/comparison_aggregation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/comparator/comparison_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/comparator/comparison_result.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/comparator/comparison_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/comparator/tree_table_formatter.cpp

    # Indexer
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/index_database.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/index_database_sst_writer_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/index_database_writer_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/index_batch_builder_utility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/visitors/bloom_core.cpp
    # Reader
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/trace_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/trace_reader_arrow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/trace_reader_prefilter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/arrow_row_builder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/chunk_geometry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/json_dict_builder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/member_decode_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/indexer_c.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/index_encoding.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/column_type_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/checkpoint_size.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/error.cpp
    # Indexer factory
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/indexer_factory.cpp
    # GZIP indexer
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.cpp
    # TAR indexer
    # Reader
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/reader_c.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/stream_c.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/gzip_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/reader_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/error.cpp
    # Replay
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/replay/replay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/replay/replay_executors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/replay/replay_result.cpp
    # Server (HTTP/TCP)
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/cancel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/cursor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/http_connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/http_request.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/http_response.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/router.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/signal_handler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/tcp_listener.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/trace_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/trace_api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/viz/api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/viz/calltree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/viz/counters.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/viz/density.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/viz/events.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/viz/histogram.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/viz/misc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/viz/proctree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/viz/stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/viz/summary_build.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/http_parser.cpp
)

# Add MPI-specific sources if MPI is enabled
if(DFTRACER_UTILS_ENABLE_MPI)
  list(APPEND DFTRACER_UTILS_UTILITIES_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/core/mpi/mpi_utils.cpp
  )
endif()

if(DFTRACER_UTILS_ENABLE_ARROW)
  list(APPEND DFTRACER_UTILS_UTILITIES_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/arrow/attribution.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/arrow/column_builder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/arrow/gapfill.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/arrow/join.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/arrow/window.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/arrow/frame_ops.cpp
  )
endif()

if(DFTRACER_UTILS_ENABLE_ARROW_IPC)
  list(APPEND DFTRACER_UTILS_UTILITIES_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/arrow/ipc_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/arrow/ipc_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/arrow/parallel_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/arrow/partition_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/common/arrow/partition_router.cpp
  )
endif()

# Determine which dependencies to require based on what was found vs built with
# CPM
set(PKG_CONFIG_REQUIRES "")
set(PKG_CONFIG_LIBS_PRIVATE "")

# Add Threads (always required)
set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -lpthread")

# Only add zlib to pkg-config requirements if it was found on system
if(ZLIB_FOUND AND NOT ZLIB_CPM)
  set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -lz")
else()
  set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -ldftracer_zlib")
endif()

# Add other dependencies built with CPM
set(PKG_CONFIG_LIBS_PRIVATE "${PKG_CONFIG_LIBS_PRIVATE} -lsimdjson")

# +++++++++++++++++++++++++++++++++++++++++
# Core Library
# +++++++++++++++++++++++++++++++++++++++++

# Collect all dependency libraries
if(DFTRACER_UTILS_BUILD_SHARED OR DFTRACER_UTILS_BUILD_STATIC)
  # noop
endif()

# Compile options for coverage
set(COVERAGE_COMPILE_OPTIONS "")
set(COVERAGE_LINK_OPTIONS "")
if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(COVERAGE_COMPILE_OPTIONS --coverage -fprofile-arcs -ftest-coverage)
  set(COVERAGE_LINK_OPTIONS --coverage)
endif()

# +++++++++++++++++++++++++++++++++++++++++
# Configure Precompiled Headers
# +++++++++++++++++++++++++++++++++++++++++

# Set global PCH configuration (used by use_precompiled_header function)
if(DFTRACER_UTILS_ENABLE_PCH)
  set(DFTRACER_UTILS_PCH_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
  set(DFTRACER_UTILS_PCH_MIN_COUNT 2)
endif()

create_library(
  NAME
    dftracer_utils_core
  NAMESPACE
    dftracer_utils_core
  SOURCES
    ${DFTRACER_UTILS_CORE_SOURCES}
  VERSION
    ${PROJECT_VERSION}
  DESCRIPTION
    "DFTracer utils core library (indexer, reader, pipeline)"
  URL
    "https://github.com/LLNL/dftracer-utils"
  BUILD_SHARED
    ${DFTRACER_UTILS_BUILD_SHARED}
  BUILD_STATIC
    ${DFTRACER_UTILS_BUILD_STATIC}
  PUBLIC_LINK_LIBRARIES
    ghc_filesystem
    Threads::Threads
    readerwriterqueue
    concurrentqueue
  PUBLIC_INCLUDE_DIRS
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE_COMPILE_OPTIONS
    ${COVERAGE_COMPILE_OPTIONS}
  PKG_CONFIG_REQUIRES
    "${PKG_CONFIG_REQUIRES}"
  PKG_CONFIG_LIBS_PRIVATE
    "${PKG_CONFIG_LIBS_PRIVATE}")

# Post-process core library targets to add dependency linking and warnings
foreach(variant shared static)
  string(TOUPPER ${variant} VARIANT_UPPER)
  if(TARGET dftracer_utils_core_${variant})
    # Link dependencies using helper functions
    link_simdjson(dftracer_utils_core_${variant} ${VARIANT_UPPER})
    link_rocksdb(dftracer_utils_core_${variant} ${VARIANT_UPPER})
    link_zlib(dftracer_utils_core_${variant} ${VARIANT_UPPER})
    link_unordered_dense(dftracer_utils_core_${variant})
    link_tl_expected(dftracer_utils_core_${variant})

    # RocksDB is a static archive linked into this .so. Without this its
    # symbols are re-exported into ELF's flat namespace, where they can bind
    # against a different RocksDB loaded in the same process.
    if(variant STREQUAL "shared"
       AND ROCKSDB_IS_STATIC
       AND NOT APPLE
       AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_link_options(dftracer_utils_core_shared PRIVATE
                          "LINKER:--exclude-libs,librocksdb.a")
    endif()

    # The coroutine monitor (monitor.cpp) calls dladdr() for symbolization.
    # On older glibc (e.g. manylinux aarch64) dladdr lives in libdl, so the
    # core library must link it explicitly; CMAKE_DL_LIBS is empty where the
    # symbol is already in libc (newer glibc) or libSystem (macOS).
    target_link_libraries(dftracer_utils_core_${variant}
                          PUBLIC ${CMAKE_DL_LIBS})

    # Add stdfs if needed
    add_stdfs_if_needed(dftracer_utils_core_${variant})

    # filesystem_info.cpp (core) calls llapi_file_get_stripe when lustreapi is
    # present
    if(DFTRACER_UTILS_HAVE_LUSTREAPI)
      target_link_libraries(dftracer_utils_core_${variant}
                            PRIVATE ${LUSTREAPI_LIBRARY})
    endif()

    # mpi_utils.cpp is part of the core library sources when MPI is
    # enabled, so core itself needs the MPI include path and runtime.
    # Without this the precompiled header pulls in <mpi.h> and fails
    # with "mpi.h: No such file or directory".
    if(DFTRACER_UTILS_ENABLE_MPI)
      target_compile_definitions(dftracer_utils_core_${variant} PUBLIC DFTRACER_UTILS_MPI_ENABLED)
      target_link_libraries(dftracer_utils_core_${variant} PUBLIC MPI::MPI_CXX)
    endif()

    # Set warnings
    target_set_warnings(dftracer_utils_core_${variant})

    # Apply precompiled headers (automatically handles detection and reuse)
    if(DFTRACER_UTILS_ENABLE_PCH)
      use_precompiled_header(dftracer_utils_core_${variant})
    endif()

    # Link coverage if needed
    if(COVERAGE_LINK_OPTIONS)
      target_link_libraries(dftracer_utils_core_${variant}
                            PRIVATE ${COVERAGE_LINK_OPTIONS})
    endif()

    # io_uring compile definition
    if(DFTRACER_UTILS_HAVE_IO_URING)
      target_compile_definitions(dftracer_utils_core_${variant}
                                PRIVATE DFTRACER_UTILS_HAVE_IO_URING)
    endif()
  endif()
endforeach()

# +++++++++++++++++++++++++++++++++++++++++
# Vec Library (native vectorized columnar engine)
# +++++++++++++++++++++++++++++++++++++++++

set(DFTRACER_UTILS_VEC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/abi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/abi_enum_lockstep.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/op_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/agg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/agg/state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/agg/finalize.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/agg/seed.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/agg/serialize.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/agg/spill.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/agg/group.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/agg_expr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/expr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/lazyframe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/group_transform.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/column_op.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/spill.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/parallel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/parallel_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/arrow_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/arrow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/batch_ops.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/join.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/sketch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/sketch_keys.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/arithmetic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/arithmetic_extra.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/temporal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/cast.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/cast_simd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/prims.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/sort.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/stats_simd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/moments.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/field_stat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/elementwise.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/predicate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/select.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/comparison.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/dictionary.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/filter_simd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/compare_simd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/reduce_simd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/group_by.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/logical.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/reduce.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/string_ops.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/substr_simd.cpp
)

# Highway per-target dispatch functions (N_SSE2 etc.) live in the named
# HWY_NAMESPACE for HWY_EXPORT and are referenced only within their own TU, so
# they legitimately have no forward declaration.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/arithmetic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/cast_simd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/compare_simd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/elementwise.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/field_stat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/filter_simd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/logical.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/moments.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/predicate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/prims.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/reduce_simd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/sketch_keys.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/stats_simd.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/kernels/substr_simd.cpp
    PROPERTIES COMPILE_OPTIONS -Wno-missing-declarations)

# The columnar engine (DFTRACER_UTILS_VEC_SOURCES) is built as part of the
# single dftracer_utils_dataframe target below, not its own library.

# +++++++++++++++++++++++++++++++++++++++++
# JSON Library (leaf: simdjson-backed value + parser)
# +++++++++++++++++++++++++++++++++++++++++

set(DFTRACER_UTILS_JSON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/json/json_value.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/json/parser.cpp)

create_library(
  NAME
    dftracer_utils_json
  NAMESPACE
    dftracer_utils_json
  SOURCES
    ${DFTRACER_UTILS_JSON_SOURCES}
  VERSION
    ${PROJECT_VERSION}
  DESCRIPTION
    "DFTracer utils json library (simdjson-backed JSON value + parser)"
  URL
    "https://github.com/LLNL/dftracer-utils"
  BUILD_SHARED
    ${DFTRACER_UTILS_BUILD_SHARED}
  BUILD_STATIC
    ${DFTRACER_UTILS_BUILD_STATIC}
  SHARED_LINK_LIBRARIES
    dftracer_utils_core::shared
  STATIC_LINK_LIBRARIES
    dftracer_utils_core::static
  PUBLIC_INCLUDE_DIRS
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE_COMPILE_OPTIONS
    ${COVERAGE_COMPILE_OPTIONS}
  PKG_CONFIG_REQUIRES
    "dftracer_utils_core"
  FIND_DEPENDENCIES
    dftracer_utils_core)

foreach(variant shared static)
  if(TARGET dftracer_utils_json_${variant})
    target_set_warnings(dftracer_utils_json_${variant})
    string(TOUPPER ${variant} VARIANT_UPPER)
    link_simdjson(dftracer_utils_json_${variant} ${VARIANT_UPPER})
    add_stdfs_if_needed(dftracer_utils_json_${variant})
    if(DFTRACER_UTILS_ENABLE_PCH)
      use_precompiled_header(dftracer_utils_json_${variant})
    endif()
    if(COVERAGE_LINK_OPTIONS)
      target_link_libraries(dftracer_utils_json_${variant}
                            PRIVATE ${COVERAGE_LINK_OPTIONS})
    endif()
  endif()
endforeach()

# +++++++++++++++++++++++++++++++++++++++++
# Query Library (predicate IR + string codec + evaluator)
# +++++++++++++++++++++++++++++++++++++++++

set(DFTRACER_UTILS_QUERY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/query/ast.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/query/parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/query/evaluator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/query/query.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/query/subsumption.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/query/abi.cpp)

create_library(
  NAME
    dftracer_utils_query
  NAMESPACE
    dftracer_utils_query
  SOURCES
    ${DFTRACER_UTILS_QUERY_SOURCES}
  VERSION
    ${PROJECT_VERSION}
  DESCRIPTION
    "DFTracer utils query library (predicate IR, string parser, evaluator)"
  URL
    "https://github.com/LLNL/dftracer-utils"
  BUILD_SHARED
    ${DFTRACER_UTILS_BUILD_SHARED}
  BUILD_STATIC
    ${DFTRACER_UTILS_BUILD_STATIC}
  SHARED_LINK_LIBRARIES
    dftracer_utils_core::shared
    dftracer_utils_json::shared
  STATIC_LINK_LIBRARIES
    dftracer_utils_core::static
    dftracer_utils_json::static
  PUBLIC_INCLUDE_DIRS
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE_COMPILE_OPTIONS
    ${COVERAGE_COMPILE_OPTIONS}
  PKG_CONFIG_REQUIRES
    "dftracer_utils_core dftracer_utils_json"
  FIND_DEPENDENCIES
    dftracer_utils_core dftracer_utils_json)

foreach(variant shared static)
  if(TARGET dftracer_utils_query_${variant})
    target_set_warnings(dftracer_utils_query_${variant})
    string(TOUPPER ${variant} VARIANT_UPPER)
    link_simdjson(dftracer_utils_query_${variant} ${VARIANT_UPPER})
    add_stdfs_if_needed(dftracer_utils_query_${variant})
    if(DFTRACER_UTILS_ENABLE_PCH)
      use_precompiled_header(dftracer_utils_query_${variant})
    endif()
    if(COVERAGE_LINK_OPTIONS)
      target_link_libraries(dftracer_utils_query_${variant}
                            PRIVATE ${COVERAGE_LINK_OPTIONS})
    endif()
  endif()
endforeach()

# +++++++++++++++++++++++++++++++++++++++++
# DataFrame Library (query execution over vec: mask + plan)
# +++++++++++++++++++++++++++++++++++++++++

set(DFTRACER_UTILS_DATAFRAME_SOURCES
    ${DFTRACER_UTILS_VEC_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/series_methods.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/dataframe_methods.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/dataframe_abi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/provider_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/node_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/mask.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/dataframe/mask_abi.cpp)

create_library(
  NAME
    dftracer_utils_dataframe
  NAMESPACE
    dftracer_utils_dataframe
  SOURCES
    ${DFTRACER_UTILS_DATAFRAME_SOURCES}
  VERSION
    ${PROJECT_VERSION}
  DESCRIPTION
    "DFTracer utils dataframe engine (columnar Series/DataFrame + kernels, expressions, query execution)"
  URL
    "https://github.com/LLNL/dftracer-utils"
  BUILD_SHARED
    ${DFTRACER_UTILS_BUILD_SHARED}
  BUILD_STATIC
    ${DFTRACER_UTILS_BUILD_STATIC}
  SHARED_LINK_LIBRARIES
    dftracer_utils_core::shared
    dftracer_utils_query::shared
  STATIC_LINK_LIBRARIES
    dftracer_utils_core::static
    dftracer_utils_query::static
  PUBLIC_INCLUDE_DIRS
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE_COMPILE_OPTIONS
    ${COVERAGE_COMPILE_OPTIONS}
  PKG_CONFIG_REQUIRES
    "dftracer_utils_core dftracer_utils_query"
  FIND_DEPENDENCIES
    dftracer_utils_core dftracer_utils_query)

foreach(variant shared static)
  if(TARGET dftracer_utils_dataframe_${variant})
    target_set_warnings(dftracer_utils_dataframe_${variant})
    # Keep the many per-ISA Highway instantiations and header-inline helpers out
    # of the shared object's dynamic symbol table; the stable C ABI (marked
    # DFTU_EXPORT / default-visible) and the out-of-line C++ API stay exported.
    set_target_properties(dftracer_utils_dataframe_${variant} PROPERTIES
                          VISIBILITY_INLINES_HIDDEN ON)
    string(TOUPPER ${variant} VARIANT_UPPER)
    link_simdjson(dftracer_utils_dataframe_${variant} ${VARIANT_UPPER})
    # Highway powers the SIMD kernels; hwy_contrib carries VQSort (argsort) and
    # publicly depends on hwy, so linking it pulls hwy in too. Fall back to hwy
    # alone when contrib is absent; linking both puts libhwy.a on the line twice.
    if(TARGET hwy_contrib)
      target_link_libraries(dftracer_utils_dataframe_${variant}
                            PRIVATE $<BUILD_INTERFACE:hwy_contrib>)
    elseif(TARGET hwy)
      target_link_libraries(dftracer_utils_dataframe_${variant}
                            PRIVATE $<BUILD_INTERFACE:hwy>)
    endif()
    add_stdfs_if_needed(dftracer_utils_dataframe_${variant})
    if(DFTRACER_UTILS_ENABLE_PCH)
      use_precompiled_header(dftracer_utils_dataframe_${variant})
    endif()
    if(DFTRACER_UTILS_ENABLE_ARROW)
      link_nanoarrow(dftracer_utils_dataframe_${variant} ${VARIANT_UPPER})
    endif()
    if(COVERAGE_LINK_OPTIONS)
      target_link_libraries(dftracer_utils_dataframe_${variant}
                            PRIVATE ${COVERAGE_LINK_OPTIONS})
    endif()
  endif()
endforeach()

# +++++++++++++++++++++++++++++++++++++++++
# Utilities Library
# +++++++++++++++++++++++++++++++++++++++++

create_library(
  NAME
    dftracer_utils_utilities
  NAMESPACE
    dftracer_utils_utilities
  SOURCES
    ${DFTRACER_UTILS_UTILITIES_SOURCES}
  VERSION
    ${PROJECT_VERSION}
  DESCRIPTION
    "DFTracer utils utilities library (hash, io, compression, trace)"
  URL
    "https://github.com/LLNL/dftracer-utils"
  BUILD_SHARED
    ${DFTRACER_UTILS_BUILD_SHARED}
  BUILD_STATIC
    ${DFTRACER_UTILS_BUILD_STATIC}
  SHARED_LINK_LIBRARIES
    dftracer_utils_core::shared
    dftracer_utils_dataframe::shared
    dftracer_utils_json::shared
    dftracer_utils_query::shared
  STATIC_LINK_LIBRARIES
    dftracer_utils_core::static
    dftracer_utils_dataframe::static
    dftracer_utils_json::static
    dftracer_utils_query::static
  PUBLIC_INCLUDE_DIRS
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE_COMPILE_OPTIONS
    ${COVERAGE_COMPILE_OPTIONS}
  PKG_CONFIG_REQUIRES
    "dftracer_utils_core"
  FIND_DEPENDENCIES
    dftracer_utils_core)

# Post-process utilities library targets
# simdjson (third-party) inlined here trips GCC's optimizer-based
# -Wstrict-overflow that -isystem cannot suppress; keep =2 for our own code.
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/trace_reader.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/trace_reader_arrow.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/trace_reader_prefilter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/arrow_row_builder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/utilities/reader/internal/json_dict_builder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/trace/views/view_scanner_utility.cpp
  PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-strict-overflow>")

foreach(variant shared static)
  if(TARGET dftracer_utils_utilities_${variant})
    # No need to link dependencies here - they're inherited from core

    # Link tl::expected for C++20/23 expected shim
    link_tl_expected(dftracer_utils_utilities_${variant})

    # The plugin host dlopen()s plugin shared libraries; libdl is empty where
    # the symbols already live in libc/libSystem.
    target_link_libraries(dftracer_utils_utilities_${variant}
                          PUBLIC ${CMAKE_DL_LIBS})

    # Add stdfs if needed
    add_stdfs_if_needed(dftracer_utils_utilities_${variant})

    # Set warnings
    target_set_warnings(dftracer_utils_utilities_${variant})

    # Add MPI compile definitions and link library when enabled
    if(DFTRACER_UTILS_ENABLE_MPI)
      target_compile_definitions(dftracer_utils_utilities_${variant} PUBLIC DFTRACER_UTILS_MPI_ENABLED)
      target_link_libraries(dftracer_utils_utilities_${variant} PUBLIC MPI::MPI_CXX)
    endif()

    # Apply precompiled headers (automatically reuses from core library)
    if(DFTRACER_UTILS_ENABLE_PCH)
      use_precompiled_header(dftracer_utils_utilities_${variant})
    endif()

    # Link coverage if needed
    if(COVERAGE_LINK_OPTIONS)
      target_link_libraries(dftracer_utils_utilities_${variant}
                            PRIVATE ${COVERAGE_LINK_OPTIONS})
    endif()

    # Link nanoarrow for Arrow data interchange
    if(DFTRACER_UTILS_ENABLE_ARROW)
      string(TOUPPER ${variant} VARIANT_UPPER)
      link_nanoarrow(dftracer_utils_utilities_${variant} ${VARIANT_UPPER})
    endif()

    # Boost.Math (standalone, header-only) for DLIO distribution fitting.
    link_boost_math(dftracer_utils_utilities_${variant})

    # yaml-cpp for DLIO config emit.
    link_yaml_cpp(dftracer_utils_utilities_${variant})

    # libdeflate: one-shot gzip/deflate codec for member-at-a-time decode.
    link_libdeflate(dftracer_utils_utilities_${variant})

    # Link zstd when ENABLE_ZSTD is on so headers propagate to consumers
    # (e.g. arrow ipc_writer.cpp guarded by DFTRACER_UTILS_ENABLE_ZSTD).
    if(DFTRACER_UTILS_ENABLE_ZSTD)
      if(TARGET zstd::libzstd_shared)
        target_link_libraries(dftracer_utils_utilities_${variant}
                              PRIVATE zstd::libzstd_shared)
      elseif(TARGET zstd::libzstd_static)
        target_link_libraries(dftracer_utils_utilities_${variant}
                              PRIVATE zstd::libzstd_static)
      elseif(TARGET libzstd_shared)
        target_link_libraries(dftracer_utils_utilities_${variant}
                              PRIVATE libzstd_shared)
      elseif(TARGET libzstd_static)
        target_link_libraries(dftracer_utils_utilities_${variant}
                              PRIVATE libzstd_static)
      endif()
    endif()

    # Lustre stripe query (optional). The config header carries the
    # DFTRACER_UTILS_HAVE_LUSTREAPI define; we just need to link the lib.
    if(DFTRACER_UTILS_HAVE_LUSTREAPI)
      target_link_libraries(dftracer_utils_utilities_${variant}
                            PRIVATE ${LUSTREAPI_LIBRARY})
    endif()

    # The host ops (dftu.fs.*, dftu.file.*, dftu.text.*) register from a static
    # initializer that nothing references, so a linker drops it and the ops
    # silently do not exist. A static link drops the archive member; a shared
    # link under GNU ld drops the whole DT_NEEDED entry, since --as-needed is
    # the default on most Linux distros and no symbol of ours is referenced.
    # Force a reference through the unmangled anchor in both cases. Mach-O
    # prefixes an underscore, and does not default to --as-needed, which is why
    # the shared half only ever failed on Linux.
    if(APPLE)
      set(host_ops_anchor "_dftu_register_host_ops")
    else()
      set(host_ops_anchor "dftu_register_host_ops")
    endif()
    target_link_options(dftracer_utils_utilities_${variant}
                        INTERFACE "LINKER:-u,${host_ops_anchor}")
    # GNU ld does not count -u as a reference when deciding --as-needed, so the
    # shared library is still dropped from DT_NEEDED and its initializer never
    # runs. Keep the dependency explicitly; Mach-O has no --as-needed and needs
    # only the -u above.
    if(NOT APPLE AND variant STREQUAL "shared")
      target_link_options(dftracer_utils_utilities_shared INTERFACE
                          "LINKER:--no-as-needed")
    endif()
  endif()
endforeach()

# Create unified configs for core and utilities libraries This allows discovery
# via "dftracer_utils_core" instead of "dftracer_utils_core_shared"
create_unified_library_config(
  NAME
    dftracer_utils_core
  VERSION
    ${PROJECT_VERSION}
  DESCRIPTION
    "DFTracer utils core library (indexer, reader, pipeline)"
  URL
    "https://github.com/LLNL/dftracer-utils"
  BUILD_SHARED
    ${DFTRACER_UTILS_BUILD_SHARED}
  BUILD_STATIC
    ${DFTRACER_UTILS_BUILD_STATIC})

create_unified_library_config(
  NAME
    dftracer_utils_utilities
  VERSION
    ${PROJECT_VERSION}
  DESCRIPTION
    "DFTracer utils utilities library (hash, io, compression, trace)"
  URL
    "https://github.com/LLNL/dftracer-utils"
  PKG_CONFIG_REQUIRES
    "dftracer_utils_core"
  FIND_DEPENDENCIES
    dftracer_utils_core
  BUILD_SHARED
    ${DFTRACER_UTILS_BUILD_SHARED}
  BUILD_STATIC
    ${DFTRACER_UTILS_BUILD_STATIC})

# +++++++++++++++++++++++++++++++++++++++++
# Unified Interface Library
# +++++++++++++++++++++++++++++++++++++++++

# Create interface libraries that link both core and utilities
if(DFTRACER_UTILS_BUILD_SHARED)
  add_library(dftracer_utils_shared INTERFACE)
  target_link_libraries(
    dftracer_utils_shared INTERFACE dftracer_utils_core::shared
                                    dftracer_utils_utilities::shared
                                    dftracer_utils_dataframe::shared)

  # Create aliases
  add_library(dftracer_utils::shared ALIAS dftracer_utils_shared)
  add_library(dftracer_utils ALIAS dftracer_utils_shared)
  add_library(dftracer::utils ALIAS dftracer_utils_shared)
  add_library(dftracer_utils::dftracer_utils ALIAS dftracer_utils_shared)

  # Export the interface library
  install(TARGETS dftracer_utils_shared EXPORT dftracer_utilsTargets)
endif()

if(DFTRACER_UTILS_BUILD_STATIC)
  add_library(dftracer_utils_static INTERFACE)
  target_link_libraries(
    dftracer_utils_static INTERFACE dftracer_utils_core::static
                                    dftracer_utils_utilities::static
                                    dftracer_utils_dataframe::static)

  # Create aliases
  add_library(dftracer_utils::static ALIAS dftracer_utils_static)

  # If shared is not built, create default aliases
  if(NOT DFTRACER_UTILS_BUILD_SHARED)
    add_library(dftracer_utils ALIAS dftracer_utils_static)
    add_library(dftracer::utils ALIAS dftracer_utils_static)
    add_library(dftracer_utils::dftracer_utils ALIAS dftracer_utils_static)
  endif()

  # Export the interface library
  install(TARGETS dftracer_utils_static EXPORT dftracer_utilsTargets)
endif()

# Create unified package config for dftracer_utils
if(DFTRACER_UTILS_BUILD_SHARED)
  set(DFTRACER_UTILS_PREFERRED_TYPE "shared")
elseif(DFTRACER_UTILS_BUILD_STATIC)
  set(DFTRACER_UTILS_PREFERRED_TYPE "static")
endif()

if(DFTRACER_UTILS_PREFERRED_TYPE)
  # Create the unified CMake Config file programmatically
  set(UNIFIED_CONFIG_CONTENT "")
  string(APPEND UNIFIED_CONFIG_CONTENT "@PACKAGE_INIT@\n\n")
  string(APPEND UNIFIED_CONFIG_CONTENT "include(CMakeFindDependencyMacro)\n\n")
  string(APPEND UNIFIED_CONFIG_CONTENT "# Find core and utilities libraries\n")
  string(APPEND UNIFIED_CONFIG_CONTENT "find_dependency(dftracer_utils_core)\n")
  string(APPEND UNIFIED_CONFIG_CONTENT
         "find_dependency(dftracer_utils_utilities)\n")
  string(APPEND UNIFIED_CONFIG_CONTENT
         "find_dependency(dftracer_utils_dataframe)\n\n")
  string(APPEND UNIFIED_CONFIG_CONTENT "# Include the unified targets file\n")
  string(
    APPEND UNIFIED_CONFIG_CONTENT
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/dftracer_utilsTargets.cmake\")\n\n")

  # Add convenient aliases
  string(APPEND UNIFIED_CONFIG_CONTENT "# Create convenient aliases\n")
  if(DFTRACER_UTILS_BUILD_SHARED)
    string(APPEND UNIFIED_CONFIG_CONTENT
           "if(TARGET dftracer_utils::dftracer_utils_shared)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "  # dftracer_utils::shared\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET dftracer_utils::shared)\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(dftracer_utils::shared ALIAS dftracer_utils::dftracer_utils_shared)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  # dftracer_utils_shared (without namespace)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET dftracer_utils_shared)\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(dftracer_utils_shared ALIAS dftracer_utils::dftracer_utils_shared)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "endif()\n\n")
  endif()

  if(DFTRACER_UTILS_BUILD_STATIC)
    string(APPEND UNIFIED_CONFIG_CONTENT
           "if(TARGET dftracer_utils::dftracer_utils_static)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "  # dftracer_utils::static\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET dftracer_utils::static)\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(dftracer_utils::static ALIAS dftracer_utils::dftracer_utils_static)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  # dftracer_utils_static (without namespace)\n")
    string(APPEND UNIFIED_CONFIG_CONTENT
           "  if(NOT TARGET dftracer_utils_static)\n")
    string(
      APPEND
      UNIFIED_CONFIG_CONTENT
      "    add_library(dftracer_utils_static ALIAS dftracer_utils::dftracer_utils_static)\n"
    )
    string(APPEND UNIFIED_CONFIG_CONTENT "  endif()\n")
    string(APPEND UNIFIED_CONFIG_CONTENT "endif()\n\n")
  endif()

  string(APPEND UNIFIED_CONFIG_CONTENT
         "check_required_components(dftracer_utils)\n")

  # Write the config template
  set(UNIFIED_CONFIG_TEMPLATE
      "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utilsConfig.cmake.in")
  file(WRITE ${UNIFIED_CONFIG_TEMPLATE} ${UNIFIED_CONFIG_CONTENT})

  # Configure the config file
  include(CMakePackageConfigHelpers)
  set(UNIFIED_CONFIG_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utilsConfig.cmake")
  configure_package_config_file(
    ${UNIFIED_CONFIG_TEMPLATE} ${UNIFIED_CONFIG_FILE}
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils)

  # Create version file
  set(UNIFIED_VERSION_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utilsConfigVersion.cmake")
  write_basic_package_version_file(
    ${UNIFIED_VERSION_FILE}
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

  # Install config files
  install(FILES ${UNIFIED_CONFIG_FILE} ${UNIFIED_VERSION_FILE}
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils)

  # Install export targets
  install(
    EXPORT dftracer_utilsTargets
    FILE dftracer_utilsTargets.cmake
    NAMESPACE dftracer_utils::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils)

  # Create a unified pkg-config file
  create_pkgconfig_file(
    TARGET
    dftracer_utils
    NAME
    dftracer_utils
    VERSION
    ${PROJECT_VERSION}
    DESCRIPTION
    "DFTracer utils library (core + utilities)"
    URL
    "https://github.com/LLNL/dftracer-utils"
    REQUIRES
    "dftracer_utils_core dftracer_utils_utilities")

  # ==========================================================================
  # Create variant-specific wrapper configs for dftracer_utils_static and
  # dftracer_utils_shared These allow discovery via
  # "find_package(dftracer_utils_static)" and
  # "find_package(dftracer_utils_shared)"
  # ==========================================================================

  # dftracer_utils_shared wrapper config
  if(DFTRACER_UTILS_BUILD_SHARED)
    set(VARIANT_CONFIG_CONTENT "")
    string(APPEND VARIANT_CONFIG_CONTENT "@PACKAGE_INIT@\n\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "# dftracer_utils_shared wrapper config\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "# This simply includes the main dftracer_utils config\n\n")
    string(
      APPEND
      VARIANT_CONFIG_CONTENT
      "include(\${CMAKE_CURRENT_LIST_DIR}/../dftracer_utils/dftracer_utilsConfig.cmake)\n\n"
    )
    string(APPEND VARIANT_CONFIG_CONTENT "# Ensure the target is available\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "if(NOT TARGET dftracer_utils_shared)\n")
    string(
      APPEND
      VARIANT_CONFIG_CONTENT
      "  message(FATAL_ERROR \"dftracer_utils_shared target not found. Shared library may not have been built.\")\n"
    )
    string(APPEND VARIANT_CONFIG_CONTENT "endif()\n\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "check_required_components(dftracer_utils_shared)\n")

    set(VARIANT_CONFIG_TEMPLATE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_sharedConfig.cmake.in")
    file(WRITE ${VARIANT_CONFIG_TEMPLATE} ${VARIANT_CONFIG_CONTENT})

    set(VARIANT_CONFIG_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_sharedConfig.cmake")
    configure_package_config_file(
      ${VARIANT_CONFIG_TEMPLATE} ${VARIANT_CONFIG_FILE}
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils_shared)

    set(VARIANT_VERSION_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_sharedConfigVersion.cmake")
    write_basic_package_version_file(
      ${VARIANT_VERSION_FILE}
      VERSION ${PROJECT_VERSION}
      COMPATIBILITY SameMajorVersion)

    install(FILES ${VARIANT_CONFIG_FILE} ${VARIANT_VERSION_FILE}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils_shared)
  endif()

  # dftracer_utils_static wrapper config
  if(DFTRACER_UTILS_BUILD_STATIC)
    set(VARIANT_CONFIG_CONTENT "")
    string(APPEND VARIANT_CONFIG_CONTENT "@PACKAGE_INIT@\n\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "# dftracer_utils_static wrapper config\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "# This simply includes the main dftracer_utils config\n\n")
    string(
      APPEND
      VARIANT_CONFIG_CONTENT
      "include(\${CMAKE_CURRENT_LIST_DIR}/../dftracer_utils/dftracer_utilsConfig.cmake)\n\n"
    )
    string(APPEND VARIANT_CONFIG_CONTENT "# Ensure the target is available\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "if(NOT TARGET dftracer_utils_static)\n")
    string(
      APPEND
      VARIANT_CONFIG_CONTENT
      "  message(FATAL_ERROR \"dftracer_utils_static target not found. Static library may not have been built.\")\n"
    )
    string(APPEND VARIANT_CONFIG_CONTENT "endif()\n\n")
    string(APPEND VARIANT_CONFIG_CONTENT
           "check_required_components(dftracer_utils_static)\n")

    set(VARIANT_CONFIG_TEMPLATE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_staticConfig.cmake.in")
    file(WRITE ${VARIANT_CONFIG_TEMPLATE} ${VARIANT_CONFIG_CONTENT})

    set(VARIANT_CONFIG_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_staticConfig.cmake")
    configure_package_config_file(
      ${VARIANT_CONFIG_TEMPLATE} ${VARIANT_CONFIG_FILE}
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils_static)

    set(VARIANT_VERSION_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/dftracer_utils_staticConfigVersion.cmake")
    write_basic_package_version_file(
      ${VARIANT_VERSION_FILE}
      VERSION ${PROJECT_VERSION}
      COMPATIBILITY SameMajorVersion)

    install(FILES ${VARIANT_CONFIG_FILE} ${VARIANT_VERSION_FILE}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dftracer_utils_static)
  endif()
endif()

# Install headers: <includedir>/dftracer/utils/<subpath>, so a consumer
# includes <dftracer/utils/...>. A wheel sets the includedir to dftracer/include
# (beside dftracer/lib and dftracer/bin), so the same rule serves both.
install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../include/dftracer/utils/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dftracer/utils
  FILES_MATCHING
  PATTERN "*.h")

install(
  DIRECTORY ${CMAKE_BINARY_DIR}/include/dftracer/utils/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dftracer/utils
  FILES_MATCHING
  PATTERN "*.h")

# Public headers under include/ include these implementation headers that live
# in src/ (both trees share the <dftracer/utils/...> include path at build time).
# They must be installed to their mirrored public destination so the installed
# public headers are self-contained against the install prefix alone.
set(DFTRACER_UTILS_SRC_PUBLIC_HEADERS
    utilities/indexer/internal/helpers.h)

foreach(hdr ${DFTRACER_UTILS_SRC_PUBLIC_HEADERS})
  get_filename_component(hdr_dir ${hdr} DIRECTORY)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/${hdr}
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dftracer/utils/${hdr_dir})
endforeach()

# ##############################################################################
# Executables
# ##############################################################################

if(DFTRACER_UTILS_BUILD_BINARIES)
  set(DFTRACER_BINARIES
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_event_count.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_validate.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_pgzip.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_split.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_info.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_replay.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_index.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_gen_fake_trace.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_stats.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_view.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_run.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_server.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_comparator.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/dftracer_gen_dlio_config.cpp)

  set(DFTRACER_MPI_BINARIES "")

  foreach(bin ${DFTRACER_BINARIES})
    string(REPLACE ".cpp" "" bin_exec ${bin})
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/" ""
                   bin_exec ${bin_exec})

    add_executable(${bin_exec} ${bin})
    set_target_properties(
      ${bin_exec} PROPERTIES OUTPUT_NAME "${bin_exec}" RUNTIME_OUTPUT_DIRECTORY
                                                       ${CMAKE_BINARY_DIR}/bin)
    target_add_rpath(${bin_exec})

    # Link to unified library (which includes both core and utilities)
    target_link_libraries(${bin_exec} PRIVATE dftracer_utils argparse::argparse)

    target_include_directories(
      ${bin_exec} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                          ${CMAKE_CURRENT_SOURCE_DIR})

    add_stdfs_if_needed(${bin_exec})

    target_set_warnings(${bin_exec})

    # Apply coverage flags if enabled
    if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${bin_exec} PRIVATE --coverage -fprofile-arcs
                                                 -ftest-coverage)
      target_link_libraries(${bin_exec} PRIVATE --coverage)
    endif()

    # Install executable
    install(TARGETS ${bin_exec} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Create Python wrapper for venv/bin that calls the binary in site-packages
    if(SKBUILD)
      create_python_wrapper(${bin_exec})
    endif()
  endforeach()

  # dftracer_view is one source that compiles a distributed (MPI) path under
  # DFTRACER_UTILS_ENABLE_MPI, so it needs the MPI library when that is on.
  if(DFTRACER_UTILS_ENABLE_MPI AND TARGET dftracer_view)
    target_link_libraries(dftracer_view PRIVATE MPI::MPI_CXX)
  endif()

  # ----------------------------------------------------------------------------
  # Embed the single-file web UI into dftracer_server.
  #
  # The C++ build never requires Node. web/dist/ is git-ignored, so unless it
  # was built locally the embed step falls back to a placeholder page (the
  # /api/v1 REST API works either way). The full standalone UI comes from a
  # prior `npm run build`, the `web-ui` target, or DFTRACER_UTILS_BUILD_WEB_UI=ON
  # (needs npm). The VS Code extension bundles its own copy for portability.
  # ----------------------------------------------------------------------------
  if(TARGET dftracer_server)
    set(DFTRACER_WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../web)
    set(DFTRACER_WEB_DIST ${DFTRACER_WEB_DIR}/dist/index.html)
    set(DFTRACER_WEB_GENERATED
        ${CMAKE_BINARY_DIR}/generated/dftracer_web_assets.cpp)
    set(DFTRACER_API_DIST ${DFTRACER_WEB_DIR}/dist/api.html)
    set(DFTRACER_API_GENERATED
        ${CMAKE_BINARY_DIR}/generated/dftracer_api_assets.cpp)
    set(DFTRACER_EMBED_SCRIPT
        ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/scripts/embed_asset.cmake)

    find_program(NPM_EXECUTABLE npm)
    option(DFTRACER_UTILS_BUILD_WEB_UI
           "Rebuild the web UI with npm during the build (needs Node/npm)" OFF)

    if(NPM_EXECUTABLE)
      add_custom_target(
        web-ui
        COMMAND ${NPM_EXECUTABLE} install
        COMMAND ${NPM_EXECUTABLE} run build
        WORKING_DIRECTORY ${DFTRACER_WEB_DIR}
        COMMENT "Building DFTracer web UI (web/dist/index.html)"
        VERBATIM)
    endif()

    set(_embed_deps ${DFTRACER_EMBED_SCRIPT})
    if(DFTRACER_UTILS_BUILD_WEB_UI AND NPM_EXECUTABLE)
      add_custom_command(
        OUTPUT ${DFTRACER_WEB_DIST}
        COMMAND ${NPM_EXECUTABLE} install
        COMMAND ${NPM_EXECUTABLE} run build
        WORKING_DIRECTORY ${DFTRACER_WEB_DIR}
        COMMENT "Building DFTracer web UI (web/dist/index.html)"
        VERBATIM)
      list(APPEND _embed_deps ${DFTRACER_WEB_DIST})
    elseif(EXISTS ${DFTRACER_WEB_DIST})
      list(APPEND _embed_deps ${DFTRACER_WEB_DIST})
    endif()

    add_custom_command(
      OUTPUT ${DFTRACER_WEB_GENERATED}
      COMMAND
        ${CMAKE_COMMAND} -DINPUT=${DFTRACER_WEB_DIST}
        -DOUTPUT=${DFTRACER_WEB_GENERATED} -P ${DFTRACER_EMBED_SCRIPT}
      DEPENDS ${_embed_deps}
      COMMENT "Embedding web UI into dftracer_server"
      VERBATIM)

    set(_api_embed_deps ${DFTRACER_EMBED_SCRIPT})
    if(EXISTS ${DFTRACER_API_DIST})
      list(APPEND _api_embed_deps ${DFTRACER_API_DIST})
    endif()
    add_custom_command(
      OUTPUT ${DFTRACER_API_GENERATED}
      COMMAND
        ${CMAKE_COMMAND} -DINPUT=${DFTRACER_API_DIST}
        -DOUTPUT=${DFTRACER_API_GENERATED} -DFUNC=viz_api_page_html
        -DVAR=VIZ_API_PAGE_HTML -P ${DFTRACER_EMBED_SCRIPT}
      DEPENDS ${_api_embed_deps}
      COMMENT "Embedding API explorer page into dftracer_server"
      VERBATIM)

    target_sources(
      dftracer_server
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/server/viz/ui.cpp
              ${DFTRACER_WEB_GENERATED} ${DFTRACER_API_GENERATED})
  endif()

  foreach(bin ${DFTRACER_MPI_BINARIES})
    string(REPLACE ".cpp" "" bin_exec ${bin})
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/binaries/" ""
                   bin_exec ${bin_exec})

    add_executable(${bin_exec} ${bin})
    set_target_properties(
      ${bin_exec} PROPERTIES OUTPUT_NAME "${bin_exec}" RUNTIME_OUTPUT_DIRECTORY
                                                       ${CMAKE_BINARY_DIR}/bin)
    target_add_rpath(${bin_exec})

    target_link_libraries(${bin_exec}
                          PRIVATE dftracer_utils argparse::argparse
                                  MPI::MPI_CXX)
    target_include_directories(
      ${bin_exec} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                          ${CMAKE_CURRENT_SOURCE_DIR})
    add_stdfs_if_needed(${bin_exec})
    target_set_warnings(${bin_exec})

    if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${bin_exec} PRIVATE --coverage -fprofile-arcs
                                                 -ftest-coverage)
      target_link_libraries(${bin_exec} PRIVATE --coverage)
    endif()

    install(TARGETS ${bin_exec} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(SKBUILD)
      create_python_wrapper(${bin_exec})
    endif()
  endforeach()
endif()

# ##############################################################################
# API Examples (optional)
# ##############################################################################

option(DFTRACER_UTILS_BUILD_EXAMPLES "Build API examples" ON)

if(DFTRACER_UTILS_BUILD_EXAMPLES)
  set(DFTRACER_EXAMPLES
      ${CMAKE_CURRENT_SOURCE_DIR}/../examples/view_containment_example.cpp)

  foreach(example ${DFTRACER_EXAMPLES})
    get_filename_component(example_name ${example} NAME_WE)

    add_executable(${example_name} ${example})
    set_target_properties(${example_name} PROPERTIES OUTPUT_NAME "${example_name}")
    target_add_rpath(${example_name})
    target_link_libraries(
      ${example_name} PRIVATE dftracer_utils argparse::argparse)

    target_include_directories(${example_name}
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

    add_stdfs_if_needed(${example_name})

    target_set_warnings(${example_name})

    # Apply coverage flags if enabled
    if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${example_name} PRIVATE --coverage -fprofile-arcs
                                                     -ftest-coverage)
      target_link_libraries(${example_name} PRIVATE --coverage)
    endif()

    # Install example
    install(TARGETS ${example_name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/examples)
  endforeach()

  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../examples/plugins
                   ${CMAKE_BINARY_DIR}/examples/plugins)
endif()

# Standalone benchmarks (manual; excluded from ctest/CI by default).
option(DFTRACER_UTILS_BUILD_BENCHMARKS "Build standalone benchmarks" OFF)

if(DFTRACER_UTILS_BUILD_BENCHMARKS)
  set(DFTRACER_BENCHMARKS
      ${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/member_cache_bench.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/dataframe_parallel_bench.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/view_api_bench.cpp)

  foreach(bench ${DFTRACER_BENCHMARKS})
    get_filename_component(bench_name ${bench} NAME_WE)
    add_executable(${bench_name} ${bench})
    target_add_rpath(${bench_name})
    target_link_libraries(${bench_name} PRIVATE dftracer_utils)
    target_include_directories(
      ${bench_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                            ${CMAKE_CURRENT_SOURCE_DIR})
    add_stdfs_if_needed(${bench_name})
    target_set_warnings(${bench_name})
  endforeach()
endif()

# ##############################################################################
# Python binding
# ##############################################################################

if(CMAKE_VERSION VERSION_LESS 3.18)
  set(DEV_MODULE Development)
else()
  set(DEV_MODULE Development.Module)
endif()

find_package(
  Python 3.8
  COMPONENTS Interpreter ${DEV_MODULE}
  OPTIONAL_COMPONENTS Development.SABIModule)

if(DFTRACER_UTILS_BUILD_PYTHON)
  if(NOT Python_FOUND)
    message(
      FATAL_ERROR "Python was not found, but DFTRACER_UTILS_BUILD_PYTHON is ON")
  endif()

  if(NOT DFTRACER_UTILS_BUILD_SHARED)
    message(
      FATAL_ERROR
        "Python bindings require shared library build (DFTRACER_UTILS_BUILD_SHARED must be ON)"
    )
  endif()

  dftracer_utils_ok("Building Python bindings")

  # Python C API binding sources
  python_add_library(
    dftracer_utils_ext
    MODULE
    WITH_SOABI
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/dftracer_utils_ext.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/py_errors.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/py_errors.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/indexer.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/indexer.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/index_database.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/index_database.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/sst_distribution.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/sst_distribution.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/series.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/series.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/series_detail.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/series/arith.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/series/math.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/series/window.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/series/stats.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/series/reduce.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/series/string.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/series/arrow.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/dataframe.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/dataframe.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/lazyframe.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/lazyframe.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/columnar_eval.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/columnar_eval.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/py_scalar_helpers.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/py_agg_helpers.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/batch_indexer.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/batch_indexer.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/trace_viewer.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/trace_viewer.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/plugin_host.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/op_runner.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/ops.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/ops.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/plugin_host.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/runtime.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/runtime.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/task_handle.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/task_handle.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/memoryview_batch.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/memoryview_batch.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/trace_reader_iterator.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/trace_reader_iterator.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/json.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/json.h
       ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/arrow_helpers.cpp
       ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/arrow_helpers.h
       ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/arrow_stream_capsule.cpp
       ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/arrow_stream_capsule.h
       ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/schema_reconcile.cpp
       ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/schema_reconcile.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/streaming_iterator.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/streaming_iterator.h
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/arrow_parallel_reader.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/dftracer/utils/python/arrow_parallel_reader.h)

  # Link to unified library to test if this fixes the bus error
  target_link_libraries(dftracer_utils_ext PRIVATE dftracer_utils::shared)

  if(DFTRACER_UTILS_ENABLE_ARROW)
    link_nanoarrow(dftracer_utils_ext SHARED)
  endif()

  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dftracer_utils_ext PRIVATE --coverage)
    target_link_options(dftracer_utils_ext PRIVATE --coverage)
  endif()

  set_target_properties(
    dftracer_utils_ext PROPERTIES LIBRARY_OUTPUT_DIRECTORY
                                  "${CMAKE_BINARY_DIR}/dftracer/utils")

  # The package layout: a wheel or an editable install, never a plain
  # `cmake --install`, whose prefix nothing on sys.path reads.
  if(SKBUILD)
    install(TARGETS dftracer_utils_ext LIBRARY DESTINATION "dftracer/utils")

    # Editable installs place the .so in site-packages but serve .py files
    # from the source tree via .pth redirect. Install the .pyi stub next
    # to the .so so sphinx-autodoc-typehints and IDEs can find it.
    # (Production wheels already include these via wheel.packages.)
    install(
      FILES
        "${CMAKE_SOURCE_DIR}/python/dftracer/utils/dftracer_utils_ext.pyi"
        "${CMAKE_SOURCE_DIR}/python/dftracer/utils/py.typed"
      DESTINATION "dftracer/utils")
  endif()

endif()
