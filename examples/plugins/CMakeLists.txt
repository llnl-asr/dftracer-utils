# Example plugins built as loadable modules against the public plugin ABI only
# (the include/ headers); they deliberately link none of the internal libraries,
# so a broken build here is real ABI rot in abi.h / plugin.h.

# Generated headers (abi_version.h) land under the build tree, not include/.
include_directories(${CMAKE_BINARY_DIR}/include)

# Reads the batch dataframe directly (dftu_series_data / dftu_series_type), so
# it links the dataframe C ABI like dur_stats_consumer does.
add_library(event_counter_plugin MODULE event_counter.c)
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

add_library(query_filter_plugin MODULE query_filter.cpp)
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

# Reads the batch dataframe directly (dftu_series_data / dftu_series_type), so
# it links the dataframe C ABI like dur_stats_consumer does.
add_library(perbatch_producer_plugin MODULE perbatch_producer.c)
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
# dataframe C ABI. C11 because it includes both abi.h headers, which repeat the
# dftu_series / dftu_dataframe typedefs.
add_library(dur_stats_consumer_plugin MODULE dur_stats_consumer.c)
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
set_target_properties(
  process_regr_stats_plugin
  PROPERTIES OUTPUT_NAME process_regr_stats
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 99
             C_STANDARD_REQUIRED ON)
target_include_directories(process_regr_stats_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include)

install(TARGETS event_counter_plugin duration_histogram_plugin
                query_filter_plugin
                perbatch_producer_plugin perbatch_consumer_plugin
                dur_stats_producer_plugin dur_stats_consumer_plugin
                process_counts_plugin process_file_edges_plugin
                process_collections_plugin process_pid_argstats_plugin
                process_topk_plugin process_regr_stats_plugin
        LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}/examples/plugins)
