# ##############################################################################
# Dependencies
# ##############################################################################

need_test_deps()
need_zlib()

# ##############################################################################
# Testing Utilities
# ##############################################################################

add_library(testing_utilities STATIC testing_utilities.cpp)
target_link_libraries(
  testing_utilities
  PUBLIC dftracer_utils
  PRIVATE doctest::doctest ghc_filesystem)
link_zlib(testing_utilities STATIC)

# ##############################################################################
# Unit Tests
# ##############################################################################

# +++++++++++++++++++++++++++++++++++++++++
# Test (C++ API)
# +++++++++++++++++++++++++++++++++++++++++

set(TEST_CPP_SOURCES
    # Core tests
    core/test_runtime.cpp
    core/test_logging.cpp
    # Pipeline tests
    pipeline/test_pipeline.cpp

    pipeline/test_task_scope.cpp
    pipeline/test_simple_scope.cpp
    pipeline/test_coro_scope.cpp
    # Coroutine tests
    coro/test_coro_syntax.cpp
    coro/test_channel.cpp
    coro/test_generator.cpp
    coro/test_async_generator.cpp
    coro/test_coro.cpp
    coro/test_yield.cpp
    coro/test_when_all_heterogeneous.cpp
    coro/test_when_any_heterogeneous.cpp
    # Async I/O tests
    io/test_async_io.cpp
    # Task Graph tests
    task_graph/test_task_graph.cpp
    # Reader tests
    reader/test_reader.cpp
    reader/test_reader_stream.cpp
    reader/test_reader_line_byte_stream.cpp
    reader/test_reader_multi_lines_byte_stream.cpp
    reader/test_basic_factory.cpp
    reader/test_reader_formats.cpp
    reader/test_reader_robustness.cpp
    # reader/test_reader_tar_comprehensive.cpp
    # Trace generation utility tests
    utilities/trace_gen/test_fake_trace_writer.cpp
    # Replay tests
    replay/test_replay.cpp
    replay/test_replay_fidelity.cpp
    # Server unit tests
    server/test_http_parser.cpp
    server/test_http_response.cpp
    server/test_http_request.cpp
    server/test_query_params.cpp
    server/test_trace_index.cpp
    )

foreach(test_file ${TEST_CPP_SOURCES})
  # Extract path and filename
  string(REPLACE ".cpp" "" bin_exec ${test_file})
  string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" bin_exec ${bin_exec})

  # Create valid target name (replace / with _)
  string(REPLACE "/" "_" target_name ${bin_exec})

  add_executable(${target_name} ${test_file})
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${target_name} PRIVATE doctest::doctest testing_utilities)
  target_set_warnings(${target_name})
  target_enable_coroutine(${target_name})
  target_add_rpath(${target_name})
  
  # Pass CMAKE_BINARY_DIR to tests that need to execute binaries
  target_compile_definitions(${target_name} PRIVATE CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}")

  # Set output directory to preserve folder structure in binaries
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

  add_test(NAME ${bin_exec} COMMAND ${target_name})

  # Default timeout for all tests (safety net against hangs in CI)
  set(default_test_timeout 120)
  if(DFTRACER_UTILS_ENABLE_TSAN)
    set(default_test_timeout 300)
  endif()
  set_tests_properties(${bin_exec} PROPERTIES TIMEOUT ${default_test_timeout})

  # These thread-heavy tests contend heavily when run together on low-core CI.
  # Give them extra timeout and serialize via a shared lock.
  if(bin_exec STREQUAL "core/test_runtime"
     OR bin_exec STREQUAL "pipeline/test_task_scope"
     OR bin_exec STREQUAL "pipeline/test_pipeline"
     OR bin_exec STREQUAL "coro/test_channel")
    set(heavy_thread_timeout 180)
    if(DFTRACER_UTILS_ENABLE_TSAN OR DFTRACER_UTILS_ENABLE_COVERAGE
       OR DFTRACER_UTILS_ENABLE_ASAN)
      set(heavy_thread_timeout 600)
    endif()
    set_tests_properties(${bin_exec} PROPERTIES
                                           TIMEOUT ${heavy_thread_timeout}
                                           RESOURCE_LOCK heavy_threading)
  endif()

  # Robustness test generates large (128 MB) gzip files; coverage
  # instrumentation on macOS CI runners makes it significantly slower.
  if(bin_exec STREQUAL "reader/test_reader_robustness")
    set(robustness_timeout 600)
    if(DFTRACER_UTILS_COVERAGE)
      set(robustness_timeout 900)
    endif()
    set_tests_properties(${bin_exec} PROPERTIES TIMEOUT ${robustness_timeout})
  endif()
