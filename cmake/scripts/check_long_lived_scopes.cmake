# Check that only reviewed owners make a long-lived CoroScope. Such a scope is
# a member that outlives the call that made it, so its owner must hold
# everything the children touch by pointer and join in its destructor (see
# fileio/long_lived_release.h). A new caller goes into ALLOWED after review.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_long_lived_scopes.cmake

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "check_long_lived_scopes.cmake requires -DSOURCE_DIR=...")
endif()

set(ALLOWED
    src/dftracer/utils/utilities/fileio/gzip_line_writer.cpp
    src/dftracer/utils/utilities/fileio/parallel/padded_striped_writer.cpp)

file(GLOB_RECURSE sources "${SOURCE_DIR}/src/*.h" "${SOURCE_DIR}/src/*.cpp"
     "${SOURCE_DIR}/include/*.h" "${SOURCE_DIR}/python/*.cpp")
list(SORT sources)

set(errors "")
foreach(path ${sources})
  file(RELATIVE_PATH rel "${SOURCE_DIR}" "${path}")
  list(FIND ALLOWED "${rel}" idx)
  if(NOT idx EQUAL -1)
    continue()
  endif()
  file(STRINGS "${path}" hits REGEX "CoroScope::long_lived\\(")
  foreach(line ${hits})
    string(STRIP "${line}" line)
    if(NOT line MATCHES "^//")
      string(APPEND errors "  ${rel}: ${line}\n")
    endif()
  endforeach()
endforeach()

if(errors)
  message(FATAL_ERROR
          "CoroScope::long_lived called outside the reviewed owners:\n${errors}")
endif()
message(STATUS "long-lived scopes ok")
