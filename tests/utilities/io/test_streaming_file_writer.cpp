// Suppress GCC 14.3.0 false positive warnings
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/io/streaming_file_writer_utility.h>
#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::io;

TEST_CASE("StreamingFileWriterUtility - Basic Operations") {
    fs::path test_file = "test_streaming_writer.txt";

    SUBCASE("Write single chunk") {
        {
            StreamingFileWriterUtility writer(test_file);
            RawData chunk("Hello, World!");
            writer.process(chunk);
            writer.close();

            CHECK(writer.total_bytes() == 13);
            CHECK(writer.total_chunks() == 1);
            CHECK(writer.is_closed());
        }

        // Verify file content
        std::ifstream ifs(test_file);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "Hello, World!");

        fs::remove(test_file);
    }

    SUBCASE("Write multiple chunks") {
        {
            StreamingFileWriterUtility writer(test_file);
            writer.process(RawData("First "));
            writer.process(RawData("Second "));
            writer.process(RawData("Third"));
            writer.close();

            CHECK(writer.total_bytes() ==
                  18);  // "First " (6) + "Second " (7) + "Third" (5)
            CHECK(writer.total_chunks() == 3);
        }

        // Verify file content
        std::ifstream ifs(test_file);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "First Second Third");

        fs::remove(test_file);
    }

    SUBCASE("Write empty chunk") {
        {
            StreamingFileWriterUtility writer(test_file);
            writer.process(RawData("Before"));
            writer.process(RawData{});  // Empty chunk
            writer.process(RawData("After"));
            writer.close();

            CHECK(writer.total_bytes() == 11);
            CHECK(writer.total_chunks() == 2);  // Empty chunk not counted
        }

        // Verify file content
        std::ifstream ifs(test_file);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "BeforeAfter");

        fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileWriterUtility - Binary Data") {
    fs::path test_file = "test_binary_writer.bin";

    SUBCASE("Write binary data") {
        {
            StreamingFileWriterUtility writer(test_file);
            std::vector<unsigned char> data = {0x00, 0x01, 0x02, 0xFF, 0xFE};
            writer.process(RawData(data));
            writer.close();

            CHECK(writer.total_bytes() == 5);
        }

        // Verify binary data
        std::ifstream ifs(test_file, std::ios::binary);
        std::vector<unsigned char> content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        CHECK(content.size() == 5);
        CHECK(content[0] == 0x00);
        CHECK(content[4] == 0xFE);

        fs::remove(test_file);
    }

    SUBCASE("Write null bytes") {
        {
            StreamingFileWriterUtility writer(test_file);
            std::vector<unsigned char> data = {0x00, 0x00, 0x00};
            writer.process(RawData(data));
            writer.close();

            CHECK(writer.total_bytes() == 3);
        }

        // Verify null bytes
        std::ifstream ifs(test_file, std::ios::binary);
        std::vector<unsigned char> content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        CHECK(content.size() == 3);
        CHECK(content[0] == 0x00);
        CHECK(content[1] == 0x00);
        CHECK(content[2] == 0x00);

        fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileWriterUtility - Append Mode") {
    fs::path test_file = "test_append_writer.txt";

    // Write initial content
    {
        StreamingFileWriterUtility writer(test_file);
        writer.process(RawData("Initial content\n"));
        writer.close();
    }

    // Append to file
    {
        StreamingFileWriterUtility writer(test_file, true);  // append=true
        writer.process(RawData("Appended content\n"));
        writer.close();
    }

    // Verify both contents present
    std::ifstream ifs(test_file);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "Initial content\nAppended content\n");

    fs::remove(test_file);
}

TEST_CASE("StreamingFileWriterUtility - Directory Creation") {
    fs::path test_dir = "test_nested/dir/structure";
    fs::path test_file = test_dir / "file.txt";

    SUBCASE("Auto-create directories") {
        {
            StreamingFileWriterUtility writer(test_file, false,
                                              true);  // create_dirs=true
            writer.process(RawData("Content"));
            writer.close();
        }

        CHECK(fs::exists(test_file));
        CHECK(fs::exists(test_dir));

        fs::remove_all("test_nested");
    }

    SUBCASE("Don't create directories") {
        CHECK_THROWS_AS(StreamingFileWriterUtility(test_file, false,
                                                   false),  // create_dirs=false
                        std::runtime_error);
    }
}

TEST_CASE("StreamingFileWriterUtility - Error Handling") {
    SUBCASE("Write to closed file") {
        fs::path test_file = "test_closed_writer.txt";
        StreamingFileWriterUtility writer(test_file);
        writer.close();

        CHECK_THROWS_AS(writer.process(RawData("Data")), std::runtime_error);

        fs::remove(test_file);
    }

    SUBCASE("Invalid path") {
        // Try to write to invalid location (assuming /invalid/path doesn't
        // exist)
        CHECK_THROWS_AS(StreamingFileWriterUtility(
                            "/invalid/nonexistent/path/file.txt", false, false),
                        std::runtime_error);
    }
}

