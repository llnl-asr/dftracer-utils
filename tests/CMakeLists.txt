# ##############################################################################
# Dependencies
# ##############################################################################

need_test_deps()
need_zlib()

# ##############################################################################
# Testing Utilities
# ##############################################################################

add_library(testing_utilities STATIC testing_utilities.cpp)
target_link_libraries(
  testing_utilities
  PUBLIC dftracer_utils
  PRIVATE doctest::doctest ghc_filesystem)
link_zlib(testing_utilities STATIC)

# ##############################################################################
# Unit Tests
# ##############################################################################

# +++++++++++++++++++++++++++++++++++++++++
# Test (C++ API)
# +++++++++++++++++++++++++++++++++++++++++

set(TEST_CPP_SOURCES
    test_reader.cpp
    test_basic_factory.cpp
    test_reader_formats.cpp
    # test_reader_tar_comprehensive.cpp
    test_pipeline.cpp
    test_reader_robustness.cpp)

foreach(test_file ${TEST_CPP_SOURCES})
  string(REPLACE ".cpp" "" bin_exec ${test_file})
  string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" bin_exec
                 ${bin_exec})

  add_executable(${bin_exec} ${test_file})
  target_link_libraries(${bin_exec} PRIVATE doctest::doctest testing_utilities)
  target_set_warnings(${bin_exec})

  # Apply coverage flags if enabled
  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${bin_exec} PRIVATE --coverage -fprofile-arcs
                                               -ftest-coverage)
    target_link_libraries(${bin_exec} PRIVATE --coverage)
  endif()

  add_test(NAME ${bin_exec} COMMAND ${bin_exec})
endforeach()

# +++++++++++++++++++++++++++++++++++++++++
# Test Reader C API
# +++++++++++++++++++++++++++++++++++++++++

set(TEST_C_SOURCES test_reader.c)

foreach (test_file ${TEST_C_SOURCES})
  string(REPLACE ".c" "" bin_exec ${test_file})
  string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" bin_exec
                 ${bin_exec})
  set(bin_exec "${bin_exec}_c")

  add_executable(${bin_exec} ${test_file})
  target_link_libraries(${bin_exec} PRIVATE unity_lib testing_utilities)

  # Apply coverage flags if enabled
  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${bin_exec} PRIVATE --coverage -fprofile-arcs
                                                 -ftest-coverage)
    target_link_libraries(${bin_exec} PRIVATE --coverage)
  endif()

  add_test(NAME ${bin_exec} COMMAND ${bin_exec})
endforeach()

# +++++++++++++++++++++++++++++++++++++++++
# Python Tests (SKBUILD only)
# +++++++++++++++++++++++++++++++++++++++++

