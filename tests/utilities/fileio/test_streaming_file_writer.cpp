// SPDX-License-Identifier: MIT
// Suppress GCC 14.3.0 false positive warnings
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::fileio;
using namespace dftu_utils_test;
using dftracer::utils::dstd::ByteView;

TEST_CASE("StreamingFileWriterUtility - Basic Operations") {
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_streaming_writer.txt");

    SUBCASE("Write single chunk") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()));
            ByteView chunk("Hello, World!");
            writer.process(chunk).get();
            writer.close();

            CHECK(writer.total_bytes() == 13);
            CHECK(writer.total_chunks() == 1);
            CHECK(writer.is_closed());
        }

        // Verify file content
        std::ifstream ifs(test_file.c_str());
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "Hello, World!");

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("Write multiple chunks") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()));
            writer.process(ByteView("First ")).get();
            writer.process(ByteView("Second ")).get();
            writer.process(ByteView("Third")).get();
            writer.close();

            CHECK(writer.total_bytes() ==
                  18);  // "First " (6) + "Second " (7) + "Third" (5)
            CHECK(writer.total_chunks() == 3);
        }

        // Verify file content
        std::ifstream ifs(test_file.c_str());
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "First Second Third");

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("Write empty chunk") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()));
            writer.process(ByteView("Before")).get();
            writer.process(ByteView()).get();  // Empty chunk
            writer.process(ByteView("After")).get();
            writer.close();

            CHECK(writer.total_bytes() == 11);
            CHECK(writer.total_chunks() == 2);  // Empty chunk not counted
        }

        // Verify file content
        std::ifstream ifs(test_file.c_str());
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "BeforeAfter");

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileWriterUtility - Binary Data") {
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_binary_writer.bin");

    SUBCASE("Write binary data") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()));
            std::vector<unsigned char> data = {0x00, 0x01, 0x02, 0xFF, 0xFE};
            writer.process(ByteView(data)).get();
            writer.close();

            CHECK(writer.total_bytes() == 5);
        }

        // Verify binary data
        std::ifstream ifs(test_file.c_str(), std::ios::binary);
        std::vector<unsigned char> content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        CHECK(content.size() == 5);
        CHECK(content[0] == 0x00);
        CHECK(content[4] == 0xFE);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("Write null bytes") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()));
            std::vector<unsigned char> data = {0x00, 0x00, 0x00};
            writer.process(ByteView(data)).get();
            writer.close();

            CHECK(writer.total_bytes() == 3);
        }

        // Verify null bytes
        std::ifstream ifs(test_file.c_str(), std::ios::binary);
        std::vector<unsigned char> content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        CHECK(content.size() == 3);
        CHECK(content[0] == 0x00);
        CHECK(content[1] == 0x00);
        CHECK(content[2] == 0x00);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileWriterUtility - Append Mode") {
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_append_writer.txt");

    // Write initial content
    {
        StreamingFileWriterUtility writer(
            dftracer::utils::dstd::PathView(test_file.str()));
        writer.process(ByteView("Initial content\n")).get();
        writer.close();
    }

    // Append to file
    {
        StreamingFileWriterUtility writer(
            dftracer::utils::dstd::PathView(test_file.str()),
            true);  // append=true
        writer.process(ByteView("Appended content\n")).get();
        writer.close();
    }

    // Verify both contents present
    std::ifstream ifs(test_file.c_str());
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "Initial content\nAppended content\n");

    (void)dftracer::utils::dstd::fs::remove(test_file);
}

TEST_CASE("StreamingFileWriterUtility - Directory Creation") {
    dftracer::utils::dstd::Path test_root =
        make_unique_test_path("test_nested");
    dftracer::utils::dstd::Path test_dir = test_root / "dir" / "structure";
    dftracer::utils::dstd::Path test_file = test_dir / "file.txt";

    SUBCASE("Auto-create directories") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()), false,
                true);  // create_dirs=true
            writer.process(ByteView("Content")).get();
            writer.close();
        }

        CHECK(dftracer::utils::dstd::fs::exists(test_file).value());
        CHECK(dftracer::utils::dstd::fs::exists(test_dir).value());

        (void)dftracer::utils::dstd::fs::remove_all(test_root);
    }

    SUBCASE("Don't create directories") {
        CHECK_THROWS_AS(
            StreamingFileWriterUtility(
                dftracer::utils::dstd::PathView(test_file.str()), false,
                false),  // create_dirs=false
            dftracer::utils::common::DFTUtilsException);
    }
}

TEST_CASE("StreamingFileWriterUtility - Error Handling") {
    SUBCASE("Write to closed file") {
        dftracer::utils::dstd::Path test_file =
            make_unique_test_path("test_closed_writer.txt");
        StreamingFileWriterUtility writer(
            dftracer::utils::dstd::PathView(test_file.str()));
        writer.close();

        CHECK_THROWS_AS(writer.process(ByteView("Data")).get(),
                        dftracer::utils::common::DFTUtilsException);

        (void)dftracer::utils::dstd::fs::remove(test_file);
    }

    SUBCASE("Invalid path") {
        // Try to write to invalid location (assuming /invalid/path doesn't
        // exist)
        CHECK_THROWS_AS(StreamingFileWriterUtility(
                            dftracer::utils::dstd::PathView(
                                "/invalid/nonexistent/path/file.txt"),
                            false, false),
                        dftracer::utils::common::DFTUtilsException);
    }
}