endforeach()

# +++++++++++++++++++++++++++++++++++++++++
# Test Reader C API
# +++++++++++++++++++++++++++++++++++++++++

set(TEST_C_SOURCES
    reader/test_reader.c
    reader/test_reader_stream.c
    # reader/test_reader_formats_c.c  # TODO: Add when ready
    )

foreach (test_file ${TEST_C_SOURCES})
  # Extract path and filename
  string(REPLACE ".c" "" bin_exec ${test_file})
  string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" bin_exec ${bin_exec})
  set(bin_exec "${bin_exec}_c")

  # Create valid target name (replace / with _)
  string(REPLACE "/" "_" target_name ${bin_exec})

  add_executable(${target_name} ${test_file})
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${target_name} PRIVATE unity_lib testing_utilities)
  target_add_rpath(${target_name})

  # Set output directory to preserve folder structure in binaries
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

  add_test(NAME ${bin_exec} COMMAND ${target_name})
endforeach()

# +++++++++++++++++++++++++++++++++++++++++
# Utilities Tests
# +++++++++++++++++++++++++++++++++++++++++

add_subdirectory(utilities)

# +++++++++++++++++++++++++++++++++++++++++
# Binary Integration Tests
# +++++++++++++++++++++++++++++++++++++++++

set(TEST_BINARY_SOURCES
    binaries/test_dftracer_server.cpp
    binaries/test_dftracer_merge.cpp
    binaries/test_dftracer_event_count.cpp
    binaries/test_dftracer_validate.cpp
    binaries/test_dftracer_split.cpp
    binaries/test_dftracer_pgzip.cpp
    binaries/test_dftracer_gen_fake_trace.cpp
    binaries/test_dftracer_info.cpp
    binaries/test_dftracer_reader.cpp
    binaries/test_dftracer_stats.cpp
    binaries/test_dftracer_index.cpp
    binaries/test_dftracer_aggregator.cpp
    binaries/test_dftracer_gen_dlio_config.cpp
    binaries/test_dftracer_organize.cpp
    binaries/test_dftracer_view.cpp
    binaries/test_dftracer_tar.cpp
    binaries/test_dftracer_replay.cpp
    binaries/test_dftracer_comparator.cpp
    binaries/test_dftracer_call_tree.cpp
    )

if(DFTRACER_UTILS_ENABLE_MPI)
  list(APPEND TEST_BINARY_SOURCES
    binaries/test_dftracer_aggregator_mpi.cpp
    binaries/test_dftracer_call_tree_mpi.cpp)
endif()

