# SPDX-License-Identifier: MIT
# ##############################################################################
# File I/O Library Tests
# ##############################################################################

set(FILEIO_TEST_SOURCES
    test_streaming_file_writer.cpp
    test_file_compressor.cpp
    test_file_decompressor.cpp
    test_gzip_line_writer.cpp
    test_line_format.cpp
    test_gzip_sink.cpp

    compress/test_libdeflate_gzip.cpp
    compress/test_gzip_rechunker.cpp
    compress/test_truncated_member.cpp
    compress/test_gzip_crc.cpp

    hash/test_hasher_utility.cpp

    parallel/test_layout_sizing.cpp
    parallel/test_striped_writer.cpp
    parallel/test_sharded_writer.cpp
    parallel/test_padded_striped_writer.cpp

    lines/sources/test_async_streaming_gz_line_generator.cpp
    lines/sources/test_async_streaming_plain_line_generator.cpp
)

foreach(test_file ${FILEIO_TEST_SOURCES})
  # Extract path and filename
  string(REPLACE ".cpp" "" bin_exec ${test_file})

  # Create valid target name (replace / with _)
  string(REPLACE "/" "_" target_name "fileio_${bin_exec}")

  add_executable(${target_name} ${test_file})
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${target_name} PRIVATE doctest::doctest dftracer_utils testing_utilities)
  target_set_warnings(${target_name})
  target_enable_coroutine(${target_name})

  # Set output directory to preserve folder structure
  get_filename_component(bin_dir ${bin_exec} DIRECTORY)
  get_filename_component(bin_name ${bin_exec} NAME)

  if(bin_dir)
    set_target_properties(${target_name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${bin_dir}")
  endif()

  set_target_properties(${target_name} PROPERTIES OUTPUT_NAME ${bin_name})

  # Apply coverage flags if enabled
  if(DFTRACER_UTILS_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE --coverage -fprofile-arcs
                                                   -ftest-coverage)
    target_link_libraries(${target_name} PRIVATE --coverage)
  endif()

  set(test_working_directory
      "${CMAKE_CURRENT_BINARY_DIR}/workdirs/${target_name}")
  file(MAKE_DIRECTORY "${test_working_directory}")

  add_test(NAME fileio/${bin_exec} COMMAND ${target_name})
  set_tests_properties(fileio/${bin_exec} PROPERTIES
                       WORKING_DIRECTORY "${test_working_directory}")
endforeach()

# libstdc++'s std::to_chars, inlined through common/to_chars.h, trips
# GCC's optimizer-based -Wstrict-overflow=2 inside the system header, which
# -isystem cannot suppress; the rest of the tests keep =2.
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/test_line_format.cpp
  PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-strict-overflow>")
