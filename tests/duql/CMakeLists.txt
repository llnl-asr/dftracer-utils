find_program(UV_EXECUTABLE uv)
if(UV_EXECUTABLE)
  add_test(
    NAME duql_grammar
    COMMAND
      ${UV_EXECUTABLE} run --no-project --with lark python
      ${CMAKE_SOURCE_DIR}/scripts/check_duql_grammar.py --samples
      ${CMAKE_CURRENT_SOURCE_DIR}/grammar_samples.duql)
else()
  add_test(NAME duql_grammar COMMAND ${CMAKE_COMMAND} -E echo
                                     "uv not found; duql grammar check skipped")
  set_tests_properties(duql_grammar PROPERTIES SKIP_REGULAR_EXPRESSION
                                               "skipped")
endif()

foreach(name test_duql_syntax test_duql_golden test_parser test_evaluator
             test_query test_builder test_subsumption test_differential
             test_patterns test_pattern_corpus test_vectorize test_lookup
             test_overlap)
  set(target duql_${name})
  add_executable(${target} ${name}.cpp)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src
                                               ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${target} PRIVATE doctest::doctest dftracer_utils
                                          testing_utilities)
  target_compile_definitions(
    ${target}
    PRIVATE DUQL_SAMPLES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/grammar_samples.duql"
            DUQL_GOLDEN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/golden_filters.jsonl")
  target_set_warnings(${target})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME ${name})
  target_enable_coroutine(${target})
  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE --coverage -fprofile-arcs
                                             -ftest-coverage)
    target_link_libraries(${target} PRIVATE --coverage)
  endif()
  set(workdir "${CMAKE_CURRENT_BINARY_DIR}/workdirs/${target}")
  file(MAKE_DIRECTORY "${workdir}")
  add_test(NAME duql/${name} COMMAND ${target})
  set_tests_properties(duql/${name} PROPERTIES WORKING_DIRECTORY "${workdir}")
endforeach()