TEST_CASE("StreamingFileWriterUtility - Large Files") {
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_large_writer.dat");

    SUBCASE("Write 1MB in chunks") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()));

            // Write 1MB in 1KB chunks
            std::vector<unsigned char> chunk(1024, 0xAA);
            for (int i = 0; i < 1024; ++i) {
                writer.process(ByteView(chunk)).get();
            }
            writer.close();

            CHECK(writer.total_bytes() == 1024 * 1024);
            CHECK(writer.total_chunks() == 1024);
        }

        // Verify file size
        CHECK(dftracer::utils::dstd::fs::file_size(test_file).value() ==
              1024 * 1024);

        dftracer::utils::dstd::fs::remove(test_file).has_value();
    }

    SUBCASE("Write many small chunks") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()));

            // Write 10000 small chunks
            for (int i = 0; i < 10000; ++i) {
                writer.process(ByteView("x")).get();
            }
            writer.close();

            CHECK(writer.total_bytes() == 10000);
            CHECK(writer.total_chunks() == 10000);
        }

        dftracer::utils::dstd::fs::remove(test_file).has_value();
    }
}

TEST_CASE("StreamingFileWriterUtility - Round Trip") {
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_round_trip.txt");

    SUBCASE("Write and read back") {
        std::string original_data;
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()));
            for (int i = 0; i < 100; ++i) {
                std::string line = "Line " + std::to_string(i) + "\n";
                writer.process(ByteView(line)).get();
                original_data += line;
            }
            writer.close();
        }

        // Read back
        std::ifstream ifs(test_file.c_str());
        std::string read_data((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());

        CHECK(read_data == original_data);
        CHECK(read_data.size() == original_data.size());

        dftracer::utils::dstd::fs::remove(test_file).has_value();
    }
}

TEST_CASE("StreamingFileWriterUtility - Automatic Closure") {
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_auto_close.txt");

    SUBCASE("Destructor closes file") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()));
            writer.process(ByteView("Data")).get();
            // No explicit close() - destructor should handle it
        }

        // File should exist and be readable
        CHECK(dftracer::utils::dstd::fs::exists(test_file).value());

        std::ifstream ifs(test_file.c_str());
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "Data");

        dftracer::utils::dstd::fs::remove(test_file).has_value();
    }
}

TEST_CASE("StreamingFileWriterUtility - Different Data Types") {
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_data_types.txt");

    SUBCASE("Write text data") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()));
            writer.process(ByteView("Text line 1\n")).get();
            writer.process(ByteView("Text line 2\n")).get();
            writer.close();
        }

        std::ifstream ifs(test_file.c_str());
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content.find("Text line 1") != std::string::npos);
        CHECK(content.find("Text line 2") != std::string::npos);

        dftracer::utils::dstd::fs::remove(test_file).has_value();
    }

    SUBCASE("Write binary patterns") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()));

            // Write alternating pattern
            for (int i = 0; i < 100; ++i) {
                unsigned char byte = (i % 2 == 0) ? 0xAA : 0x55;
                std::vector<unsigned char> data = {byte};
                writer.process(ByteView(data)).get();
            }
            writer.close();
        }

        // Verify pattern
        std::ifstream ifs(test_file.c_str(), std::ios::binary);
        std::vector<unsigned char> content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        CHECK(content.size() == 100);
        CHECK(content[0] == 0xAA);
        CHECK(content[1] == 0x55);

        dftracer::utils::dstd::fs::remove(test_file).has_value();
    }
}

TEST_CASE("StreamingFileWriterUtility - Truncate vs Append") {
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_truncate_append.txt");

    // Write initial data
    {
        StreamingFileWriterUtility writer(
            dftracer::utils::dstd::PathView(test_file.str()));
        writer.process(ByteView("Initial data\n")).get();
        writer.close();
    }

    SUBCASE("Truncate mode overwrites") {
        {
            StreamingFileWriterUtility writer(
                dftracer::utils::dstd::PathView(test_file.str()),
                false);  // append=false (truncate)
            writer.process(ByteView("New data\n")).get();
            writer.close();
        }

        std::ifstream ifs(test_file.c_str());
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "New data\n");
        CHECK(content.find("Initial") == std::string::npos);
    }

    dftracer::utils::dstd::fs::remove(test_file).has_value();
}

TEST_CASE("StreamingFileWriterUtility - Path Information") {
    dftracer::utils::dstd::Path test_file =
        make_unique_test_path("test_path_info.txt");

    {
        StreamingFileWriterUtility writer(
            dftracer::utils::dstd::PathView(test_file.str()));
        CHECK(writer.path().str() == test_file.str());
        CHECK_FALSE(writer.is_closed());

        writer.process(ByteView("Data")).get();
        writer.close();

        CHECK(writer.is_closed());
        CHECK(writer.path().str() == test_file.str());
    }

    dftracer::utils::dstd::fs::remove(test_file).has_value();
}
