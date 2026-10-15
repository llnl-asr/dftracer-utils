# Check .git-blame-ignore-revs: every entry is a commit, and every file it
# changes is a pure rename or equals its parent version once all whitespace is
# removed. Skips on a shallow clone, where parents are missing.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P check_blame_ignore_revs.cmake

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "check_blame_ignore_revs.cmake requires -DSOURCE_DIR=...")
endif()

find_program(GIT_EXECUTABLE git)
if(NOT GIT_EXECUTABLE)
  message(FATAL_ERROR "git not found")
endif()

function(run_git OUT_VAR)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} ${ARGN}
    WORKING_DIRECTORY ${SOURCE_DIR}
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE rc)
  set(${OUT_VAR} "${out}" PARENT_SCOPE)
  set(${OUT_VAR}_RC ${rc} PARENT_SCOPE)
endfunction()

function(squeeze OUT_VAR REV PATH)
  run_git(content show ${REV}:${PATH})
  if(NOT content_RC EQUAL 0)
    message(FATAL_ERROR "git show ${REV}:${PATH} failed")
  endif()
  string(REGEX REPLACE "[ \t\r\n]" "" content "${content}")
  set(${OUT_VAR} "${content}" PARENT_SCOPE)
endfunction()

run_git(shallow rev-parse --is-shallow-repository)
string(STRIP "${shallow}" shallow)
if(NOT shallow_RC EQUAL 0)
  message(FATAL_ERROR "${SOURCE_DIR} is not a git repository")
endif()
if(shallow STREQUAL "true")
  message("blame_ignore_revs: shallow clone, skipping")
  return()
endif()

set(LIST_FILE ${SOURCE_DIR}/.git-blame-ignore-revs)
if(NOT EXISTS ${LIST_FILE})
  message(FATAL_ERROR "${LIST_FILE} is missing")
endif()
file(STRINGS ${LIST_FILE} LINES)

foreach(line IN LISTS LINES)
  string(STRIP "${line}" rev)
  if(rev STREQUAL "" OR rev MATCHES "^#")
    continue()
  endif()

  run_git(type cat-file -t ${rev})
  string(STRIP "${type}" type)
  if(NOT type_RC EQUAL 0 OR NOT type STREQUAL "commit")
    message(FATAL_ERROR "${rev}: not a commit")
  endif()

  run_git(changes diff-tree -r -M100% --name-status ${rev}^ ${rev})
  if(NOT changes_RC EQUAL 0)
    message(FATAL_ERROR "${rev}: has no parent to compare with")
  endif()
  string(REPLACE "\n" ";" changes "${changes}")
  foreach(change IN LISTS changes)
    if(change STREQUAL "")
      continue()
    endif()
    string(REPLACE "\t" ";" fields "${change}")
    list(GET fields 0 status)
    list(GET fields 1 path)
    if(status STREQUAL "R100")
      continue()
    endif()
    if(NOT status STREQUAL "M")
      message(FATAL_ERROR "${rev}: ${path} is not formatting only (status ${status})")
    endif()
    squeeze(before ${rev}^ ${path})
    squeeze(after ${rev} ${path})
    if(NOT before STREQUAL after)
      message(FATAL_ERROR "${rev}: ${path} differs beyond whitespace")
    endif()
  endforeach()
endforeach()
