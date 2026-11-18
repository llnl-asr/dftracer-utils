# SPDX-License-Identifier: MIT
# ##############################################################################
# Plugin Host Tests
# ##############################################################################

set(PLUGINS_TEST_SOURCES
    test_plugin_host_services.cpp
    test_plugin_io_writer.cpp
    test_plugin_cxx_ergonomics.cpp
    test_plugin_cxx_wrappers.cpp
    test_plugin_abi_features.cpp
    test_plugin_ports.cpp
    test_plugin_ports_sdk.cpp
    test_plugin_phase_seed.cpp
    test_plugin_results.cpp
    test_plugin_agg.cpp
    test_plugin_agg_spill.cpp
    test_plugin_ops.cpp
    test_plugin_udf.cpp
    test_plugin_udaf.cpp
    test_plugin_udaf_groupby.cpp
    test_plugin_udaf_duql.cpp
    test_plugin_udaf_window.cpp
    test_plugin_lazyframe_facade.cpp
    test_plugin_planner.cpp
    test_plugin_order.cpp
    test_plugin_transform.cpp
    test_plugin_abi_gate.cpp
    test_plugin_callback_errors.cpp
    test_plugin_index_extension.cpp
    test_plugin_factory_host.cpp
    test_plugin_config.cpp
    test_plugin_public_config.cpp
    test_plugin_state.cpp
    test_plugin_memory.cpp
    test_plugin_reads.cpp
    test_plugin_providers.cpp
    test_plugin_names.cpp
    test_plugin_describe.cpp
    test_plugin_frame_check.cpp
    test_plugin_frame_check_off.cpp
    test_plugin_file_formats.cpp
    test_global_plugins.cpp
    test_plugin_format_index.cpp
    test_plugin_explain.cpp
    test_plugin_sink_duql.cpp
    test_plugin_node_duql.cpp
    test_provider_pushdown.cpp
    test_plugin_library_lifetime.cpp
    test_plugin_borrowed_column.cpp
    test_example_sessionize.cpp
    test_example_two_phase_normalize.cpp
    test_example_csv_reader.cpp
    test_provider_schema_types.cpp
    test_example_range_provider.cpp
    test_lazyframe_plugin_node.cpp
    test_abi_task.cpp
    test_source_hooks_vtable.cpp
    test_plugin_reference.cpp
    test_plugin_entry_macro.cpp
    test_plugin_entry_cleanup.cpp
    host/test_host_ops.cpp
    host/test_host_ops_autoregister.cpp
    plugin/test_plugin_sdk.cpp
    plugin/test_plugin_sdk_types.cpp
    plugin/test_plugin_frame.cpp
)

foreach(test_file ${PLUGINS_TEST_SOURCES})
  # Extract path and filename
  string(REPLACE ".cpp" "" bin_exec ${test_file})

  # Create valid target name (replace / with _)
  string(REPLACE "/" "_" target_name "plugins_${bin_exec}")

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

  add_test(NAME plugins/${bin_exec} COMMAND ${target_name})
  set_tests_properties(plugins/${bin_exec} PROPERTIES
                       WORKING_DIRECTORY "${test_working_directory}")
endforeach()

# +++++++++++++++++++++++++++++++++++++++++
# Plugin C-ABI test: per-ABI C consumers (compiled as C) driven by a C++ host
# harness, so the plugin C ABIs are exercised from real C, not just C++.
# +++++++++++++++++++++++++++++++++++++++++

set(cabi_target plugins_test_plugin_c_abi)
set(cabi_sources test_plugin_c_abi.cpp
                 task_abi_c_consumer.c
                 duql_abi_c_consumer.c
                 sketch_abi_c_consumer.c
                 ops_abi_c_consumer.c
                 trace_abi_c_consumer.c
                 io_abi_c_consumer.c
                 fileio_abi_c_consumer.c
                 fs_abi_c_consumer.c
                 format_abi_c_consumer.c
                 file_formats_abi_c_consumer.c
                 memory_abi_c_consumer.c)
