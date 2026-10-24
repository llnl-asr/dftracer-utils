find_program(UV_EXECUTABLE uv)
if(UV_EXECUTABLE)
  file(
    WRITE ${CMAKE_CURRENT_BINARY_DIR}/scale_ladder_smoke.cmake
    [=[
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
execute_process(
  COMMAND "${GEN}" -o "${WORK}/set/nested/trace" -p 2 -H 1 -e 4 -s 50
          --num-train-files 2 --num-val-files 1
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_fake_trace failed: ${rc}")
endif()
execute_process(
  COMMAND "${UV}" run --no-project --python ">=3.8" python "${SCRIPT}"
          --build "${BIN_DIR}" --set "smoke=${WORK}/set"
          --threads 1,2 --scratch "${WORK}/scratch" --out "${WORK}/out.tsv"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "scale_ladder.py failed: ${rc}")
endif()
execute_process(
  COMMAND "${UV}" run --no-project --python ">=3.8" python "${SCRIPT}"
          --check "${WORK}/out.tsv"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "scale_ladder.py --check failed: ${rc}")
endif()
file(WRITE "${WORK}/known.tsv" "index\tsmoke\t*\t139\tstale on purpose\n")
execute_process(
  COMMAND "${UV}" run --no-project --python ">=3.8" python "${SCRIPT}"
          --check "${WORK}/out.tsv" --known "${WORK}/known.tsv"
  RESULT_VARIABLE rc)
if(rc EQUAL 0)
  message(FATAL_ERROR "scale_ladder.py --check passed with a stale known entry")
endif()
]=])
  add_test(
    NAME scale_ladder_smoke
    COMMAND
      ${CMAKE_COMMAND} -DUV=${UV_EXECUTABLE}
      -DSCRIPT=${CMAKE_SOURCE_DIR}/scripts/bench/scale_ladder.py
      -DGEN=$<TARGET_FILE:dftracer_gen_fake_trace>
      -DBIN_DIR=$<TARGET_FILE_DIR:dftracer_index>
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/scale_ladder_smoke -P
      ${CMAKE_CURRENT_BINARY_DIR}/scale_ladder_smoke.cmake)
else()
  add_test(NAME scale_ladder_smoke
           COMMAND ${CMAKE_COMMAND} -E echo
                   "uv not found; scale ladder smoke skipped")
  set_tests_properties(scale_ladder_smoke PROPERTIES SKIP_REGULAR_EXPRESSION
                                                     "skipped")
endif()
