# SPDX-License-Identifier: MIT
# Example plugins built as loadable modules against the public plugin ABI only
# (the include/ headers); they deliberately link none of the internal libraries,
# so a broken build here is real ABI rot in abi.h / plugin.h.

# Generated headers (abi_version.h) land under the build tree, not include/.
include_directories(${CMAKE_BINARY_DIR}/include)

# Reads the batch dataframe directly (dftu_series_data / dftu_series_type), so
# it links the dataframe C ABI like dur_stats_consumer does.
add_library(event_counter_plugin MODULE event_counter.c)
target_set_warnings(event_counter_plugin)
set_target_properties(
  event_counter_plugin
  PROPERTIES OUTPUT_NAME event_counter
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 11
             C_STANDARD_REQUIRED ON)
target_include_directories(event_counter_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(event_counter_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(event_counter_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

add_library(duration_histogram_plugin MODULE duration_histogram.cpp)
target_set_warnings(duration_histogram_plugin)
set_target_properties(
  duration_histogram_plugin
  PROPERTIES OUTPUT_NAME duration_histogram
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_include_directories(duration_histogram_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_enable_coroutine(duration_histogram_plugin)
# plugins::Batch/Event (plugin.h) resolve columns through the dataframe C ABI.
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(duration_histogram_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(duration_histogram_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

# Writes its report through the host fileio service.
add_library(duration_report_plugin MODULE duration_report.cpp)
target_set_warnings(duration_report_plugin)
set_target_properties(
  duration_report_plugin
  PROPERTIES OUTPUT_NAME duration_report
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_include_directories(duration_report_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_enable_coroutine(duration_report_plugin)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(duration_report_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(duration_report_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

# Registers scalar UDFs; reads the argument columns through the dataframe C ABI.
add_library(duration_udfs_plugin MODULE duration_udfs.cpp)
target_set_warnings(duration_udfs_plugin)
set_target_properties(
  duration_udfs_plugin
  PROPERTIES OUTPUT_NAME duration_udfs
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_include_directories(duration_udfs_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_enable_coroutine(duration_udfs_plugin)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(duration_udfs_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(duration_udfs_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

# Registers user-defined aggregates; reads the argument columns through the
# dataframe C ABI.
add_library(stats_udafs_plugin MODULE stats_udafs.cpp)
target_set_warnings(stats_udafs_plugin)
set_target_properties(
  stats_udafs_plugin
  PROPERTIES OUTPUT_NAME stats_udafs
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_include_directories(stats_udafs_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_enable_coroutine(stats_udafs_plugin)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(stats_udafs_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(stats_udafs_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

add_library(query_filter_plugin MODULE query_filter.cpp)
target_set_warnings(query_filter_plugin)
set_target_properties(
  query_filter_plugin
  PROPERTIES OUTPUT_NAME query_filter
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_include_directories(query_filter_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_enable_coroutine(query_filter_plugin)
# plugins::Batch/Event (plugin.h) resolve columns through the dataframe C ABI.
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(query_filter_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(query_filter_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

# Registers the sink "csv": it writes every frame it is given to the file its
# path option names, so it reads the batch through the dataframe C ABI.
add_library(csv_sink_plugin MODULE csv_sink.cpp)
target_set_warnings(csv_sink_plugin)
set_target_properties(
  csv_sink_plugin
  PROPERTIES OUTPUT_NAME csv
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_include_directories(csv_sink_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_enable_coroutine(csv_sink_plugin)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(csv_sink_plugin PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(csv_sink_plugin PRIVATE dftracer_utils_dataframe::static)
endif()

# Registers the file format "csv_reader": it claims a .csv file, names its
# columns from the header line and builds each chunk's frame through the
# dataframe C ABI.
add_library(csv_reader_plugin MODULE csv_reader.cpp)
target_set_warnings(csv_reader_plugin)
set_target_properties(
  csv_reader_plugin
  PROPERTIES OUTPUT_NAME csv_reader
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_include_directories(csv_reader_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_enable_coroutine(csv_reader_plugin)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(csv_reader_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(csv_reader_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

# Registers the plan node "sessionize.gaps": it appends an int64 `session`
# column, copying the input columns through the dataframe C ABI.
add_library(sessionize_plugin MODULE sessionize.cpp)
target_set_warnings(sessionize_plugin)
set_target_properties(
  sessionize_plugin
  PROPERTIES OUTPUT_NAME sessionize
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_include_directories(sessionize_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_enable_coroutine(sessionize_plugin)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(sessionize_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(sessionize_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

# Declares a phase result that the host seeds with the earliest event time of the
# whole input, and reads it from the first batch through the ports service.
add_library(two_phase_normalize_plugin MODULE two_phase_normalize.cpp)
target_set_warnings(two_phase_normalize_plugin)
set_target_properties(
  two_phase_normalize_plugin
  PROPERTIES OUTPUT_NAME two_phase_normalize
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_include_directories(two_phase_normalize_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_enable_coroutine(two_phase_normalize_plugin)
# plugins::Batch/Event (plugin.h) resolve columns through the dataframe C ABI.
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(two_phase_normalize_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(two_phase_normalize_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

# Registers the provider "range_provider.numbers": a table built in code, with
# its frames built through the dataframe C ABI.
add_library(range_provider_plugin MODULE range_provider.cpp)
target_set_warnings(range_provider_plugin)
set_target_properties(
  range_provider_plugin
  PROPERTIES OUTPUT_NAME range_provider
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             CXX_STANDARD 20
             CXX_STANDARD_REQUIRED ON
             CXX_EXTENSIONS OFF)
target_include_directories(range_provider_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_enable_coroutine(range_provider_plugin)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(range_provider_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(range_provider_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

# Reads the batch dataframe directly (dftu_series_data / dftu_series_type), so
# it links the dataframe C ABI like dur_stats_consumer does.
add_library(perbatch_producer_plugin MODULE perbatch_producer.c)
target_set_warnings(perbatch_producer_plugin)
set_target_properties(
  perbatch_producer_plugin
  PROPERTIES OUTPUT_NAME perbatch_producer
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 11
             C_STANDARD_REQUIRED ON)
target_include_directories(perbatch_producer_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(perbatch_producer_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(perbatch_producer_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

add_library(perbatch_consumer_plugin MODULE perbatch_consumer.c)
target_set_warnings(perbatch_consumer_plugin)
set_target_properties(
  perbatch_consumer_plugin
  PROPERTIES OUTPUT_NAME perbatch_consumer
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 99
             C_STANDARD_REQUIRED ON)
target_include_directories(perbatch_consumer_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(dur_stats_producer_plugin MODULE dur_stats_producer.c)
target_set_warnings(dur_stats_producer_plugin)
set_target_properties(
  dur_stats_producer_plugin
  PROPERTIES OUTPUT_NAME dur_stats_producer
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 99
             C_STANDARD_REQUIRED ON)
target_include_directories(dur_stats_producer_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)

# The one example that reads a result frame, so the one that links the
# dataframe C ABI. C11 because it includes both abi.h and c_api.h headers, which repeat the
# dftu_series / dftu_dataframe typedefs.
add_library(dur_stats_consumer_plugin MODULE dur_stats_consumer.c)
target_set_warnings(dur_stats_consumer_plugin)
set_target_properties(
  dur_stats_consumer_plugin
  PROPERTIES OUTPUT_NAME dur_stats_consumer
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 11
             C_STANDARD_REQUIRED ON)
target_include_directories(dur_stats_consumer_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(dur_stats_consumer_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(dur_stats_consumer_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

# Reads the batch dataframe directly (dftu_series_data / dftu_series_type), so
# it links the dataframe C ABI like dur_stats_consumer does.
add_library(process_counts_plugin MODULE process_counts.c)
target_set_warnings(process_counts_plugin)
set_target_properties(
  process_counts_plugin
  PROPERTIES OUTPUT_NAME process_counts
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 11
             C_STANDARD_REQUIRED ON)
target_include_directories(process_counts_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(process_counts_plugin
                        PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(process_counts_plugin
                        PRIVATE dftracer_utils_dataframe::static)
endif()

add_library(process_file_edges_plugin MODULE process_file_edges.c)
target_set_warnings(process_file_edges_plugin)
set_target_properties(
  process_file_edges_plugin
  PROPERTIES OUTPUT_NAME process_file_edges
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 99
             C_STANDARD_REQUIRED ON)
target_include_directories(process_file_edges_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(process_collections_plugin MODULE process_collections.c)
target_set_warnings(process_collections_plugin)
set_target_properties(
  process_collections_plugin
  PROPERTIES OUTPUT_NAME process_collections
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 99
             C_STANDARD_REQUIRED ON)
target_include_directories(process_collections_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(process_pid_argstats_plugin MODULE process_pid_argstats.c)
target_set_warnings(process_pid_argstats_plugin)
set_target_properties(
  process_pid_argstats_plugin
  PROPERTIES OUTPUT_NAME process_pid_argstats
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 99
             C_STANDARD_REQUIRED ON)
target_include_directories(process_pid_argstats_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(process_topk_plugin MODULE process_topk.c)
target_set_warnings(process_topk_plugin)
set_target_properties(
  process_topk_plugin
  PROPERTIES OUTPUT_NAME process_topk
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 99
             C_STANDARD_REQUIRED ON)
target_include_directories(process_topk_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(process_regr_stats_plugin MODULE process_regr_stats.c)
target_set_warnings(process_regr_stats_plugin)
set_target_properties(
  process_regr_stats_plugin
  PROPERTIES OUTPUT_NAME process_regr_stats
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 99
             C_STANDARD_REQUIRED ON)
target_include_directories(process_regr_stats_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)

# The one example that may include nanoarrow: it turns a column into an
# ArrowSchema/ArrowArray pair and back with nanoarrow's own zero-copy buffers,
# so it links the dataframe C ABI plus nanoarrow and nothing from the arrow
# library. tests/CMakeLists.txt runs the binary as the arrow_convert test.
add_executable(arrow_convert arrow_convert.c)
target_set_warnings(arrow_convert)
set_target_properties(
  arrow_convert
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED ON)
target_add_rpath(arrow_convert)
target_include_directories(arrow_convert PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(arrow_convert PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(arrow_convert PRIVATE dftracer_utils_dataframe::static)
endif()
if(DFTRACER_UTILS_BUILD_SHARED)
  link_nanoarrow(arrow_convert SHARED)
else()
  link_nanoarrow(arrow_convert STATIC)
endif()

install(TARGETS event_counter_plugin duration_histogram_plugin
                duration_report_plugin duration_udfs_plugin stats_udafs_plugin
                query_filter_plugin csv_sink_plugin csv_reader_plugin
                sessionize_plugin two_phase_normalize_plugin
                range_provider_plugin
                perbatch_producer_plugin perbatch_consumer_plugin
                dur_stats_producer_plugin dur_stats_consumer_plugin
                process_counts_plugin process_file_edges_plugin
                process_collections_plugin process_pid_argstats_plugin
                process_topk_plugin process_regr_stats_plugin
        LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}/examples/plugins)
