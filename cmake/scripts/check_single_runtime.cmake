# Check that the library makes no runtime of its own: every part runs on the
# process runtime, which only runtime/runtime.cpp makes (default_runtime(),
# configure_default_runtime()). Tests and benchmarks may build a Runtime to
# test the class itself.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_single_runtime.cmake

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "check_single_runtime.cmake requires -DSOURCE_DIR=...")
endif()

set(ALLOWED
    include/dftracer/utils/runtime/scheduler/task_executor.h
    src/dftracer/utils/runtime/runtime.cpp
    src/dftracer/utils/runtime/scheduler/task_executor.cpp)

# A Runtime or executor made by value, by make_shared/make_unique or by new.
set(PATTERN
    "((^|[^A-Za-z0-9_&*])Runtime[ \t]+[a-z_][A-Za-z0-9_]*[ \t]*[({]|make_(shared|unique)<([A-Za-z_:]*::)?(Runtime|ThreadPoolExecutor)>|new[ \t]+([A-Za-z_:]*::)?(Runtime|ThreadPoolExecutor)[ \t]*[({]|make_task_executor[ \t]*\\()"
)

file(GLOB_RECURSE sources "${SOURCE_DIR}/src/*.h" "${SOURCE_DIR}/src/*.cpp"
     "${SOURCE_DIR}/include/*.h")
list(SORT sources)

set(errors "")
foreach(path ${sources})
  file(RELATIVE_PATH rel "${SOURCE_DIR}" "${path}")
  list(FIND ALLOWED "${rel}" idx)
  if(NOT idx EQUAL -1)
    continue()
  endif()
  file(STRINGS "${path}" hits REGEX "${PATTERN}")
  foreach(line ${hits})
    string(STRIP "${line}" line)
    if(NOT line MATCHES "^(//|\\*|/\\*)")
      string(APPEND errors "  ${rel}: ${line}\n")
    endif()
  endforeach()
endforeach()

if(errors)
  message(FATAL_ERROR
          "a runtime made outside the process runtime; use default_runtime() "
          "or configure_default_runtime():\n${errors}")
endif()
message(STATUS "single runtime ok")
