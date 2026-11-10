// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/utilities/fileio/lines/sources/indexed_file_line_iterator.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <string>
#include <vector>

using namespace dftracer::utils::utilities::fileio::lines::sources;
using namespace dftracer::utils::utilities::fileio::lines;
using namespace dftracer::utils;
using namespace dftracer::utils::index::gzip;
using namespace dftracer::utils::utilities::reader::internal;
using namespace dftu_utils_test;

TEST_SUITE("IndexedFileLineIterator") {
    TEST_CASE("IndexedFileLineIterator - Basic Operations with Line Range" *
              doctest::test_suite("vg")) {
        SUBCASE("Read entire indexed file") {
            // Create a test environment with 10 lines
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();

            // Create index
            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // Create iterator config
            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                              .with_line_range(1, 10);

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            dstd::Vector<dstd::String> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(dstd::String(line.content));
            }

            REQUIRE(lines.size() == 10);
            CHECK(lines[0] == "{\"id\": 1, \"message\": \"Test message 1\"}");
            CHECK(lines[9] == "{\"id\": 10, \"message\": \"Test message 10\"}");
        }

        SUBCASE("Read partial line range") {
            TestEnvironment env(20);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                              .with_line_range(5, 10);

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            dstd::Vector<dstd::String> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(dstd::String(line.content));
            }

            REQUIRE(lines.size() == 6);
            CHECK(lines[0] == "{\"id\": 5, \"message\": \"Test message 5\"}");
            CHECK(lines[5] == "{\"id\": 10, \"message\": \"Test message 10\"}");
        }

        SUBCASE("Read single line") {
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                              .with_line_range(7, 7);

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            CHECK(iter.has_next());
            Line line = iter.next();
            CHECK(dstd::String(line.content) ==
                  "{\"id\": 7, \"message\": \"Test message 7\"}");
            CHECK(line.line_number == 7);

            CHECK_FALSE(iter.has_next());
        }

        SUBCASE("Auto-detect end line") {
            TestEnvironment env(15);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // When end_line is 0, it should auto-detect the file's total lines
            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                              .with_line_range(10, 0);

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            dstd::Vector<dstd::String> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(dstd::String(line.content));
            }

            REQUIRE(lines.size() == 6);  // Lines 10-15
            CHECK(lines[0] == "{\"id\": 10, \"message\": \"Test message 10\"}");
            CHECK(lines[5] == "{\"id\": 15, \"message\": \"Test message 15\"}");
        }
    }

    TEST_CASE("IndexedFileLineIterator - Configuration Builder") {
        SUBCASE("Configuration with reader") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // Create reader first
            auto reader = dftu_utils_test::ok(
                ReaderFactory::create(gz_path, dstd::fmt<"{}.idx">(gz_path)));
            REQUIRE(reader != nullptr);

            // Configure with existing reader
            auto config = IndexedFileLineIteratorConfig()
                              .with_reader(reader)
                              .with_line_range(2, 4)
                              .with_buffer_size(4096);

            CHECK(config.reader() == reader);
            CHECK(config.range_type() == RangeType::LineRange);
            CHECK(config.start() == 2);
            CHECK(config.end() == 4);
            CHECK(config.buffer_size() == 4096);

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            int count = 0;
            while (iter.has_next()) {
                iter.next();
                count++;
            }

            CHECK(count == 3);  // Lines 2, 3, 4
        }

        SUBCASE("Configuration with byte range") {
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                              .with_byte_range(0, 200)  // Read first 200 bytes
                              .with_buffer_size(8192);

            CHECK(config.range_type() == RangeType::ByteRange);
            CHECK(config.start() == 0);
            CHECK(config.end() == 200);

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            // Should read some lines within the byte range
            int count = 0;
            while (iter.has_next()) {
                iter.next();
                count++;
            }

            CHECK(count > 0);
        }

        SUBCASE("Default configuration values") {
            IndexedFileLineIteratorConfig config;

            CHECK(config.reader() == nullptr);
            CHECK(config.range_type() == RangeType::LineRange);
            CHECK(config.start() == 1);
            CHECK(config.end() == 0);
            CHECK(config.buffer_size() == 1024 * 1024);  // 1MB default
        }
    }

    TEST_CASE("IndexedFileLineIterator - STL Iterator Interface") {
        SUBCASE("Range-based for loop") {
            TestEnvironment env(8);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                              .with_line_range(3, 6);

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            dstd::Vector<dstd::String> lines;
            for (const auto& line : iter) {
                lines.push_back(dstd::String(line.content));
            }

            REQUIRE(lines.size() == 4);
            CHECK(lines[0] == "{\"id\": 3, \"message\": \"Test message 3\"}");
            CHECK(lines[3] == "{\"id\": 6, \"message\": \"Test message 6\"}");
        }

        SUBCASE("Iterator comparison") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig().with_file(
                gz_path, dstd::fmt<"{}.idx">(gz_path));

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            auto begin = iter.begin();
            auto end = iter.end();

            CHECK(begin != end);

            // Consume all lines
            while (iter.has_next()) {
                iter.next();
            }

            // After consuming all, begin should equal end
            auto new_begin = iter.begin();
            CHECK(new_begin == end);
        }
    }

    TEST_CASE("IndexedFileLineIterator - Position Tracking") {
        SUBCASE("Current position with line range") {
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                              .with_line_range(3, 7);

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            CHECK(iter.current_position() == 3);

            iter.next();
            CHECK(iter.current_position() == 4);

            iter.next();
            CHECK(iter.current_position() == 5);

            // Skip to end
            while (iter.has_next()) {
                iter.next();
            }
            CHECK(iter.current_position() == 8);  // After reading line 7
        }

        SUBCASE("Total count calculation") {
            TestEnvironment env(20);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                              .with_line_range(5, 15);

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            CHECK(iter.total_count() == 11);  // Lines 5-15 inclusive
            CHECK(iter.range_type() == RangeType::LineRange);
        }
    }

    TEST_CASE("IndexedFileLineIterator - Error Handling") {
        SUBCASE("Invalid line range") {
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // Start line < 1
            auto config1 = IndexedFileLineIteratorConfig()
                               .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                               .with_line_range(0, 5);

            dftu_utils_test::expect_error(
                IndexedFileLineIterator::open(config1),
                common::ErrorCode::InvalidArgument);

            // End < Start
            auto config2 = IndexedFileLineIteratorConfig()
                               .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                               .with_line_range(10, 5);

            dftu_utils_test::expect_error(
                IndexedFileLineIterator::open(config2),
                common::ErrorCode::InvalidArgument);
        }

        SUBCASE("Invalid byte range") {
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // End < Start for byte range
            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                              .with_byte_range(1000, 500);

            dftu_utils_test::expect_error(IndexedFileLineIterator::open(config),
                                          common::ErrorCode::InvalidArgument);
        }

        SUBCASE("Null reader") {
            auto config =
                IndexedFileLineIteratorConfig().with_line_range(1, 10);

            dftu_utils_test::expect_error(IndexedFileLineIterator::open(config),
                                          common::ErrorCode::InvalidArgument,
                                          "Reader cannot be null");
        }

        SUBCASE("Non-existent file") {
            // ReaderFactory::create fails when the file doesn't exist, and the
            // config keeps the error for open() to give.
            auto config = IndexedFileLineIteratorConfig().with_file(
                "non_existent.gz", "non_existent.gz.idx");
            REQUIRE(config.error().has_value());
            CHECK(dstd::StringView(config.error()->c_str())
                      .find("does not exist") != dstd::StringView::npos);

            auto opened = IndexedFileLineIterator::open(config);
            REQUIRE(!opened.has_value());
            CHECK(dstd::StringView(opened.error().c_str())
                      .find("does not exist") != dstd::StringView::npos);
        }
    }

    TEST_CASE("next() with no more lines ends the process" * doctest::skip()) {
        fresh_process::announce_abort();
        TestEnvironment env(3);
        dstd::String gz_path = env.create_test_gzip_file();
        auto indexer =
            CheckpointIndexerFactory::create(gz_path, "", 1024, true);
        indexer->build();
        auto config = IndexedFileLineIteratorConfig()
                          .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                          .with_line_range(1, 2);
        auto iter = dftu_utils_test::ok(IndexedFileLineIterator::open(config));
        iter.next();
        iter.next();
        iter.next();
    }

    TEST_CASE("next() with no more lines aborts with its message") {
        auto result =
            fresh_process::run("next() with no more lines ends the process");
        CHECK(result.aborted_with(
            "IndexedFileLineIterator::next: no more lines"));
    }

    TEST_CASE("IndexedFileLineIterator - Large Files" *
              doctest::test_suite("vg")) {
        SUBCASE("Large line range") {
            TestEnvironment env(valgrind_scale(1000, 4));
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 4096, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                              .with_line_range(100, 200)
                              .with_buffer_size(16384);

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            dstd::Vector<dstd::String> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(dstd::String(line.content));
            }

            REQUIRE(lines.size() == 101);  // Lines 100-200 inclusive
            CHECK(lines[0] ==
                  "{\"id\": 100, \"message\": \"Test message 100\"}");
            CHECK(lines[100] ==
                  "{\"id\": 200, \"message\": \"Test message 200\"}");
        }

        SUBCASE("Different buffer sizes") {
            TestEnvironment env(50);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // Small buffer
            {
                auto config =
                    IndexedFileLineIteratorConfig()
                        .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                        .with_line_range(1, 10)
                        .with_buffer_size(256);

                auto iter =
                    dftu_utils_test::ok(IndexedFileLineIterator::open(config));

                int count = 0;
                while (iter.has_next()) {
                    iter.next();
                    count++;
                }
                CHECK(count == 10);
            }

            // Large buffer
            {
                auto config =
                    IndexedFileLineIteratorConfig()
                        .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                        .with_line_range(1, 10)
                        .with_buffer_size(1024 * 1024);  // 1MB

                auto iter =
                    dftu_utils_test::ok(IndexedFileLineIterator::open(config));

                int count = 0;
                while (iter.has_next()) {
                    iter.next();
                    count++;
                }
                CHECK(count == 10);
            }
        }
    }

    TEST_CASE("IndexedFileLineIterator - String View Lifetime") {
        SUBCASE("String view validity") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig().with_file(
                gz_path, dstd::fmt<"{}.idx">(gz_path));

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            // Get first line
            CHECK(iter.has_next());
            Line line1 = iter.next();
            dstd::String line1_copy(line1.content);  // Must copy immediately

            // Get second line
            CHECK(iter.has_next());
            Line line2 = iter.next();
            dstd::String line2_copy(line2.content);

            // Verify the copies are different
            CHECK(line1_copy != line2_copy);
            CHECK(line1_copy == "{\"id\": 1, \"message\": \"Test message 1\"}");
            CHECK(line2_copy == "{\"id\": 2, \"message\": \"Test message 2\"}");
        }
    }

    // TODO: Enable when TarReader::stream() is implemented
    // TEST_CASE("IndexedFileLineIterator - TAR.GZ Files") {
    //     SUBCASE("Read from TAR.GZ archive") {
    //         TestEnvironment env(15, Format::TAR_GZIP);
    //         std::string tar_gz_path = env.create_test_tar_gzip_file();

    //         auto indexer = CheckpointIndexerFactory::create(tar_gz_path, "",
    //         1024, true); REQUIRE(indexer != nullptr); indexer->build();

    //         auto config = IndexedFileLineIteratorConfig()
    //             .with_file(tar_gz_path, tar_gz_path + ".idx")
    //             .with_line_range(5, 10);

    //         IndexedFileLineIterator iter(config);

    //         std::vector<std::string> lines;
    //         while (iter.has_next()) {
    //             Line line = iter.next();
    //             lines.push_back(std::string(line.content));
    //         }

    //         // TAR.GZ test files have different content structure
    //         // Check that we got the expected number of lines
    //         CHECK(lines.size() > 0);
    //     }
    // }

    TEST_CASE("IndexedFileLineIterator - Reader Access") {
        SUBCASE("Get underlying reader") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                CheckpointIndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = dftu_utils_test::ok(
                ReaderFactory::create(gz_path, dstd::fmt<"{}.idx">(gz_path)));
            REQUIRE(reader != nullptr);

            auto config = IndexedFileLineIteratorConfig().with_reader(reader);

            auto iter =
                dftu_utils_test::ok(IndexedFileLineIterator::open(config));

            CHECK(iter.reader() == reader);
            CHECK(iter.reader() != nullptr);
        }
    }
}