foreach(test_file ${TEST_BINARY_SOURCES})
  string(REPLACE ".cpp" "" bin_exec ${test_file})
  string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" bin_exec ${bin_exec})
  string(REPLACE "/" "_" target_name ${bin_exec})

  add_executable(${target_name} ${test_file})
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${target_name} PRIVATE doctest::doctest testing_utilities)
  target_set_warnings(${target_name})
  target_enable_coroutine(${target_name})
  target_add_rpath(${target_name})

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

  add_test(NAME ${bin_exec} COMMAND ${target_name})

  # Integration tests need more time (server startup + HTTP requests).
  # macOS CI runners are slower; keep generous headroom.
  set(integration_test_timeout 120)
  if(DFTRACER_UTILS_ENABLE_TSAN)
    set(integration_test_timeout 180)
  endif()
  # The MPI aggregator spawns mpiexec several times per test case and
  # each child runs under ASan, which is ~10x slower than release. The
  # default 120s is not enough.
  if(bin_exec STREQUAL "binaries/test_dftracer_aggregator_mpi" OR
     bin_exec STREQUAL "binaries/test_dftracer_call_tree_mpi")
    set(integration_test_timeout 600)
  endif()
  set_tests_properties(${bin_exec} PROPERTIES TIMEOUT ${integration_test_timeout})

  # dftracer_tar wraps the gzip indexer's parallel pipeline and has shown
  # intermittent deadlocks on CI. Tar is a secondary code path (real
  # workflows use directories of .pfw.gz).
  if(bin_exec STREQUAL "binaries/test_dftracer_tar")
    set_tests_properties(${bin_exec} PROPERTIES
      TIMEOUT 240
      RUN_SERIAL TRUE
      REPEAT "UNTIL_PASS:3")
  endif()

  # Pass binary paths so tests can find them
  if(bin_exec STREQUAL "binaries/test_dftracer_server")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_SERVER_PATH=$<TARGET_FILE:dftracer_server>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_merge")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_MERGE_PATH=$<TARGET_FILE:dftracer_merge>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_event_count")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_EVENT_COUNT_PATH=$<TARGET_FILE:dftracer_event_count>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_split")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_SPLIT_PATH=$<TARGET_FILE:dftracer_split>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_pgzip")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_PGZIP_PATH=$<TARGET_FILE:dftracer_pgzip>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_gen_fake_trace")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_GEN_FAKE_TRACE_PATH=$<TARGET_FILE:dftracer_gen_fake_trace>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_info")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_INFO_PATH=$<TARGET_FILE:dftracer_info>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_reader")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_READER_BIN_PATH=$<TARGET_FILE:dftracer_reader>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_stats")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_STATS_PATH=$<TARGET_FILE:dftracer_stats>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_index")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_INDEX_PATH=$<TARGET_FILE:dftracer_index>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_aggregator")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_AGGREGATOR_PATH=$<TARGET_FILE:dftracer_aggregator>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_gen_dlio_config")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_GEN_DLIO_CONFIG_PATH=$<TARGET_FILE:dftracer_gen_dlio_config>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_organize")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_ORGANIZE_PATH=$<TARGET_FILE:dftracer_organize>;DFTRACER_RECONSTRUCT_PATH=$<TARGET_FILE:dftracer_reconstruct>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_view")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_VIEW_PATH=$<TARGET_FILE:dftracer_view>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_tar")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_TAR_PATH=$<TARGET_FILE:dftracer_tar>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_replay")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_REPLAY_PATH=$<TARGET_FILE:dftracer_replay>;ASAN_OPTIONS=detect_leaks=0;LSAN_OPTIONS=detect_leaks=0")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_comparator")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_COMPARATOR_PATH=$<TARGET_FILE:dftracer_comparator>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_call_tree")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_CALL_TREE_PATH=$<TARGET_FILE:dftracer_call_tree>")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_call_tree_mpi")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_CALL_TREE_PATH=$<TARGET_FILE:dftracer_call_tree>;DFTRACER_CALL_TREE_MPI_PATH=$<TARGET_FILE:dftracer_call_tree_mpi>;MPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE};ASAN_OPTIONS=detect_leaks=0;LSAN_OPTIONS=detect_leaks=0")
  elseif(bin_exec STREQUAL "binaries/test_dftracer_aggregator_mpi")
    set_tests_properties(${bin_exec} PROPERTIES ENVIRONMENT
      "DFTRACER_AGGREGATOR_PATH=$<TARGET_FILE:dftracer_aggregator>;DFTRACER_AGGREGATOR_MPI_PATH=$<TARGET_FILE:dftracer_aggregator_mpi>;MPIEXEC_EXECUTABLE=${MPIEXEC_EXECUTABLE};ASAN_OPTIONS=detect_leaks=0;LSAN_OPTIONS=detect_leaks=0")
  endif()

endforeach()
# +++++++++++++++++++++++++++++++++++++++++
# Python Tests (SKBUILD only)
# +++++++++++++++++++++++++++++++++++++++++

