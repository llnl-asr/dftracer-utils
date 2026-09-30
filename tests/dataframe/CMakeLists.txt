# ##############################################################################
# DataFrame Library Tests
# ##############################################################################

set(DATAFRAME_TEST_SOURCES test_mask.cpp test_series.cpp test_methods.cpp
                           test_arrow.cpp test_arrow_types.cpp test_arrow_types_compute.cpp
                           test_arrow_large_types_compute.cpp
                           test_arrow_types_matrix.cpp test_arrow_types_tail.cpp
                           test_arrow_types_lazy.cpp
                           test_arrow_types_windowed.cpp test_arrow_types_reshape.cpp
                           test_field.cpp test_op_registry.cpp test_op_parity.cpp
                           test_lazyframe.cpp test_join.cpp test_concat.cpp test_lazy_frame_ops.cpp
                           test_lazyframe_abi.cpp test_lazy_ops.cpp test_lazy_plan.cpp test_source_hooks.cpp test_plugin_sdk.cpp test_spill.cpp test_native_frame.cpp test_join_spill.cpp test_agg_state.cpp
                           test_agg_extended_ops.cpp test_host_ops.cpp
                           test_host_ops_autoregister.cpp test_expr_string.cpp test_expr_string_ops.cpp test_expr_duql.cpp
                           test_expr_infer_type.cpp
                           test_provider_pushdown.cpp test_provider_schema_types.cpp
                           test_type_parameter_propagation.cpp
                           test_lazyframe_output_schema.cpp
                           test_lazyframe_plugin_node.cpp
                           test_morsel_ordering.cpp
                           test_order.cpp test_frame_window.cpp
                           test_frame_gap_fill.cpp test_frame_asof.cpp
                           test_frame_ops.cpp test_reduce_types.cpp
                           test_count_nulls.cpp test_set_union.cpp test_cast_pairs.cpp
                           test_filter_columns.cpp test_rank_pct.cpp test_rank_nan.cpp test_cut_flags.cpp test_fillna_bool_string.cpp test_cast_followups.cpp
                           test_where_null.cpp test_constant_column.cpp
                           test_variance_stable.cpp
                           test_covariance_stable.cpp
                           test_join_null_keys.cpp
                           test_concat_large_string.cpp
                           test_groupby_scale.cpp
                           test_string_simd.cpp test_expr_string_methods.cpp
                           test_concat_text.cpp)

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
  # 1.2M rows folded per case on every thread count: about 200 s alone on a
  # laptop, twice that on a 4-core runner and more under coverage.
  if(bin_exec STREQUAL "test_groupby_scale")
    set(scale_timeout 900)
    if(DFTRACER_UTILS_COVERAGE OR DFTRACER_UTILS_ENABLE_TSAN
       OR DFTRACER_UTILS_ENABLE_ASAN)
      set(scale_timeout 1800)
    endif()
    set_tests_properties(dataframe/${bin_exec} PROPERTIES TIMEOUT ${scale_timeout})
  endif()
endforeach()

# ##############################################################################
# DFTU_SCALAR_* C macros (guarded #ifndef __cplusplus in abi.h; needs a C TU)
# ##############################################################################

add_executable(dataframe_test_scalar_macros_c test_scalar_macros_c.c)
target_include_directories(dataframe_test_scalar_macros_c PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(dataframe_test_scalar_macros_c PRIVATE dftracer_utils)
set_target_properties(dataframe_test_scalar_macros_c PROPERTIES OUTPUT_NAME test_scalar_macros_c)
add_test(NAME dataframe/test_scalar_macros_c COMMAND dataframe_test_scalar_macros_c)
