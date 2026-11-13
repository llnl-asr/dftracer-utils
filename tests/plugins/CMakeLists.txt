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
    test_plugin_results.cpp
    test_plugin_agg.cpp
    test_plugin_agg_spill.cpp
    test_plugin_ops.cpp
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
    test_plugin_reads.cpp
    test_plugin_providers.cpp
    test_provider_pushdown.cpp
    test_plugin_library_lifetime.cpp
    test_provider_schema_types.cpp
    test_lazyframe_plugin_node.cpp
    test_source_hooks_vtable.cpp
    test_plugin_reference.cpp
    host/test_host_ops.cpp
    host/test_host_ops_autoregister.cpp
    plugin/test_plugin_sdk.cpp
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
                 fs_abi_c_consumer.c)
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
# ABI gate: real .so files the loader must refuse - a wrong
# DFTRACER_UTILS_PLUGIN_ABI_VERSION, a wrong header hash, and no info symbol -
# loaded by plugins/test_plugin_abi_gate through a real dlopen.
# +++++++++++++++++++++++++++++++++++++++++

foreach(gate_plugin bad_abi_version_plugin bad_abi_hash_plugin no_info_plugin)
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

if(TARGET plugins_test_plugin_abi_gate)
  target_compile_definitions(
    plugins_test_plugin_abi_gate
    PRIVATE BAD_ABI_VERSION_PLUGIN_PATH="$<TARGET_FILE:bad_abi_version_plugin>"
            BAD_ABI_HASH_PLUGIN_PATH="$<TARGET_FILE:bad_abi_hash_plugin>"
            NO_INFO_PLUGIN_PATH="$<TARGET_FILE:no_info_plugin>")
  add_dependencies(plugins_test_plugin_abi_gate
                   bad_abi_version_plugin bad_abi_hash_plugin no_info_plugin)
endif()

if(TARGET plugins_test_plugin_index_extension)
  target_compile_definitions(
    plugins_test_plugin_index_extension PRIVATE
    INDEX_MINMAX_PLUGIN_PATH="$<TARGET_FILE:index_minmax_plugin>")
  add_dependencies(plugins_test_plugin_index_extension
                   index_minmax_plugin)
endif()

# +++++++++++++++++++++++++++++++++++++++++
# Build-phase host: one plugin whose factory registers an op at load, one whose
# factory reaches past the registration surface. Both are real .so files loaded
# through Plugins::Builder by plugins/test_plugin_factory_host.
# +++++++++++++++++++++++++++++++++++++++++

foreach(fh_plugin factory_registers_op_plugin factory_escapes_plugin
                  scan_registers_op_plugin index_minmax_plugin)
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

foreach(cxx_plugin builder_plugin config_keys_plugin reads_projection_plugin
                  provider_fixture_plugin)
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
endforeach()

# It reads the batch dataframe and runs its own registered op, so it links the
# dataframe C ABI like the examples that do the same.
foreach(df_plugin factory_registers_op_plugin builder_plugin
                  config_keys_plugin reads_projection_plugin
                  provider_fixture_plugin)
  if(TARGET dftracer_utils_dataframe::shared)
    target_link_libraries(${df_plugin} PRIVATE dftracer_utils_dataframe::shared)
  elseif(TARGET dftracer_utils_dataframe::static)
    target_link_libraries(${df_plugin} PRIVATE dftracer_utils_dataframe::static)
  endif()
endforeach()

if(TARGET plugins_test_plugin_config)
  target_compile_definitions(
    plugins_test_plugin_config PRIVATE
    CONFIG_KEYS_PLUGIN_PATH="$<TARGET_FILE:config_keys_plugin>")
  add_dependencies(plugins_test_plugin_config config_keys_plugin)
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

if(TARGET plugins_test_plugin_factory_host)
  target_compile_definitions(
    plugins_test_plugin_factory_host PRIVATE
    FACTORY_REGISTERS_OP_PLUGIN_PATH="$<TARGET_FILE:factory_registers_op_plugin>"
    FACTORY_ESCAPES_PLUGIN_PATH="$<TARGET_FILE:factory_escapes_plugin>"
    SCAN_REGISTERS_OP_PLUGIN_PATH="$<TARGET_FILE:scan_registers_op_plugin>"
    BUILDER_PLUGIN_PATH="$<TARGET_FILE:builder_plugin>")
  add_dependencies(plugins_test_plugin_factory_host
                   factory_registers_op_plugin factory_escapes_plugin
                   scan_registers_op_plugin
                   builder_plugin)
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

if(TARGET plugins_test_plugin_providers)
  target_compile_definitions(
    plugins_test_plugin_providers PRIVATE
    PROVIDER_FIXTURE_PLUGIN_PATH="$<TARGET_FILE:provider_fixture_plugin>")
  add_dependencies(plugins_test_plugin_providers
                   provider_fixture_plugin)
endif()

# +++++++++++++++++++++++++++++++++++++++++
# Reference plugin: touches every dftu.svc.* service slot from real C (plan
# 12, gap 11 - "conformance + trust"). Only built when Arrow is enabled: it
# exercises dftu.svc.arrow too, and there is no service-less stand-in for
# that check.
# +++++++++++++++++++++++++++++++++++++++++

if(DFTRACER_UTILS_ENABLE_ARROW)
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
  # A loadable module links the shared nanoarrow: the static archive is not
  # position independent on every target (aarch64 Linux refuses it).
  link_nanoarrow(reference_plugin SHARED)
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
endif()