TEST_CASE("StreamingFileWriterUtility - Large Files") {
    fs::path test_file = "test_large_writer.dat";

    SUBCASE("Write 1MB in chunks") {
        {
            StreamingFileWriterUtility writer(test_file);

            // Write 1MB in 1KB chunks
            std::vector<unsigned char> chunk(1024, 0xAA);
            for (int i = 0; i < 1024; ++i) {
                writer.process(RawData(chunk));
            }
            writer.close();

            CHECK(writer.total_bytes() == 1024 * 1024);
            CHECK(writer.total_chunks() == 1024);
        }

        // Verify file size
        CHECK(fs::file_size(test_file) == 1024 * 1024);

        fs::remove(test_file);
    }

    SUBCASE("Write many small chunks") {
        {
            StreamingFileWriterUtility writer(test_file);

            // Write 10000 small chunks
            for (int i = 0; i < 10000; ++i) {
                writer.process(RawData("x"));
            }
            writer.close();

            CHECK(writer.total_bytes() == 10000);
            CHECK(writer.total_chunks() == 10000);
        }

        fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileWriterUtility - Round Trip") {
    fs::path test_file = "test_round_trip.txt";

    SUBCASE("Write and read back") {
        std::string original_data;
        {
            StreamingFileWriterUtility writer(test_file);
            for (int i = 0; i < 100; ++i) {
                std::string line = "Line " + std::to_string(i) + "\n";
                writer.process(RawData(line));
                original_data += line;
            }
            writer.close();
        }

        // Read back
        std::ifstream ifs(test_file);
        std::string read_data((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());

        CHECK(read_data == original_data);
        CHECK(read_data.size() == original_data.size());

        fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileWriterUtility - Automatic Closure") {
    fs::path test_file = "test_auto_close.txt";

    SUBCASE("Destructor closes file") {
        {
            StreamingFileWriterUtility writer(test_file);
            writer.process(RawData("Data"));
            // No explicit close() - destructor should handle it
        }

        // File should exist and be readable
        CHECK(fs::exists(test_file));

        std::ifstream ifs(test_file);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "Data");

        fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileWriterUtility - Different Data Types") {
    fs::path test_file = "test_data_types.txt";

    SUBCASE("Write text data") {
        {
            StreamingFileWriterUtility writer(test_file);
            writer.process(RawData("Text line 1\n"));
            writer.process(RawData("Text line 2\n"));
            writer.close();
        }

        std::ifstream ifs(test_file);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content.find("Text line 1") != std::string::npos);
        CHECK(content.find("Text line 2") != std::string::npos);

        fs::remove(test_file);
    }

    SUBCASE("Write binary patterns") {
        {
            StreamingFileWriterUtility writer(test_file);

            // Write alternating pattern
            for (int i = 0; i < 100; ++i) {
                unsigned char byte = (i % 2 == 0) ? 0xAA : 0x55;
                std::vector<unsigned char> data = {byte};
                writer.process(RawData(data));
            }
            writer.close();
        }

        // Verify pattern
        std::ifstream ifs(test_file, std::ios::binary);
        std::vector<unsigned char> content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        CHECK(content.size() == 100);
        CHECK(content[0] == 0xAA);
        CHECK(content[1] == 0x55);

        fs::remove(test_file);
    }
}

TEST_CASE("StreamingFileWriterUtility - Truncate vs Append") {
    fs::path test_file = "test_truncate_append.txt";

    // Write initial data
    {
        StreamingFileWriterUtility writer(test_file);
        writer.process(RawData("Initial data\n"));
        writer.close();
    }

    SUBCASE("Truncate mode overwrites") {
        {
            StreamingFileWriterUtility writer(
                test_file, false);  // append=false (truncate)
            writer.process(RawData("New data\n"));
            writer.close();
        }

        std::ifstream ifs(test_file);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "New data\n");
        CHECK(content.find("Initial") == std::string::npos);
    }

    fs::remove(test_file);
}

TEST_CASE("StreamingFileWriterUtility - Path Information") {
    fs::path test_file = "test_path_info.txt";

    {
        StreamingFileWriterUtility writer(test_file);
        CHECK(writer.path() == test_file);
        CHECK_FALSE(writer.is_closed());

        writer.process(RawData("Data"));
        writer.close();

        CHECK(writer.is_closed());
        CHECK(writer.path() == test_file);
    }

    fs::remove(test_file);
}
