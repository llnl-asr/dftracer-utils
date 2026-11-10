// SPDX-License-Identifier: MIT
// Suppress GCC 14.3.0 false positive warnings
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

using namespace dftracer::utils;

using namespace dftracer::utils::utilities::fileio;
using namespace dftu_utils_test;

TEST_CASE("StreamingFileWriterUtility - Basic Operations") {
    dstd::Path test_file = make_unique_test_path("test_streaming_writer.txt");

    SUBCASE("Write single chunk") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str())));
            dstd::ByteView chunk("Hello, World!");
            dftu_utils_test::ok(writer.process(chunk).get());
            writer.close();

            CHECK(writer.total_bytes() == 13);
            CHECK(writer.total_chunks() == 1);
            CHECK(writer.is_closed());
        }

        // Verify file content
        dstd::String content = dstd::fs::read_file(dstd::PathView(test_file))
                                   .value_or(dstd::String());
        CHECK(content == "Hello, World!");

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("Write multiple chunks") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str())));
            dftu_utils_test::ok(writer.process(dstd::ByteView("First ")).get());
            dftu_utils_test::ok(
                writer.process(dstd::ByteView("Second ")).get());
            dftu_utils_test::ok(writer.process(dstd::ByteView("Third")).get());
            writer.close();

            CHECK(writer.total_bytes() ==
                  18);  // "First " (6) + "Second " (7) + "Third" (5)
            CHECK(writer.total_chunks() == 3);
        }

        // Verify file content
        dstd::String content = dstd::fs::read_file(dstd::PathView(test_file))
                                   .value_or(dstd::String());
        CHECK(content == "First Second Third");

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("Write empty chunk") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str())));
            dftu_utils_test::ok(writer.process(dstd::ByteView("Before")).get());
            dftu_utils_test::ok(
                writer.process(dstd::ByteView()).get());  // Empty chunk
            dftu_utils_test::ok(writer.process(dstd::ByteView("After")).get());
            writer.close();

            CHECK(writer.total_bytes() == 11);
            CHECK(writer.total_chunks() == 2);  // Empty chunk not counted
        }

        // Verify file content
        dstd::String content = dstd::fs::read_file(dstd::PathView(test_file))
                                   .value_or(dstd::String());
        CHECK(content == "BeforeAfter");

        dstd::fs::remove(test_file).discard();
    }
}

TEST_CASE("StreamingFileWriterUtility - Binary Data") {
    dstd::Path test_file = make_unique_test_path("test_binary_writer.bin");

    SUBCASE("Write binary data") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str())));
            dstd::Vector<unsigned char> data = {0x00, 0x01, 0x02, 0xFF, 0xFE};
            dftu_utils_test::ok(
                writer.process(dstd::ByteView(data.data(), data.size())).get());
            writer.close();

            CHECK(writer.total_bytes() == 5);
        }

        // Verify binary data
        const dstd::String content_raw =
            dstd::fs::read_file(dstd::PathView(test_file))
                .value_or(dstd::String());
        dstd::Vector<unsigned char> content(content_raw.begin(),
                                            content_raw.end());
        CHECK(content.size() == 5);
        CHECK(content[0] == 0x00);
        CHECK(content[4] == 0xFE);

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("Write null bytes") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str())));
            dstd::Vector<unsigned char> data = {0x00, 0x00, 0x00};
            dftu_utils_test::ok(
                writer.process(dstd::ByteView(data.data(), data.size())).get());
            writer.close();

            CHECK(writer.total_bytes() == 3);
        }

        // Verify null bytes
        const dstd::String content_raw =
            dstd::fs::read_file(dstd::PathView(test_file))
                .value_or(dstd::String());
        dstd::Vector<unsigned char> content(content_raw.begin(),
                                            content_raw.end());
        CHECK(content.size() == 3);
        CHECK(content[0] == 0x00);
        CHECK(content[1] == 0x00);
        CHECK(content[2] == 0x00);

        dstd::fs::remove(test_file).discard();
    }
}

