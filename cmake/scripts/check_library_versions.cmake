# Check that each built shared library carries the interface versions in
# VERSION: SONAME lib<name>.so.<abi> on Linux, compatibility version <abi> and
# current version <api> on macOS. Also check that each shared library defines
# every global text symbol of its static archive, since both link the same
# object files. The shared library may define more: symbols of the static
# libraries it bundles. Also check the libraries of ours that each shared
# library needs: memory needs none, common needs only memory, and json and duql
# do not need core. Library
# names come from the create_library NAME arguments in src/CMakeLists.txt.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -DLIB_DIR=<build>/lib
#              [-DVERSION_FILE=<file>] -P check_library_versions.cmake

cmake_minimum_required(VERSION 3.20)

foreach(_var SOURCE_DIR LIB_DIR)
  if(NOT DEFINED ${_var})
    message(FATAL_ERROR "check_library_versions.cmake requires -D${_var}=...")
  endif()
endforeach()
if(NOT DEFINED VERSION_FILE)
  set(VERSION_FILE "${SOURCE_DIR}/VERSION")
endif()

foreach(_key api abi)
  file(STRINGS "${VERSION_FILE}" _line REGEX "^${_key}:")
  string(REGEX REPLACE "^${_key}:[ \t]*" "" _${_key} "${_line}")
  string(STRIP "${_${_key}}" _${_key})
  if(NOT _${_key} MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "${VERSION_FILE}: bad '${_key}' line '${_${_key}}'")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/src/CMakeLists.txt" _src)
string(REGEX MATCHALL "create_library\\([ \t\r\n]*NAME[ \t\r\n]+[A-Za-z0-9_]+" _calls
             "${_src}")
set(_names "")
foreach(_call ${_calls})
  string(REGEX REPLACE ".*NAME[ \t\r\n]+" "" _name "${_call}")
  list(APPEND _names "${_name}")
endforeach()
list(LENGTH _names _count)
if(_count EQUAL 0)
  message(FATAL_ERROR "no create_library NAME found in src/CMakeLists.txt")
endif()

find_program(_nm nm REQUIRED)
find_program(_sort sort REQUIRED)
find_program(_awk awk REQUIRED)
find_program(_comm comm REQUIRED)
if(APPLE)
  set(_ext dylib)
  # nm -m names weak definitions, which a dylib does not keep one for each
  # object that has them.
  set(_nm_flags -gUm)
  set(_text_filter "$2 == \"(__TEXT,__text)\" && $3 == \"external\" { print $NF }")
  find_program(_otool otool REQUIRED)
else()
  set(_ext so)
  set(_nm_flags -g --defined-only)
  set(_text_filter "$2 == \"T\" { print $3 }")
  find_program(_readelf readelf REQUIRED)
endif()

# Writes the sorted global text symbols of a library or archive to <out>.
function(_text_symbols lib out)
  get_filename_component(_dir "${out}" DIRECTORY)
  file(MAKE_DIRECTORY "${_dir}")
  execute_process(
    COMMAND "${_nm}" ${_nm_flags} "${lib}"
    COMMAND "${_awk}" "${_text_filter}"
    COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C "${_sort}" -u
    OUTPUT_FILE "${out}"
    RESULT_VARIABLE _rcs
    ERROR_QUIET)
  foreach(_rc ${_rcs})
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "cannot list symbols of ${lib}")
    endif()
  endforeach()
endfunction()

# Sets <out> to up to 10 symbols that are in file <a> and not in file <b>.
function(_symbols_only_in a b out)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C "${_comm}" -23 "${a}" "${b}"
    OUTPUT_VARIABLE _only
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "comm failed on ${a} and ${b}")
  endif()
  string(STRIP "${_only}" _only)
  string(REPLACE "\n" ";" _only "${_only}")
  list(LENGTH _only _n)
  if(_n GREATER 10)
    list(SUBLIST _only 0 10 _only)
  endif()
  set(${out} "${_only}" PARENT_SCOPE)
endfunction()

# Sets <out> to the dftracer_utils_<x> libraries that a shared library needs.
function(_needed_libraries lib out)
  if(APPLE)
    execute_process(
      COMMAND "${_otool}" -L "${lib}"
      OUTPUT_VARIABLE _dump
      RESULT_VARIABLE _rc)
    set(_pattern "[ \t]([^ \t\n]*/)?lib(dftracer_utils_[a-z_]+)[.0-9]*\\.dylib")
  else()
    execute_process(
      COMMAND "${_readelf}" -d "${lib}"
      OUTPUT_VARIABLE _dump
      RESULT_VARIABLE _rc)
    set(_pattern "\\(NEEDED\\)[^\n]*\\[lib(dftracer_utils_[a-z_]+)\\.so")
  endif()
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "cannot list needed libraries of ${lib}")
  endif()
  string(REGEX MATCHALL "${_pattern}" _hits "${_dump}")
  set(_libs "")
  foreach(_hit ${_hits})
    string(REGEX REPLACE ".*lib(dftracer_utils_[a-z_]+)[.0-9]*\\.(dylib|so).*" "\\1" _lib
                         "${_hit}")
    list(APPEND _libs "${_lib}")
  endforeach()
  list(REMOVE_DUPLICATES _libs)
  set(${out} "${_libs}" PARENT_SCOPE)
