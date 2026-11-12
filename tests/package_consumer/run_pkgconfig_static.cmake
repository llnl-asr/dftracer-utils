# SPDX-License-Identifier: MIT
# Links a program against the static archives of the installed tree, with the
# libraries pkg-config reports for dftracer_utils_domains_static, and runs it.
# Each -l<name> that has an archive in the install becomes that archive, so the
# shared libraries beside it do not stand in for a missing Requires entry.
find_program(PKG_CONFIG NAMES pkg-config pkgconf)
if(NOT PKG_CONFIG)
  message(STATUS "pkgconfig_static: skipped, no pkg-config")
  return()
endif()
set(ENV{PKG_CONFIG_PATH} "${PREFIX}/lib/pkgconfig:${PREFIX}/lib64/pkgconfig")
execute_process(
  COMMAND "${PKG_CONFIG}" "--define-variable=prefix=${PREFIX}" --static --libs
          --cflags dftracer_utils_domains_static
  OUTPUT_VARIABLE flags
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "pkg-config does not find dftracer_utils_domains_static: ${rc}")
endif()
separate_arguments(flags NATIVE_COMMAND "${flags}")
# Each -l<name> with an archive in the install becomes that archive, in the
# order pkg-config gave, so a shared library cannot stand in for it and a wrong
# order fails on a linker that reads archives once.
set(args)
foreach(flag IN LISTS flags)
  set(archive)
  if(flag MATCHES "^-l(.+)$")
    foreach(libdir lib lib64)
      if(EXISTS "${PREFIX}/${libdir}/lib${CMAKE_MATCH_1}.a")
        set(archive "${PREFIX}/${libdir}/lib${CMAKE_MATCH_1}.a")
      endif()
    endforeach()
  endif()
  if(archive)
    list(APPEND args "${archive}")
  elseif(NOT flag MATCHES "^-L")
    list(APPEND args "${flag}")
  endif()
endforeach()
file(MAKE_DIRECTORY "${WORK_DIR}")
separate_arguments(cxx_flags NATIVE_COMMAND "${CXX_FLAGS}")
execute_process(
  COMMAND "${CXX_COMPILER}" -std=c++20 ${cxx_flags}
          "${SOURCE_DIR}/pkgconfig_static_consumer.cpp" ${args}
          -o "${WORK_DIR}/pkgconfig_static_consumer"
  RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "the static pkg-config consumer does not link: ${rc}")
endif()
# The default runtime outlives main, so the leak check of the test presets does
# not apply here.
set(ENV{DFTRACER_UTILS_MEMORY_EXIT_CHECK} 0)
execute_process(COMMAND "${WORK_DIR}/pkgconfig_static_consumer"
                RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "the static pkg-config consumer failed: ${rc}")
endif()
