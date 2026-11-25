# SPDX-License-Identifier: MIT
# ##############################################################################
# Arrow Bridge Tests
#
# The only place a C++ test may use Arrow types: frame and column conversion,
# schema reconciliation and the IPC reader/writer.
# ##############################################################################

set(ARROW_TEST_SOURCES
    test_arrow_bridge.cpp
    test_arrow_index.cpp
    test_arrow_ipc_reader.cpp
    test_arrow_ipc_writer.cpp
    test_arrow_stream.cpp
    test_schema_reconcile.cpp
    test_arrow.cpp
    test_arrow_types.cpp
    test_arrow_view.cpp
    test_arrow_zero_copy.cpp
    test_arrow_type_parameters.cpp
    test_chunked_nested_export.cpp
)

foreach(test_file ${ARROW_TEST_SOURCES})
  string(REPLACE ".cpp" "" bin_exec ${test_file})
  string(REPLACE "/" "_" target_name "arrow_${bin_exec}")

  add_executable(${target_name} ${test_file})
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${target_name} PRIVATE doctest::doctest dftracer_utils ${DFTRACER_UTILS_ARROW_TARGET} testing_utilities)
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

  add_test(NAME arrow/${bin_exec} COMMAND ${target_name})
  set_tests_properties(arrow/${bin_exec} PROPERTIES
                       WORKING_DIRECTORY "${test_working_directory}")
endforeach()