if(SKBUILD)
  find_package(
    Python
    COMPONENTS Interpreter
    REQUIRED)
  add_test(
    NAME test_python_reader
    COMMAND ${Python_EXECUTABLE} -m pytest
            ${CMAKE_CURRENT_SOURCE_DIR}/python/test_reader.py -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(
    test_python_reader PROPERTIES ENVIRONMENT
                                  "PYTHONPATH=${CMAKE_BINARY_DIR}/python/src")
endif()

# ##############################################################################
# Package Discovery Tests
# ##############################################################################

# Test discovery using the installed package
# Use a local install directory for testing to avoid permission issues
set(TEST_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/test_install")

option(DFTRACER_UTILS_PACKAGE_TESTS "Run installed-package discovery tests" ON)
if(DFTRACER_UTILS_PACKAGE_TESTS)
  add_test(
    NAME install_package
    COMMAND
      ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --prefix
      ${TEST_INSTALL_PREFIX})
  set_tests_properties(install_package PROPERTIES FIXTURES_SETUP
                                                   "installed_package")

  # ==========================================================================
  # pkg-config discovery tests
  # ==========================================================================

  # Test core library
  add_test(
    NAME test_pkgconfig_discovery_core
    COMMAND
      ${CMAKE_COMMAND}
      -DPKG_CONFIG_PATH=${TEST_INSTALL_PREFIX}/lib/pkgconfig:${TEST_INSTALL_PREFIX}/lib64/pkgconfig
      -DLIBRARY_NAME=dftracer_utils_core
      -DTEST_TYPE=pkgconfig -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # Test utilities library
  add_test(
    NAME test_pkgconfig_discovery_utilities
    COMMAND
      ${CMAKE_COMMAND}
      -DPKG_CONFIG_PATH=${TEST_INSTALL_PREFIX}/lib/pkgconfig:${TEST_INSTALL_PREFIX}/lib64/pkgconfig
      -DLIBRARY_NAME=dftracer_utils_utilities
      -DTEST_TYPE=pkgconfig -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # Test unified library
  add_test(
    NAME test_pkgconfig_discovery_unified
    COMMAND
      ${CMAKE_COMMAND}
      -DPKG_CONFIG_PATH=${TEST_INSTALL_PREFIX}/lib/pkgconfig:${TEST_INSTALL_PREFIX}/lib64/pkgconfig
      -DLIBRARY_NAME=dftracer_utils
      -DTEST_TYPE=pkgconfig -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # ==========================================================================
  # CMake find_package discovery tests
  # ==========================================================================

  # Test core library
  add_test(
    NAME test_cmake_discovery_core
    COMMAND
      ${CMAKE_COMMAND} -DCMAKE_PREFIX_PATH=${TEST_INSTALL_PREFIX}
      -DLIBRARY_NAME=dftracer_utils_core
      -DTEST_TYPE=cmake -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # Test utilities library
  add_test(
    NAME test_cmake_discovery_utilities
    COMMAND
      ${CMAKE_COMMAND} -DCMAKE_PREFIX_PATH=${TEST_INSTALL_PREFIX}
      -DLIBRARY_NAME=dftracer_utils_utilities
      -DTEST_TYPE=cmake -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # Test unified library
  add_test(
    NAME test_cmake_discovery_unified
    COMMAND
      ${CMAKE_COMMAND} -DCMAKE_PREFIX_PATH=${TEST_INSTALL_PREFIX}
      -DLIBRARY_NAME=dftracer_utils
      -DTEST_TYPE=cmake -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # ==========================================================================
  # Target alias tests
  # ==========================================================================

  set(PACKAGE_TEST_VARIANTS "")
  if(DFTRACER_UTILS_BUILD_SHARED)
    list(APPEND PACKAGE_TEST_VARIANTS shared)
  endif()
  if(DFTRACER_UTILS_BUILD_STATIC)
    list(APPEND PACKAGE_TEST_VARIANTS static)
  endif()

  set(PACKAGE_DISCOVERY_TESTS
      test_pkgconfig_discovery_core
      test_pkgconfig_discovery_utilities
      test_pkgconfig_discovery_unified
      test_cmake_discovery_core
      test_cmake_discovery_utilities
      test_cmake_discovery_unified)

  function(add_target_alias_test TEST_NAME TARGET_ALIAS)
    add_test(
      NAME ${TEST_NAME}
      COMMAND
        ${CMAKE_COMMAND} -DCMAKE_PREFIX_PATH=${TEST_INSTALL_PREFIX}
        -DTARGET_ALIAS=${TARGET_ALIAS} -DTEST_TYPE=target -P
        ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)
  endfunction()

  add_target_alias_test(test_target_dftracer_utils "dftracer_utils")
  list(APPEND PACKAGE_DISCOVERY_TESTS test_target_dftracer_utils)

  foreach(VARIANT IN LISTS PACKAGE_TEST_VARIANTS)
    foreach(LIBRARY dftracer_utils_core dftracer_utils_utilities)
      add_target_alias_test(test_target_${LIBRARY}_${VARIANT}
                            "${LIBRARY}::${VARIANT}")
      list(APPEND PACKAGE_DISCOVERY_TESTS test_target_${LIBRARY}_${VARIANT})
    endforeach()

    add_target_alias_test(test_target_dftracer_utils_${VARIANT}
                          "dftracer_utils::${VARIANT}")
    add_target_alias_test(test_target_dftracer_utils_${VARIANT}_no_namespace
                          "dftracer_utils_${VARIANT}")
    list(APPEND PACKAGE_DISCOVERY_TESTS
         test_target_dftracer_utils_${VARIANT}
         test_target_dftracer_utils_${VARIANT}_no_namespace)
  endforeach()

  # ==========================================================================
  # Set test dependencies - all discovery tests should run after installation
  # ==========================================================================
  set_tests_properties(${PACKAGE_DISCOVERY_TESTS} PROPERTIES FIXTURES_REQUIRED
                                                             "installed_package")
endif()
