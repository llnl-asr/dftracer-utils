# ##############################################################################
# Memory Library Tests
# ##############################################################################

set(MEMORY_TEST_SOURCES test_system_backend.cpp test_heap.cpp
                        test_heap_threads.cpp test_heap_trim.cpp
                        test_budget.cpp test_pool.cpp
                        test_pool_threads.cpp test_reclaim.cpp
                        test_pressure.cpp test_debug.cpp test_allocator.cpp
                        test_arena.cpp test_block.cpp test_block_threads.cpp
                        test_reserve_wait.cpp)

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

# The pool tests run once per backend: the loop above runs them on the
# system backend.
foreach(bin_exec test_heap test_pool test_pool_threads test_reclaim test_pressure
                 test_allocator test_arena test_block test_block_threads
                 test_reserve_wait)
  add_test(NAME memory/${bin_exec}_page_heap COMMAND memory_${bin_exec})
  set_tests_properties(
    memory/${bin_exec}_page_heap PROPERTIES ENVIRONMENT
                                            "DFTRACER_UTILS_MEMORY_BACKEND=page_heap")
endforeach()

# test_pool closes scratch pools with live blocks on purpose, which sets the
# leak flag the exit check reads.
set_tests_properties(
  memory/test_pool
  PROPERTIES ENVIRONMENT
             "DFTRACER_UTILS_MEMORY_BACKEND=system;DFTRACER_UTILS_MEMORY_EXIT_CHECK=0")
set_tests_properties(
  memory/test_pool_page_heap
  PROPERTIES ENVIRONMENT
             "DFTRACER_UTILS_MEMORY_BACKEND=page_heap;DFTRACER_UTILS_MEMORY_EXIT_CHECK=0")

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

  # The page heap poisons the slack after the requested size.
  add_test(NAME memory_asan_page_heap_overrun
           COMMAND memory_test_heap "--test-case=planted page heap overrun"
                   --no-skip)
  set_tests_properties(
    memory_asan_page_heap_overrun
    PROPERTIES ENVIRONMENT
               "DFTRACER_UTILS_MEMORY_BACKEND=page_heap;ASAN_OPTIONS=abort_on_error=0"
               PASS_REGULAR_EXPRESSION
               "AddressSanitizer: (heap-buffer-overflow|use-after-poison)")

  # reset() poisons the kept chunk, so a write through an old pointer is a
  # use after poison.
  add_test(NAME memory_asan_arena_use_after_reset
           COMMAND memory_test_arena "--test-case=planted arena use after reset"
                   --no-skip)
  set_tests_properties(
    memory_asan_arena_use_after_reset
    PROPERTIES ENVIRONMENT
               "DFTRACER_UTILS_MEMORY_BACKEND=page_heap;ASAN_OPTIONS=abort_on_error=0"
               PASS_REGULAR_EXPRESSION "AddressSanitizer: use-after-poison")
endif()

# A decay time that is not a whole number aborts the process at the first
# allocation; the test case forks and checks the child aborts.
add_test(NAME memory_bad_decay_value
         COMMAND memory_test_heap_trim "--test-case=bad decay value" --no-skip)
set_tests_properties(
  memory_bad_decay_value
  PROPERTIES ENVIRONMENT
             "DFTRACER_UTILS_MEMORY_BACKEND=system;DFTRACER_UTILS_MEM_DECAY_MS=fast")

# Debug mode wraps the page heap and the system backend alike. The leak cases
# plant leaks on purpose, so the exit check is off for the plain runs.
set_tests_properties(
  memory/test_debug
  PROPERTIES ENVIRONMENT
             "DFTRACER_UTILS_MEMORY_BACKEND=page_heap;DFTRACER_UTILS_MEMORY_DEBUG=1;DFTRACER_UTILS_MEMORY_EXIT_CHECK=0")
add_test(NAME memory/test_debug_system COMMAND memory_test_debug)
set_tests_properties(
  memory/test_debug_system
  PROPERTIES ENVIRONMENT
             "DFTRACER_UTILS_MEMORY_BACKEND=system;DFTRACER_UTILS_MEMORY_DEBUG=1;DFTRACER_UTILS_MEMORY_EXIT_CHECK=0")

# A process with a planted leak must exit with status 70 and the report. The
# cases run the leaking process in a child and check its status and text.
add_test(NAME memory_exit_check_leak
         COMMAND memory_test_debug "--test-case=exit check*" --no-skip)
set_tests_properties(
  memory_exit_check_leak
  PROPERTIES ENVIRONMENT
             "DFTRACER_UTILS_MEMORY_BACKEND=page_heap;DFTRACER_UTILS_MEMORY_DEBUG=1;DFTRACER_UTILS_MEMORY_EXIT_CHECK=1")

# The page heap tells Valgrind about its blocks when valgrind/memcheck.h exists.
# The test runs Valgrind itself, so tests/valgrind/run.sh runs it as a plain
# ctest test (label valgrind_direct) and does not wrap it a second time.
find_program(VALGRIND_EXECUTABLE valgrind)
if(VALGRIND_EXECUTABLE)
  add_test(
    NAME memory_valgrind_planted_leak
    COMMAND
      ${VALGRIND_EXECUTABLE} --tool=memcheck --leak-check=full
      --show-leak-kinds=definite --fair-sched=yes $<TARGET_FILE:memory_test_heap>
      "--test-case=planted page heap leak" --no-skip)
  set_tests_properties(
    memory_valgrind_planted_leak
    PROPERTIES ENVIRONMENT
               "DFTRACER_UTILS_MEMORY_BACKEND=page_heap;DFTRACER_UTILS_MEMORY_EXIT_CHECK=0"
               LABELS valgrind_direct
               PASS_REGULAR_EXPRESSION
               "777 bytes in 1 blocks are definitely lost")
endif()
