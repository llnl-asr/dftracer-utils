# SPDX-License-Identifier: MIT
# ##############################################################################
# Domains Library Tests
# ##############################################################################

set(DOMAINS_TEST_SOURCES
    analysis/comparator/test_comparison_result.cpp
    analysis/comparator/test_comparison_config.cpp
    analysis/comparator/test_tree_table_formatter.cpp
    analysis/comparator/test_agg_parity.cpp

    analysis/statistics/test_trace_statistics.cpp
    analysis/statistics/test_statistics_query.cpp
    analysis/statistics/test_detailed_statistics.cpp
    analysis/statistics/test_stats_view.cpp
    analysis/statistics/test_detail_stats_view.cpp

    workload/dlio/test_barrier_simulator.cpp
    workload/dlio/test_trace_loader.cpp
    workload/dlio/test_yaml_emit.cpp
    workload/dlio/test_mixture.cpp
    workload/dlio/test_distributions.cpp
)

foreach(test_file ${DOMAINS_TEST_SOURCES})
  # Extract path and filename
  string(REPLACE ".cpp" "" bin_exec ${test_file})

  # Create valid target name (replace / with _)
  string(REPLACE "/" "_" target_name "domains_${bin_exec}")

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

  add_test(NAME domains/${bin_exec} COMMAND ${target_name})
  set_tests_properties(domains/${bin_exec} PROPERTIES
                       WORKING_DIRECTORY "${test_working_directory}")
endforeach()

# The parity test replays references recorded from the CLI tools and the View.
target_compile_definitions(domains_analysis_comparator_test_agg_parity PRIVATE
  DFTRACER_UTILS_AGG_PARITY_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/analysis/comparator/data")
add_dependencies(domains_analysis_comparator_test_agg_parity
                 dftracer_comparator dftracer_gen_dlio_config)
set_tests_properties(domains/analysis/comparator/test_agg_parity PROPERTIES
  ENVIRONMENT "DFTRACER_UTILS_COMPARATOR_PATH=$<TARGET_FILE:dftracer_comparator>;DFTRACER_UTILS_GEN_DLIO_CONFIG_PATH=$<TARGET_FILE:dftracer_gen_dlio_config>"
  TIMEOUT 600)
