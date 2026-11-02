# SPDX-License-Identifier: MIT
# ##############################################################################
# Compose Contract Tests
# ##############################################################################

set(COMPOSE_TEST_SOURCES test_compose.cpp)

foreach(test_file ${COMPOSE_TEST_SOURCES})
  string(REPLACE ".cpp" "" bin_exec ${test_file})
  string(REPLACE "/" "_" target_name "compose_${bin_exec}")

  add_executable(${target_name} ${test_file})
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${target_name} PRIVATE doctest::doctest dftracer_utils testing_utilities)
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

  add_test(NAME compose/${bin_exec} COMMAND ${target_name})
  set_tests_properties(compose/${bin_exec} PROPERTIES
                       WORKING_DIRECTORY "${test_working_directory}")
endforeach()
