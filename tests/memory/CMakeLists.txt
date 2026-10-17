# ##############################################################################
# Memory Library Tests
# ##############################################################################

set(MEMORY_TEST_SOURCES test_system_backend.cpp test_heap.cpp
                        test_heap_threads.cpp test_heap_trim.cpp
                        test_memory_pool.cpp)

find_package(Threads REQUIRED)

if(TARGET dftracer_utils_memory::shared)
  set(MEMORY_TEST_LIBRARY dftracer_utils_memory::shared)
else()
  set(MEMORY_TEST_LIBRARY dftracer_utils_memory::static)
endif()

foreach(test_file ${MEMORY_TEST_SOURCES})
  string(REPLACE ".cpp" "" bin_exec ${test_file})
  set(target_name "memory_${bin_exec}")

  add_executable(${target_name} ${test_file})
  target_link_libraries(${target_name} PRIVATE doctest::doctest
                                               ${MEMORY_TEST_LIBRARY}
                                               Threads::Threads)
  target_set_warnings(${target_name})
  target_add_rpath(${target_name})
  set_target_properties(${target_name} PROPERTIES OUTPUT_NAME ${bin_exec})

  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE --coverage -fprofile-arcs
                                                   -ftest-coverage)
    target_link_libraries(${target_name} PRIVATE --coverage)
  endif()

  add_test(NAME memory/${bin_exec} COMMAND ${target_name})
  set_tests_properties(
    memory/${bin_exec} PROPERTIES ENVIRONMENT
                                  "DFTRACER_UTILS_MEMORY_BACKEND=system")
endforeach()

if(DFTRACER_UTILS_ENABLE_ASAN)
  # abort_on_error=0: ctest fails a test killed by a signal before it checks
  # the output.
  add_test(NAME memory_asan_overrun
           COMMAND memory_test_system_backend "--test-case=planted overrun"
                   --no-skip)
  set_tests_properties(
    memory_asan_overrun
    PROPERTIES ENVIRONMENT
               "DFTRACER_UTILS_MEMORY_BACKEND=system;ASAN_OPTIONS=abort_on_error=0"
               PASS_REGULAR_EXPRESSION "AddressSanitizer: heap-buffer-overflow")
endif()

# A decay time that is not a whole number aborts the process at the first
# allocation; the test case forks and checks the child aborts.
add_test(NAME memory_bad_decay_value
         COMMAND memory_test_heap_trim "--test-case=bad decay value" --no-skip)
set_tests_properties(
  memory_bad_decay_value
  PROPERTIES ENVIRONMENT
             "DFTRACER_UTILS_MEMORY_BACKEND=system;DFTRACER_UTILS_MEM_DECAY_MS=fast")
