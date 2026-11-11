# SPDX-License-Identifier: MIT
foreach(name test_document test_tape test_records test_writer test_line_index)
  set(target yaml_${name})
  add_executable(${target} ${name}.cpp)
  target_link_libraries(${target} PRIVATE doctest::doctest dftracer_utils testing_utilities)
  target_set_warnings(${target})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME ${name})
  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE --coverage -fprofile-arcs
                                             -ftest-coverage)
    target_link_libraries(${target} PRIVATE --coverage)
  endif()
  set(workdir "${CMAKE_CURRENT_BINARY_DIR}/workdirs/${target}")
  file(MAKE_DIRECTORY "${workdir}")
  add_test(NAME yaml/${name} COMMAND ${target})
  set_tests_properties(yaml/${name} PROPERTIES WORKING_DIRECTORY "${workdir}")
endforeach()

# The cases of the YAML test suite, a data tag of its repository. Only the
# tests read them: nothing of it is built or installed.
cpmaddpackage(
  NAME
  yaml_test_suite
  GITHUB_REPOSITORY
  yaml/yaml-test-suite
  GIT_TAG
  data-2022-01-17
  DOWNLOAD_ONLY
  YES)
target_compile_definitions(
  yaml_test_line_index
  PRIVATE DFTRACER_UTILS_YAML_TEST_SUITE_DIR="${yaml_test_suite_SOURCE_DIR}")
