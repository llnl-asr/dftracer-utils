# ##############################################################################
# Utilities Tests
# ##############################################################################

# This file contains tests for dftracer utilities organized by category.
# Each utility has its own test file for easy maintenance.

# +++++++++++++++++++++++++++++++++++++++++
# Utilities Test Sources
# +++++++++++++++++++++++++++++++++++++++++

set(UTILITIES_TEST_SOURCES
    # Composites
    composites/test_batch_processor.cpp
    composites/test_chunk_verifier.cpp
    composites/test_directory_file_processor.cpp
    composites/test_file_compressor.cpp
    composites/test_file_decompressor.cpp
    composites/test_file_merger.cpp
    composites/test_indexed_file_reader.cpp
    composites/test_line_batch_processor.cpp

    # DFT Composites
    composites/dft/test_chunk_extractor.cpp
    composites/dft/test_event_collector.cpp
    composites/dft/test_event_hasher.cpp
    composites/dft/test_index_builder.cpp
    composites/dft/test_metadata_collector.cpp

    # Compression
    compression/zlib/test_compressor.cpp
    compression/zlib/test_decompressor.cpp
    compression/zlib/test_streaming_compressor.cpp
    compression/zlib/test_streaming_decompressor.cpp

    # Filesystem
    filesystem/test_directory_scanner.cpp
    filesystem/test_pattern_directory_scanner.cpp

    # Hash
    hash/test_hasher_utility.cpp
    hash/test_mt_hasher.cpp
    hash/test_xxh3_hasher.cpp
    hash/test_xxh64_hasher.cpp
    hash/test_std_hasher.cpp

    # I/O
    io/test_binary_file_reader.cpp
    io/test_file_reader.cpp
    io/test_streaming_file_reader.cpp
    io/test_streaming_file_writer.cpp

    # I/O Lines
    io/lines/test_streaming_line_reader.cpp
    io/lines/sources/test_indexed_file_line_iterator.cpp
    io/lines/sources/test_indexed_file_bytes_iterator.cpp
    io/lines/sources/test_plain_file_line_iterator.cpp
    io/lines/sources/test_plain_file_bytes_iterator.cpp

    # Text
    text/test_line_filter.cpp
    text/test_line_splitter.cpp
)

# +++++++++++++++++++++++++++++++++++++++++
# Build Configuration
# +++++++++++++++++++++++++++++++++++++++++

foreach(test_file ${UTILITIES_TEST_SOURCES})
  # Extract path and filename
  string(REPLACE ".cpp" "" bin_exec ${test_file})

  # Create valid target name (replace / with _)
  string(REPLACE "/" "_" target_name "utilities_${bin_exec}")

  add_executable(${target_name} ${test_file})
  target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${target_name} PRIVATE doctest::doctest dftracer_utils testing_utilities)
  target_set_warnings(${target_name})

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

  add_test(NAME utilities/${bin_exec} COMMAND ${target_name})
endforeach()