add_executable(${cabi_target} ${cabi_sources})
target_include_directories(${cabi_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(${cabi_target} PRIVATE doctest::doctest dftracer_utils
                                             testing_utilities)
target_enable_coroutine(${cabi_target})
target_set_warnings(${cabi_target})
set_target_properties(${cabi_target} PROPERTIES OUTPUT_NAME test_plugin_c_abi
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME plugins/test_plugin_c_abi COMMAND ${cabi_target})

# +++++++++++++++++++++++++++++++++++++++++
# dftu.svc.store: a C consumer writes through the store C ABI and a C++ host
# drives it, so the forwarding and the namespace rule are exercised from C.
# +++++++++++++++++++++++++++++++++++++++++

set(store_target plugins_test_plugin_store)
add_executable(${store_target} test_plugin_store.cpp store_abi_c_consumer.c)
target_include_directories(${store_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(${store_target} PRIVATE doctest::doctest dftracer_utils
                                                 testing_utilities)
target_enable_coroutine(${store_target})
target_set_warnings(${store_target})
set_target_properties(${store_target} PROPERTIES OUTPUT_NAME test_plugin_store
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME plugins/test_plugin_store COMMAND ${store_target})

# +++++++++++++++++++++++++++++++++++++++++
# dftu.svc.ports: a C consumer calls the channel, latest and phase members and
# a C++ test sequences them on folds that share one hub.
# +++++++++++++++++++++++++++++++++++++++++

set(ports_abi_target plugins_test_plugin_ports_abi)
add_executable(${ports_abi_target} test_plugin_ports_abi.cpp
                                   ports_abi_c_consumer.c)
target_include_directories(${ports_abi_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(${ports_abi_target} PRIVATE doctest::doctest dftracer_utils
                                                  testing_utilities)
target_enable_coroutine(${ports_abi_target})
target_set_warnings(${ports_abi_target})
set_target_properties(${ports_abi_target} PROPERTIES OUTPUT_NAME test_plugin_ports_abi
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME plugins/test_plugin_ports_abi COMMAND ${ports_abi_target})

# +++++++++++++++++++++++++++++++++++++++++
# ABI gate: real .so files loaded by plugins/test_plugin_abi_gate through a
# real dlopen - a wrong DFTRACER_UTILS_PLUGIN_ABI_VERSION, no info symbol, a
# plugin descriptor below the first published size, and one that omits the
# last optional callback and must still run.
# +++++++++++++++++++++++++++++++++++++++++

foreach(gate_plugin bad_abi_version_plugin no_info_plugin short_plugin
                    too_small_plugin short_info_plugin oversize_plugin
                    oversize_udf_plugin)
  add_library(${gate_plugin} MODULE ${gate_plugin}.c)
  target_set_warnings(${gate_plugin})
  set_target_properties(
    ${gate_plugin}
    PROPERTIES OUTPUT_NAME ${gate_plugin}
               PREFIX ""
               POSITION_INDEPENDENT_CODE ON
               C_STANDARD 11
               C_STANDARD_REQUIRED ON
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_include_directories(${gate_plugin}
                             PRIVATE ${CMAKE_SOURCE_DIR}/include
                                     ${CMAKE_BINARY_DIR}/include)
endforeach()

if(APPLE)
  target_link_options(short_plugin PRIVATE -undefined dynamic_lookup)
endif()

if(TARGET plugins_test_plugin_abi_gate)
  target_compile_definitions(
    plugins_test_plugin_abi_gate
    PRIVATE BAD_ABI_VERSION_PLUGIN_PATH="$<TARGET_FILE:bad_abi_version_plugin>"
            NO_INFO_PLUGIN_PATH="$<TARGET_FILE:no_info_plugin>"
            SHORT_PLUGIN_PATH="$<TARGET_FILE:short_plugin>"
            TOO_SMALL_PLUGIN_PATH="$<TARGET_FILE:too_small_plugin>"
            SHORT_INFO_PLUGIN_PATH="$<TARGET_FILE:short_info_plugin>"
            OVERSIZE_PLUGIN_PATH="$<TARGET_FILE:oversize_plugin>"
            OVERSIZE_UDF_PLUGIN_PATH="$<TARGET_FILE:oversize_udf_plugin>")
  add_dependencies(plugins_test_plugin_abi_gate bad_abi_version_plugin
                   no_info_plugin short_plugin too_small_plugin
                   short_info_plugin oversize_plugin oversize_udf_plugin)
endif()

if(TARGET plugins_test_plugin_index_extension)
  target_compile_definitions(
    plugins_test_plugin_index_extension PRIVATE
    INDEX_MINMAX_PLUGIN_PATH="$<TARGET_FILE:index_minmax_plugin>"
    INDEX_COLUMNS_PLUGIN_PATH="$<TARGET_FILE:index_columns_plugin>"
    INDEX_NO_STEP_PLUGIN_PATH="$<TARGET_FILE:index_no_step_plugin>")
  add_dependencies(plugins_test_plugin_index_extension
                   index_minmax_plugin index_columns_plugin
                   index_no_step_plugin)
endif()

# +++++++++++++++++++++++++++++++++++++++++
# Build-phase host: one plugin whose factory registers an op at load, one whose
# factory reaches past the registration surface. Both are real .so files loaded
# through Plugins::Builder by plugins/test_plugin_factory_host.
# +++++++++++++++++++++++++++++++++++++++++

foreach(fh_plugin toy_format_plugin toy_format_tie_plugin
                  chunked_format_plugin toy_bad_sink_plugin
                  factory_registers_op_plugin factory_escapes_plugin
                  scan_registers_op_plugin index_minmax_plugin
                  index_columns_plugin index_no_step_plugin udf_plugin
                  udf_clash_plugin udaf_plugin udaf_bad_plugin
                  leaky_plugin crash_plugin reject_destroy_plugin
                  explain_plugin)
  add_library(${fh_plugin} MODULE ${fh_plugin}.c)
  target_set_warnings(${fh_plugin})
  set_target_properties(
    ${fh_plugin}
    PROPERTIES OUTPUT_NAME ${fh_plugin}
               PREFIX ""
               POSITION_INDEPENDENT_CODE ON
               C_STANDARD 11
               C_STANDARD_REQUIRED ON
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_include_directories(${fh_plugin}
                             PRIVATE ${CMAKE_SOURCE_DIR}/include
                                     ${CMAKE_BINARY_DIR}/include)
endforeach()

# The same plugin built as another name, so a test can change a file's owner by
# loading the other one.
add_library(chunked_alt_plugin MODULE chunked_format_plugin.c)
target_set_warnings(chunked_alt_plugin)
set_target_properties(
  chunked_alt_plugin
  PROPERTIES OUTPUT_NAME chunked_alt_plugin
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 11
             C_STANDARD_REQUIRED ON
             RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
             LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(chunked_alt_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include
                                   ${CMAKE_BINARY_DIR}/include)
target_compile_definitions(chunked_alt_plugin PRIVATE CHUNKED_FORMAT_ALT)

# The same plugin name at a newer version, so a test can upgrade a plugin.
add_library(chunked_v2_plugin MODULE chunked_format_plugin.c)
target_set_warnings(chunked_v2_plugin)
set_target_properties(
  chunked_v2_plugin
  PROPERTIES OUTPUT_NAME chunked_v2_plugin
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 11
             C_STANDARD_REQUIRED ON
             RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
             LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(chunked_v2_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include
                                   ${CMAKE_BINARY_DIR}/include)
target_compile_definitions(chunked_v2_plugin PRIVATE CHUNKED_FORMAT_V2)

foreach(cxx_plugin builder_plugin config_keys_plugin reads_projection_plugin
                  provider_fixture_plugin toy_sink_plugin toy_node_plugin
                  broken_frame_plugin borrowed_frame_plugin
                  sessionize_input_plugin macro_plugin macro_error_plugin
                  error_after_register_plugin empty_config_key_plugin
                  sdk_chunked_format_plugin sdk_explain_plugin)
  add_library(${cxx_plugin} MODULE ${cxx_plugin}.cpp)
  target_set_warnings(${cxx_plugin})
  set_target_properties(
    ${cxx_plugin}
    PROPERTIES OUTPUT_NAME ${cxx_plugin}
               PREFIX ""
               POSITION_INDEPENDENT_CODE ON
               CXX_STANDARD 20
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_include_directories(${cxx_plugin}
                             PRIVATE ${CMAKE_SOURCE_DIR}/include
                                     ${CMAKE_BINARY_DIR}/include)
  target_enable_coroutine(${cxx_plugin})
  # GCC binds the statics of inline functions and templates as GNU unique
  # symbols, which keep a plugin mapped after the host unloads it.
  target_compile_options(${cxx_plugin}
                         PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fno-gnu-unique>)
endforeach()

# It reads the batch dataframe and runs its own registered op, so it links the
# dataframe C ABI like the examples that do the same.
foreach(df_plugin toy_format_plugin toy_format_tie_plugin
                  chunked_format_plugin chunked_alt_plugin chunked_v2_plugin toy_sink_plugin
                  toy_node_plugin
                  factory_registers_op_plugin builder_plugin udf_plugin
                  udaf_plugin
                  config_keys_plugin reads_projection_plugin
                  provider_fixture_plugin broken_frame_plugin
                  borrowed_frame_plugin sessionize_input_plugin
                  macro_plugin macro_error_plugin
                  error_after_register_plugin empty_config_key_plugin
                  leaky_plugin crash_plugin index_columns_plugin
                  sdk_chunked_format_plugin sdk_explain_plugin explain_plugin)
  if(TARGET dftracer_utils_dataframe::shared)
    target_link_libraries(${df_plugin} PRIVATE dftracer_utils_dataframe::shared)
  elseif(TARGET dftracer_utils_dataframe::static)
    target_link_libraries(${df_plugin} PRIVATE dftracer_utils_dataframe::static)
  endif()
endforeach()

if(TARGET plugins_test_plugin_config)
  target_compile_definitions(
    plugins_test_plugin_config PRIVATE
    CONFIG_KEYS_PLUGIN_PATH="$<TARGET_FILE:config_keys_plugin>"
    EMPTY_CONFIG_KEY_PLUGIN_PATH="$<TARGET_FILE:empty_config_key_plugin>")
  add_dependencies(plugins_test_plugin_config config_keys_plugin
                   empty_config_key_plugin)
endif()

if(TARGET plugins_test_plugin_public_config)
  target_compile_definitions(
    plugins_test_plugin_public_config PRIVATE
    CONFIG_KEYS_PLUGIN_PATH="$<TARGET_FILE:config_keys_plugin>")
  add_dependencies(plugins_test_plugin_public_config
                   config_keys_plugin)
endif()

if(TARGET plugins_test_plugin_reads)
  target_compile_definitions(
    plugins_test_plugin_reads PRIVATE
    READS_PROJECTION_PLUGIN_PATH="$<TARGET_FILE:reads_projection_plugin>")
  add_dependencies(plugins_test_plugin_reads
                   reads_projection_plugin)
endif()

if(TARGET plugins_test_plugin_entry_macro)
  target_compile_definitions(
    plugins_test_plugin_entry_macro
    PRIVATE MACRO_PLUGIN_PATH="$<TARGET_FILE:macro_plugin>"
            MACRO_ERROR_PLUGIN_PATH="$<TARGET_FILE:macro_error_plugin>")
  add_dependencies(plugins_test_plugin_entry_macro macro_plugin
                   macro_error_plugin)
endif()

if(TARGET plugins_test_plugin_entry_cleanup)
  target_compile_definitions(
    plugins_test_plugin_entry_cleanup
    PRIVATE ERROR_AFTER_REGISTER_PLUGIN_PATH="$<TARGET_FILE:error_after_register_plugin>")
  add_dependencies(plugins_test_plugin_entry_cleanup
                   error_after_register_plugin)
endif()

if(TARGET plugins_test_plugin_factory_host)
  target_compile_definitions(
    plugins_test_plugin_factory_host PRIVATE
    FACTORY_REGISTERS_OP_PLUGIN_PATH="$<TARGET_FILE:factory_registers_op_plugin>"
    FACTORY_ESCAPES_PLUGIN_PATH="$<TARGET_FILE:factory_escapes_plugin>"
    SCAN_REGISTERS_OP_PLUGIN_PATH="$<TARGET_FILE:scan_registers_op_plugin>"
    REJECT_DESTROY_PLUGIN_PATH="$<TARGET_FILE:reject_destroy_plugin>"
    BUILDER_PLUGIN_PATH="$<TARGET_FILE:builder_plugin>")
  add_dependencies(plugins_test_plugin_factory_host
                   factory_registers_op_plugin factory_escapes_plugin
                   scan_registers_op_plugin reject_destroy_plugin
                   builder_plugin)
endif()

if(TARGET plugins_test_plugin_describe)
  target_compile_definitions(
    plugins_test_plugin_describe PRIVATE
    TOY_FORMAT_PLUGIN_PATH="$<TARGET_FILE:toy_format_plugin>"
    TOY_SINK_PLUGIN_PATH="$<TARGET_FILE:toy_sink_plugin>"
    UDF_PLUGIN_PATH="$<TARGET_FILE:udf_plugin>"
    UDAF_PLUGIN_PATH="$<TARGET_FILE:udaf_plugin>"
    PROVIDER_FIXTURE_PLUGIN_PATH="$<TARGET_FILE:provider_fixture_plugin>"
    BUILDER_PLUGIN_PATH="$<TARGET_FILE:builder_plugin>"
    INDEX_MINMAX_PLUGIN_PATH="$<TARGET_FILE:index_minmax_plugin>"
    LIFETIME_PLUGIN_PATH="$<TARGET_FILE:lifetime_plugin>")
  add_dependencies(plugins_test_plugin_describe toy_format_plugin
                   toy_sink_plugin udf_plugin udaf_plugin
                   provider_fixture_plugin builder_plugin
                   index_minmax_plugin lifetime_plugin)
endif()

if(TARGET plugins_test_plugin_frame_check)
  target_compile_definitions(
    plugins_test_plugin_frame_check PRIVATE
    BROKEN_FRAME_PLUGIN_PATH="$<TARGET_FILE:broken_frame_plugin>")
  add_dependencies(plugins_test_plugin_frame_check broken_frame_plugin)
endif()

if(TARGET plugins_test_plugin_frame_check_off)
  target_compile_definitions(
    plugins_test_plugin_frame_check_off PRIVATE
    BROKEN_FRAME_PLUGIN_PATH="$<TARGET_FILE:broken_frame_plugin>")
  add_dependencies(plugins_test_plugin_frame_check_off broken_frame_plugin)
endif()

if(TARGET plugins_test_example_range_provider)
  target_compile_definitions(
    plugins_test_example_range_provider
    PRIVATE
      RANGE_PROVIDER_PLUGIN_PATH="$<TARGET_FILE:range_provider_plugin>")
  add_dependencies(plugins_test_example_range_provider range_provider_plugin)
endif()

if(TARGET plugins_test_plugin_udf)
  target_compile_definitions(
    plugins_test_plugin_udf PRIVATE
    UDF_PLUGIN_PATH="$<TARGET_FILE:udf_plugin>"
    UDF_CLASH_PLUGIN_PATH="$<TARGET_FILE:udf_clash_plugin>")
  add_dependencies(plugins_test_plugin_udf udf_plugin udf_clash_plugin)
  if(TARGET duration_udfs_plugin)
    target_compile_definitions(
      plugins_test_plugin_udf PRIVATE
      DURATION_UDFS_PLUGIN_PATH="$<TARGET_FILE:duration_udfs_plugin>")
    add_dependencies(plugins_test_plugin_udf duration_udfs_plugin)
  endif()
endif()

if(TARGET plugins_test_plugin_udaf)
  target_compile_definitions(
    plugins_test_plugin_udaf PRIVATE
    UDAF_PLUGIN_PATH="$<TARGET_FILE:udaf_plugin>"
    UDAF_BAD_PLUGIN_PATH="$<TARGET_FILE:udaf_bad_plugin>")
  add_dependencies(plugins_test_plugin_udaf udaf_plugin udaf_bad_plugin)
  if(TARGET stats_udafs_plugin)
    target_compile_definitions(
      plugins_test_plugin_udaf PRIVATE
      STATS_UDAFS_PLUGIN_PATH="$<TARGET_FILE:stats_udafs_plugin>")
    add_dependencies(plugins_test_plugin_udaf stats_udafs_plugin)
  endif()
endif()

if(TARGET plugins_test_plugin_udaf_groupby)
  target_compile_definitions(
    plugins_test_plugin_udaf_groupby PRIVATE
    UDAF_PLUGIN_PATH="$<TARGET_FILE:udaf_plugin>")
  add_dependencies(plugins_test_plugin_udaf_groupby udaf_plugin)
  if(TARGET stats_udafs_plugin)
    target_compile_definitions(
      plugins_test_plugin_udaf_groupby PRIVATE
      STATS_UDAFS_PLUGIN_PATH="$<TARGET_FILE:stats_udafs_plugin>")
    add_dependencies(plugins_test_plugin_udaf_groupby stats_udafs_plugin)
  endif()
endif()

if(TARGET plugins_test_plugin_udaf_window)
  target_compile_definitions(
    plugins_test_plugin_udaf_window PRIVATE
    UDAF_PLUGIN_PATH="$<TARGET_FILE:udaf_plugin>")
  add_dependencies(plugins_test_plugin_udaf_window udaf_plugin)
  if(TARGET stats_udafs_plugin)
    target_compile_definitions(
      plugins_test_plugin_udaf_window PRIVATE
      STATS_UDAFS_PLUGIN_PATH="$<TARGET_FILE:stats_udafs_plugin>")
    add_dependencies(plugins_test_plugin_udaf_window stats_udafs_plugin)
  endif()
endif()

if(TARGET plugins_test_plugin_udaf_duql)
  target_compile_definitions(
    plugins_test_plugin_udaf_duql PRIVATE
    UDAF_PLUGIN_PATH="$<TARGET_FILE:udaf_plugin>")
  add_dependencies(plugins_test_plugin_udaf_duql udaf_plugin)
  if(TARGET stats_udafs_plugin)
    target_compile_definitions(
      plugins_test_plugin_udaf_duql PRIVATE
      STATS_UDAFS_PLUGIN_PATH="$<TARGET_FILE:stats_udafs_plugin>")
    add_dependencies(plugins_test_plugin_udaf_duql stats_udafs_plugin)
  endif()
endif()

add_library(lifetime_plugin MODULE lifetime_plugin.c)
target_set_warnings(lifetime_plugin)
set_target_properties(
  lifetime_plugin
  PROPERTIES OUTPUT_NAME lifetime_plugin
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 11
             C_STANDARD_REQUIRED ON
             RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
             LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(lifetime_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include
                                   ${CMAKE_BINARY_DIR}/include)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(lifetime_plugin PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(lifetime_plugin PRIVATE dftracer_utils_dataframe::static)
endif()

if(TARGET plugins_test_plugin_library_lifetime)
  target_compile_definitions(
    plugins_test_plugin_library_lifetime PRIVATE
    LIFETIME_PLUGIN_PATH="$<TARGET_FILE:lifetime_plugin>")
  add_dependencies(plugins_test_plugin_library_lifetime lifetime_plugin)
endif()

if(TARGET plugins_test_plugin_borrowed_column)
  target_compile_definitions(
    plugins_test_plugin_borrowed_column PRIVATE
    BORROWED_FRAME_PLUGIN_PATH="$<TARGET_FILE:borrowed_frame_plugin>")
  add_dependencies(plugins_test_plugin_borrowed_column borrowed_frame_plugin)
endif()

if(TARGET plugins_test_example_csv_reader AND TARGET csv_reader_plugin)
  target_compile_definitions(
    plugins_test_example_csv_reader
    PRIVATE
    CSV_READER_PLUGIN_PATH="$<TARGET_FILE:csv_reader_plugin>"
    CSV_READER_CHUNKS_CSV="${CMAKE_SOURCE_DIR}/examples/plugins/tests/csv_reader/data/chunks.csv"
  )
  add_dependencies(plugins_test_example_csv_reader csv_reader_plugin)
endif()

if(TARGET plugins_test_example_two_phase_normalize
   AND TARGET two_phase_normalize_plugin)
  target_compile_definitions(
    plugins_test_example_two_phase_normalize PRIVATE
    TWO_PHASE_NORMALIZE_PLUGIN_PATH="$<TARGET_FILE:two_phase_normalize_plugin>")
  add_dependencies(plugins_test_example_two_phase_normalize
                   two_phase_normalize_plugin)
endif()

if(TARGET plugins_test_example_sessionize AND TARGET sessionize_plugin)
  target_compile_definitions(
    plugins_test_example_sessionize PRIVATE
    SESSIONIZE_PLUGIN_PATH="$<TARGET_FILE:sessionize_plugin>"
    SESSIONIZE_INPUT_PLUGIN_PATH="$<TARGET_FILE:sessionize_input_plugin>")
  add_dependencies(plugins_test_example_sessionize sessionize_plugin
                   sessionize_input_plugin)
endif()

if(TARGET plugins_test_global_plugins)
  target_compile_definitions(
    plugins_test_global_plugins PRIVATE
    TOY_FORMAT_PLUGIN_PATH="$<TARGET_FILE:toy_format_plugin>")
  add_dependencies(plugins_test_global_plugins toy_format_plugin)
endif()

if(TARGET plugins_test_plugin_file_formats)
  target_compile_definitions(
    plugins_test_plugin_file_formats PRIVATE
    TOY_FORMAT_PLUGIN_PATH="$<TARGET_FILE:toy_format_plugin>"
    TOY_FORMAT_TIE_PLUGIN_PATH="$<TARGET_FILE:toy_format_tie_plugin>"
    TOY_SINK_PLUGIN_PATH="$<TARGET_FILE:toy_sink_plugin>"
    TOY_BAD_SINK_PLUGIN_PATH="$<TARGET_FILE:toy_bad_sink_plugin>")
  add_dependencies(plugins_test_plugin_file_formats toy_format_plugin
                   toy_format_tie_plugin toy_sink_plugin toy_bad_sink_plugin)
endif()

if(TARGET plugins_test_plugin_format_index)
  target_compile_definitions(
    plugins_test_plugin_format_index PRIVATE
    CHUNKED_FORMAT_PLUGIN_PATH="$<TARGET_FILE:chunked_format_plugin>"
    CHUNKED_ALT_PLUGIN_PATH="$<TARGET_FILE:chunked_alt_plugin>"
    CHUNKED_V2_PLUGIN_PATH="$<TARGET_FILE:chunked_v2_plugin>"
    INDEX_COLUMNS_PLUGIN_PATH="$<TARGET_FILE:index_columns_plugin>"
    SDK_CHUNKED_PLUGIN_PATH="$<TARGET_FILE:sdk_chunked_format_plugin>")
  add_dependencies(plugins_test_plugin_format_index chunked_format_plugin
                   chunked_alt_plugin chunked_v2_plugin index_columns_plugin
                   sdk_chunked_format_plugin)
endif()

if(TARGET plugins_test_plugin_explain)
  target_compile_definitions(
    plugins_test_plugin_explain PRIVATE
    EXPLAIN_PLUGIN_PATH="$<TARGET_FILE:explain_plugin>"
    CHUNKED_FORMAT_PLUGIN_PATH="$<TARGET_FILE:chunked_format_plugin>"
    SDK_EXPLAIN_PLUGIN_PATH="$<TARGET_FILE:sdk_explain_plugin>")
  add_dependencies(plugins_test_plugin_explain explain_plugin
                   chunked_format_plugin sdk_explain_plugin)
endif()

if(TARGET plugins_test_plugin_sink_duql)
  target_compile_definitions(
    plugins_test_plugin_sink_duql PRIVATE
    TOY_SINK_PLUGIN_PATH="$<TARGET_FILE:toy_sink_plugin>")
  add_dependencies(plugins_test_plugin_sink_duql toy_sink_plugin)
endif()

if(TARGET plugins_test_plugin_node_duql)
  target_compile_definitions(
    plugins_test_plugin_node_duql PRIVATE
    TOY_NODE_PLUGIN_PATH="$<TARGET_FILE:toy_node_plugin>")
  add_dependencies(plugins_test_plugin_node_duql toy_node_plugin)
endif()

if(TARGET plugins_test_plugin_providers)
  target_compile_definitions(
    plugins_test_plugin_providers PRIVATE
    PROVIDER_FIXTURE_PLUGIN_PATH="$<TARGET_FILE:provider_fixture_plugin>")
  add_dependencies(plugins_test_plugin_providers
                   provider_fixture_plugin)
endif()

# +++++++++++++++++++++++++++++++++++++++++
# Reference plugin: touches every dftu.svc.* service slot from real C (plan
# 12, gap 11 - "conformance + trust").
# +++++++++++++++++++++++++++++++++++++++++

add_library(reference_plugin MODULE reference_plugin.c)
target_set_warnings(reference_plugin)
set_target_properties(
  reference_plugin
  PROPERTIES OUTPUT_NAME reference_plugin
             PREFIX ""
             POSITION_INDEPENDENT_CODE ON
             C_STANDARD 11
             C_STANDARD_REQUIRED ON
             RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
             LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(reference_plugin
                           PRIVATE ${CMAKE_SOURCE_DIR}/include
                                   ${CMAKE_BINARY_DIR}/include)
if(TARGET dftracer_utils_dataframe::shared)
  target_link_libraries(reference_plugin PRIVATE dftracer_utils_dataframe::shared)
elseif(TARGET dftracer_utils_dataframe::static)
  target_link_libraries(reference_plugin PRIVATE dftracer_utils_dataframe::static)
endif()
# A switch with a missing case for a newly added ref_svc_id enumerator (a
# 14th dftu.svc.* service not yet wired into this plugin) must fail the
# build, not just the editor.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reference_plugin PRIVATE -Wswitch -Werror=switch)
endif()

if(TARGET plugins_test_plugin_reference)
  target_compile_definitions(
    plugins_test_plugin_reference PRIVATE
    REFERENCE_PLUGIN_PATH="$<TARGET_FILE:reference_plugin>")
  add_dependencies(plugins_test_plugin_reference reference_plugin)
endif()