TEST_CASE("StreamingFileWriterUtility - Append Mode") {
    dstd::Path test_file = make_unique_test_path("test_append_writer.txt");

    // Write initial content
    {
        auto writer = dftu_utils_test::ok(
            StreamingFileWriterUtility::open(dstd::PathView(test_file.str())));
        dftu_utils_test::ok(
            writer.process(dstd::ByteView("Initial content\n")).get());
        writer.close();
    }

    // Append to file
    {
        auto writer = dftu_utils_test::ok(
            StreamingFileWriterUtility::open(dstd::PathView(test_file.str()),
                                             true));  // append=true
        dftu_utils_test::ok(
            writer.process(dstd::ByteView("Appended content\n")).get());
        writer.close();
    }

    // Verify both contents present
    dstd::String content =
        dstd::fs::read_file(dstd::PathView(test_file)).value_or(dstd::String());
    CHECK(content == "Initial content\nAppended content\n");

    dstd::fs::remove(test_file).discard();
}

TEST_CASE("StreamingFileWriterUtility - Directory Creation") {
    dstd::Path test_root = make_unique_test_path("test_nested");
    dstd::Path test_dir = test_root / "dir" / "structure";
    dstd::Path test_file = test_dir / "file.txt";

    SUBCASE("Auto-create directories") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str()), false,
                true));  // create_dirs=true
            dftu_utils_test::ok(
                writer.process(dstd::ByteView("Content")).get());
            writer.close();
        }

        CHECK(dstd::fs::exists(test_file).value());
        CHECK(dstd::fs::exists(test_dir).value());

        dstd::fs::remove_all(test_root).discard();
    }

    SUBCASE("Don't create directories") {
        auto opened = StreamingFileWriterUtility::open(
            dstd::PathView(test_file.str()), false,
            false);  // create_dirs=false
        REQUIRE(!opened);
        CHECK(opened.error().is_code(
            dftracer::utils::common::ErrorCode::NotFound));
    }
}

TEST_CASE("StreamingFileWriterUtility - Error Handling") {
    SUBCASE("Write to closed file") {
        dstd::Path test_file = make_unique_test_path("test_closed_writer.txt");
        auto writer = dftu_utils_test::ok(
            StreamingFileWriterUtility::open(dstd::PathView(test_file.str())));
        writer.close();

        auto written = writer.process(dstd::ByteView("Data")).get();
        REQUIRE(!written);
        CHECK(written.error().is_code(dftracer::utils::common::ErrorCode::Io));

        dstd::fs::remove(test_file).discard();
    }

    SUBCASE("Invalid path") {
        // Try to write to invalid location (assuming /invalid/path doesn't
        // exist)
        auto opened = StreamingFileWriterUtility::open(
            dstd::PathView("/invalid/nonexistent/path/file.txt"), false, false);
        REQUIRE(!opened);
        CHECK(opened.error().is_code(
            dftracer::utils::common::ErrorCode::NotFound));
    }
}

TEST_CASE("StreamingFileWriterUtility - Large Files") {
    dstd::Path test_file = make_unique_test_path("test_large_writer.dat");

    SUBCASE("Write 1MB in chunks") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str())));

            // Write 1MB in 1KB chunks
            dstd::Vector<unsigned char> chunk(1024, 0xAA);
            for (int i = 0; i < 1024; ++i) {
                dftu_utils_test::ok(
                    writer.process(dstd::ByteView(chunk.data(), chunk.size()))
                        .get());
            }
            writer.close();

            CHECK(writer.total_bytes() == 1024 * 1024);
            CHECK(writer.total_chunks() == 1024);
        }

        // Verify file size
        CHECK(dstd::fs::file_size(test_file).value() == 1024 * 1024);

        dstd::fs::remove(test_file).has_value();
    }

    SUBCASE("Write many small chunks") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str())));

            // Write 10000 small chunks
            for (int i = 0; i < 10000; ++i) {
                dftu_utils_test::ok(writer.process(dstd::ByteView("x")).get());
            }
            writer.close();

            CHECK(writer.total_bytes() == 10000);
            CHECK(writer.total_chunks() == 10000);
        }

        dstd::fs::remove(test_file).has_value();
    }
}

TEST_CASE("StreamingFileWriterUtility - Round Trip") {
    dstd::Path test_file = make_unique_test_path("test_round_trip.txt");

    SUBCASE("Write and read back") {
        dstd::String original_data;
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str())));
            for (int i = 0; i < 100; ++i) {
                dstd::String line = dstd::fmt<"Line {}\n">(i);
                dftu_utils_test::ok(writer.process(dstd::ByteView(line)).get());
                original_data += line;
            }
            writer.close();
        }

        // Read back
        dstd::String read_data = dstd::fs::read_file(dstd::PathView(test_file))
                                     .value_or(dstd::String());

        CHECK(read_data == original_data);
        CHECK(read_data.size() == original_data.size());

        dstd::fs::remove(test_file).has_value();
    }
}

