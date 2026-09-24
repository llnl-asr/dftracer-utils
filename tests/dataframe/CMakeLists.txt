# ##############################################################################
# DataFrame Library Tests
# ##############################################################################

set(DATAFRAME_TEST_SOURCES test_mask.cpp test_series.cpp test_result_batch.cpp test_methods.cpp
                           test_arrow.cpp test_arrow_types.cpp test_arrow_types_compute.cpp
                           test_arrow_large_types_compute.cpp
                           test_arrow_types_matrix.cpp test_arrow_types_tail.cpp
                           test_arrow_types_lazy.cpp
                           test_arrow_types_windowed.cpp test_arrow_types_reshape.cpp
                           test_field.cpp test_op_registry.cpp test_op_parity.cpp
                           test_lazyframe.cpp test_join.cpp test_concat.cpp test_lazy_frame_ops.cpp
                           test_lazyframe_abi.cpp test_lazy_ops.cpp test_lazy_plan.cpp test_source_hooks.cpp test_plugin_sdk.cpp test_spill.cpp test_agg_state.cpp
                           test_agg_extended_ops.cpp test_host_ops.cpp
                           test_host_ops_autoregister.cpp test_expr_string.cpp test_expr_string_ops.cpp
                           test_expr_infer_type.cpp
                           test_provider_pushdown.cpp test_provider_schema_types.cpp
                           test_type_parameter_propagation.cpp
                           test_lazyframe_output_schema.cpp
                           test_lazyframe_plugin_node.cpp
                           test_morsel_ordering.cpp)

foreach(test_file ${DATAFRAME_TEST_SOURCES})
  string(REPLACE ".cpp" "" bin_exec ${test_file})
  string(REPLACE "/" "_" target_name "dataframe_${bin_exec}")

  add_executable(${target_name} ${test_file})
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${target_name} PRIVATE doctest::doctest dftracer_utils testing_utilities)
  # Some tests include internal SIMD kernel headers (e.g. kernels/sort.h ->
  # hwy/contrib/sort/vqsort.h). The dataframe library links Highway PRIVATE, so
  # its include dir is not propagated to consumers; bring it in for the tests
  # that include those headers directly. Add only the include path: the Highway
  # libraries are already provided through the dataframe library's link, so
  # relinking them here put libhwy.a on the command twice.
  if(TARGET hwy)
    target_include_directories(${target_name} SYSTEM PRIVATE
                               $<TARGET_PROPERTY:hwy,INTERFACE_INCLUDE_DIRECTORIES>)
  endif()
  if(TARGET hwy_contrib)
    target_include_directories(${target_name} SYSTEM PRIVATE
                               $<TARGET_PROPERTY:hwy_contrib,INTERFACE_INCLUDE_DIRECTORIES>)
  endif()
  target_set_warnings(${target_name})
  target_enable_coroutine(${target_name})

  set_target_properties(${target_name} PROPERTIES OUTPUT_NAME ${bin_exec})

  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE --coverage -fprofile-arcs
                                                   -ftest-coverage)
    target_link_libraries(${target_name} PRIVATE --coverage)
  endif()

  set(test_working_directory
      "${CMAKE_CURRENT_BINARY_DIR}/workdirs/${target_name}")
  file(MAKE_DIRECTORY "${test_working_directory}")

  add_test(NAME dataframe/${bin_exec} COMMAND ${target_name})
  set_tests_properties(dataframe/${bin_exec} PROPERTIES
                       WORKING_DIRECTORY "${test_working_directory}")
endforeach()

# ##############################################################################
# DFTU_SCALAR_* C macros (guarded #ifndef __cplusplus in abi.h; needs a C TU)
# ##############################################################################

add_executable(dataframe_test_scalar_macros_c test_scalar_macros_c.c)
target_include_directories(dataframe_test_scalar_macros_c PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(dataframe_test_scalar_macros_c PRIVATE dftracer_utils)
set_target_properties(dataframe_test_scalar_macros_c PROPERTIES OUTPUT_NAME test_scalar_macros_c)
add_test(NAME dataframe/test_scalar_macros_c COMMAND dataframe_test_scalar_macros_c)