endfunction()

set(_failed FALSE)
foreach(_name ${_names})
  set(_file "${LIB_DIR}/lib${_name}.${_ext}")
  if(NOT EXISTS "${_file}")
    message(SEND_ERROR "${_name}: ${_file} is missing")
    set(_failed TRUE)
    continue()
  endif()
  if(APPLE)
    execute_process(
      COMMAND "${_otool}" -L "${_file}"
      OUTPUT_VARIABLE _out
      RESULT_VARIABLE _rc)
    string(REGEX MATCH
                 "\n[^\n]*\\(compatibility version ([0-9.]+), current version ([0-9.]+)\\)"
                 _match "${_out}")
    if(NOT _rc EQUAL 0 OR _match STREQUAL "")
      message(SEND_ERROR "${_name}: cannot read otool -L output for ${_file}")
      set(_failed TRUE)
      continue()
    endif()
    set(_got_compat "${CMAKE_MATCH_1}")
    set(_got_current "${CMAKE_MATCH_2}")
    if(NOT _got_compat STREQUAL _abi OR NOT _got_current STREQUAL _api)
      message(
        SEND_ERROR
          "${_name}: compatibility version ${_got_compat} current version ${_got_current}, expected ${_abi} and ${_api} from VERSION"
      )
      set(_failed TRUE)
    endif()
  else()
    execute_process(
      COMMAND "${_readelf}" -d "${_file}"
      OUTPUT_VARIABLE _out
      RESULT_VARIABLE _rc)
    string(REGEX MATCH "SONAME\\)[^\n]*\\[([^]\n]*)\\]" _match "${_out}")
    set(_got "${CMAKE_MATCH_1}")
    if(NOT _rc EQUAL 0 OR _match STREQUAL "")
      message(SEND_ERROR "${_name}: no SONAME in ${_file}")
      set(_failed TRUE)
      continue()
    endif()
    if(NOT _got STREQUAL "lib${_name}.so.${_abi}")
      message(
        SEND_ERROR
          "${_name}: SONAME ${_got}, expected lib${_name}.so.${_abi} from VERSION")
      set(_failed TRUE)
    endif()
  endif()
endforeach()

foreach(_name ${_names})
  set(_file "${LIB_DIR}/lib${_name}.${_ext}")
  if(NOT EXISTS "${_file}")
    continue()
  endif()
  _needed_libraries("${_file}" _needed)
  list(REMOVE_ITEM _needed "${_name}")
  if(_name STREQUAL "dftracer_utils_memory" AND _needed)
    message(SEND_ERROR "${_name}: needs ${_needed}, but memory needs no library of ours")
    set(_failed TRUE)
  endif()
  if(_name STREQUAL "dftracer_utils_common")
    set(_above_memory ${_needed})
    list(REMOVE_ITEM _above_memory "dftracer_utils_memory")
    if(_above_memory)
      message(SEND_ERROR "${_name}: needs ${_above_memory}, but common needs only memory")
      set(_failed TRUE)
    endif()
  endif()
  if((_name STREQUAL "dftracer_utils_json" OR _name STREQUAL "dftracer_utils_duql")
     AND "dftracer_utils_core" IN_LIST _needed)
    message(SEND_ERROR "${_name}: needs dftracer_utils_core (needs: ${_needed})")
    set(_failed TRUE)
  endif()
endforeach()

foreach(_name ${_names})
  set(_shared "${LIB_DIR}/lib${_name}.${_ext}")
  set(_static "${LIB_DIR}/lib${_name}.a")
  if(NOT EXISTS "${_shared}")
    continue()
  endif()
  if(NOT EXISTS "${_static}")
    message(STATUS "${_name}: no static archive in ${LIB_DIR}, "
                   "skipping the symbol comparison")
    continue()
  endif()
  set(_tmp "${LIB_DIR}/../Testing/Temporary/library_versions_${_name}")
  _text_symbols("${_shared}" "${_tmp}.shared")
  _text_symbols("${_static}" "${_tmp}.static")
  file(SIZE "${_tmp}.static" _static_size)
  if(_static_size EQUAL 0)
    message(SEND_ERROR "${_name}: ${_static} lists no text symbols")
    set(_failed TRUE)
    file(REMOVE "${_tmp}.shared" "${_tmp}.static")
    continue()
  endif()
  _symbols_only_in("${_tmp}.static" "${_tmp}.shared" _missing_shared)
  file(REMOVE "${_tmp}.shared" "${_tmp}.static")
  if(_missing_shared)
    string(REPLACE ";" "\n    " _a "${_missing_shared}")
    message(
      SEND_ERROR
        "${_name}: the shared library lacks text symbols that the static archive defines (first 10):\n    ${_a}"
    )
    set(_failed TRUE)
  endif()
endforeach()

if(_failed)
  message(FATAL_ERROR "check_library_versions: mismatch with ${VERSION_FILE}")
endif()
message(STATUS "library_versions: ${_count} libraries match abi ${_abi} api ${_api}")