TEST_CASE("StreamingFileWriterUtility - Automatic Closure") {
    dstd::Path test_file = make_unique_test_path("test_auto_close.txt");

    SUBCASE("Destructor closes file") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str())));
            dftu_utils_test::ok(writer.process(dstd::ByteView("Data")).get());
            // No explicit close() - destructor should handle it
        }

        // File should exist and be readable
        CHECK(dstd::fs::exists(test_file).value());

        dstd::String content = dstd::fs::read_file(dstd::PathView(test_file))
                                   .value_or(dstd::String());
        CHECK(content == "Data");

        dstd::fs::remove(test_file).has_value();
    }
}

TEST_CASE("StreamingFileWriterUtility - Different Data Types") {
    dstd::Path test_file = make_unique_test_path("test_data_types.txt");

    SUBCASE("Write text data") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str())));
            dftu_utils_test::ok(
                writer.process(dstd::ByteView("Text line 1\n")).get());
            dftu_utils_test::ok(
                writer.process(dstd::ByteView("Text line 2\n")).get());
            writer.close();
        }

        dstd::String content = dstd::fs::read_file(dstd::PathView(test_file))
                                   .value_or(dstd::String());
        CHECK(content.find("Text line 1") != dstd::String::npos);
        CHECK(content.find("Text line 2") != dstd::String::npos);

        dstd::fs::remove(test_file).has_value();
    }

    SUBCASE("Write binary patterns") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str())));

            // Write alternating pattern
            for (int i = 0; i < 100; ++i) {
                unsigned char byte = (i % 2 == 0) ? 0xAA : 0x55;
                dstd::Vector<unsigned char> data = {byte};
                dftu_utils_test::ok(
                    writer.process(dstd::ByteView(data.data(), data.size()))
                        .get());
            }
            writer.close();
        }

        // Verify pattern
        const dstd::String content_raw =
            dstd::fs::read_file(dstd::PathView(test_file))
                .value_or(dstd::String());
        dstd::Vector<unsigned char> content(content_raw.begin(),
                                            content_raw.end());
        CHECK(content.size() == 100);
        CHECK(content[0] == 0xAA);
        CHECK(content[1] == 0x55);

        dstd::fs::remove(test_file).has_value();
    }
}

TEST_CASE("StreamingFileWriterUtility - Truncate vs Append") {
    dstd::Path test_file = make_unique_test_path("test_truncate_append.txt");

    // Write initial data
    {
        auto writer = dftu_utils_test::ok(
            StreamingFileWriterUtility::open(dstd::PathView(test_file.str())));
        dftu_utils_test::ok(
            writer.process(dstd::ByteView("Initial data\n")).get());
        writer.close();
    }

    SUBCASE("Truncate mode overwrites") {
        {
            auto writer = dftu_utils_test::ok(StreamingFileWriterUtility::open(
                dstd::PathView(test_file.str()),
                false));  // append=false (truncate)
            dftu_utils_test::ok(
                writer.process(dstd::ByteView("New data\n")).get());
            writer.close();
        }

        dstd::String content = dstd::fs::read_file(dstd::PathView(test_file))
                                   .value_or(dstd::String());
        CHECK(content == "New data\n");
        CHECK(content.find("Initial") == dstd::String::npos);
    }

    dstd::fs::remove(test_file).has_value();
}

TEST_CASE("StreamingFileWriterUtility - Path Information") {
    dstd::Path test_file = make_unique_test_path("test_path_info.txt");

    {
        auto writer = dftu_utils_test::ok(
            StreamingFileWriterUtility::open(dstd::PathView(test_file.str())));
        CHECK(writer.path().str() == test_file.str());
        CHECK_FALSE(writer.is_closed());

        dftu_utils_test::ok(writer.process(dstd::ByteView("Data")).get());
        writer.close();

        CHECK(writer.is_closed());
        CHECK(writer.path().str() == test_file.str());
    }

    dstd::fs::remove(test_file).has_value();
}