if(SKBUILD)
  find_package(
    Python
    COMPONENTS Interpreter
    REQUIRED)
  add_test(
    NAME test_python_reader
    COMMAND ${Python_EXECUTABLE} -m pytest
            ${CMAKE_CURRENT_SOURCE_DIR}/python/test_reader.py -v
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(
    test_python_reader PROPERTIES ENVIRONMENT
                                  "PYTHONPATH=${CMAKE_BINARY_DIR}/python/src")
endif()

# ##############################################################################
# Package Discovery Tests
# ##############################################################################

# Test discovery using the installed package
# Use a local install directory for testing to avoid permission issues
set(TEST_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/test_install")

if(NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  add_test(
    NAME install_package
    COMMAND
      ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --prefix
      ${TEST_INSTALL_PREFIX})
  set_tests_properties(install_package PROPERTIES FIXTURES_SETUP
                                                   "installed_package")

  # ==========================================================================
  # pkg-config discovery tests
  # ==========================================================================

  # Test core library
  add_test(
    NAME test_pkgconfig_discovery_core
    COMMAND
      ${CMAKE_COMMAND}
      -DPKG_CONFIG_PATH=${TEST_INSTALL_PREFIX}/lib/pkgconfig
      -DLIBRARY_NAME=dftracer_utils_core
      -DTEST_TYPE=pkgconfig -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # Test utilities library
  add_test(
    NAME test_pkgconfig_discovery_utilities
    COMMAND
      ${CMAKE_COMMAND}
      -DPKG_CONFIG_PATH=${TEST_INSTALL_PREFIX}/lib/pkgconfig
      -DLIBRARY_NAME=dftracer_utils_utilities
      -DTEST_TYPE=pkgconfig -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # Test unified library
  add_test(
    NAME test_pkgconfig_discovery_unified
    COMMAND
      ${CMAKE_COMMAND}
      -DPKG_CONFIG_PATH=${TEST_INSTALL_PREFIX}/lib/pkgconfig
      -DLIBRARY_NAME=dftracer_utils
      -DTEST_TYPE=pkgconfig -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # ==========================================================================
  # CMake find_package discovery tests
  # ==========================================================================

  # Test core library
  add_test(
    NAME test_cmake_discovery_core
    COMMAND
      ${CMAKE_COMMAND} -DCMAKE_PREFIX_PATH=${TEST_INSTALL_PREFIX}
      -DLIBRARY_NAME=dftracer_utils_core
      -DTEST_TYPE=cmake -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # Test utilities library
  add_test(
    NAME test_cmake_discovery_utilities
    COMMAND
      ${CMAKE_COMMAND} -DCMAKE_PREFIX_PATH=${TEST_INSTALL_PREFIX}
      -DLIBRARY_NAME=dftracer_utils_utilities
      -DTEST_TYPE=cmake -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # Test unified library
  add_test(
    NAME test_cmake_discovery_unified
    COMMAND
      ${CMAKE_COMMAND} -DCMAKE_PREFIX_PATH=${TEST_INSTALL_PREFIX}
      -DLIBRARY_NAME=dftracer_utils
      -DTEST_TYPE=cmake -P
      ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)

  # ==========================================================================
  # Target alias tests
  # ==========================================================================

  # Test core library target aliases
  foreach(TARGET_ALIAS "dftracer_utils_core::shared" "dftracer_utils_core::static")
    string(REPLACE "::" "_" TEST_NAME_SUFFIX ${TARGET_ALIAS})
    add_test(
      NAME test_target_${TEST_NAME_SUFFIX}
      COMMAND
        ${CMAKE_COMMAND} -DCMAKE_PREFIX_PATH=${TEST_INSTALL_PREFIX}
        -DTARGET_ALIAS=${TARGET_ALIAS} -DTEST_TYPE=target -P
        ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)
  endforeach()

  # Test utilities library target aliases
  foreach(TARGET_ALIAS "dftracer_utils_utilities::shared" "dftracer_utils_utilities::static")
    string(REPLACE "::" "_" TEST_NAME_SUFFIX ${TARGET_ALIAS})
    add_test(
      NAME test_target_${TEST_NAME_SUFFIX}
      COMMAND
        ${CMAKE_COMMAND} -DCMAKE_PREFIX_PATH=${TEST_INSTALL_PREFIX}
        -DTARGET_ALIAS=${TARGET_ALIAS} -DTEST_TYPE=target -P
        ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)
  endforeach()

  # Test unified library target aliases (namespaced versions)
  foreach(
    TARGET_ALIAS
    "dftracer_utils"
    "dftracer_utils::static" "dftracer_utils::shared")
    string(REPLACE "::" "_" TEST_NAME_SUFFIX ${TARGET_ALIAS})
    add_test(
      NAME test_target_${TEST_NAME_SUFFIX}
      COMMAND
        ${CMAKE_COMMAND} -DCMAKE_PREFIX_PATH=${TEST_INSTALL_PREFIX}
        -DTARGET_ALIAS=${TARGET_ALIAS} -DTEST_TYPE=target -P
        ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)
  endforeach()

  # Test unified library target aliases (non-namespaced versions)
  foreach(TARGET_ALIAS "dftracer_utils_static" "dftracer_utils_shared")
    add_test(
      NAME test_target_${TARGET_ALIAS}_no_namespace
      COMMAND
        ${CMAKE_COMMAND} -DCMAKE_PREFIX_PATH=${TEST_INSTALL_PREFIX}
        -DTARGET_ALIAS=${TARGET_ALIAS} -DTEST_TYPE=target -P
        ${CMAKE_CURRENT_SOURCE_DIR}/test_package_discovery.cmake)
  endforeach()

  # ==========================================================================
  # Set test dependencies - all discovery tests should run after installation
  # ==========================================================================
  set_tests_properties(
    # pkg-config tests
    test_pkgconfig_discovery_core
    test_pkgconfig_discovery_utilities
    test_pkgconfig_discovery_unified
    # CMake discovery tests
    test_cmake_discovery_core
    test_cmake_discovery_utilities
    test_cmake_discovery_unified
    # Core target tests
    test_target_dftracer_utils_core_shared
    test_target_dftracer_utils_core_static
    # Utilities target tests
    test_target_dftracer_utils_utilities_shared
    test_target_dftracer_utils_utilities_static
    # Unified target tests (namespaced)
    test_target_dftracer_utils
    test_target_dftracer_utils_static
    test_target_dftracer_utils_shared
    # Unified target tests (non-namespaced)
    test_target_dftracer_utils_static_no_namespace
    test_target_dftracer_utils_shared_no_namespace
    PROPERTIES FIXTURES_REQUIRED "installed_package")
endif()
