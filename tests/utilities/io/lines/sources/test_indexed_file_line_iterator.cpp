#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/indexer/indexer_factory.h>
#include <dftracer/utils/utilities/io/lines/sources/indexed_file_line_iterator.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::io::lines::sources;
using namespace dftracer::utils::utilities::io::lines;
using namespace dftracer::utils;
using namespace dft_utils_test;

TEST_SUITE("IndexedFileLineIterator") {
    TEST_CASE("IndexedFileLineIterator - Basic Operations with Line Range") {
        SUBCASE("Read entire indexed file") {
            // Create a test environment with 10 lines
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            // Create index
            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // Create iterator config
            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(1, 10);

            IndexedFileLineIterator iter(config);

            std::vector<std::string> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 10);
            CHECK(lines[0] == "{\"id\": 1, \"message\": \"Test message 1\"}");
            CHECK(lines[9] == "{\"id\": 10, \"message\": \"Test message 10\"}");
        }

        SUBCASE("Read partial line range") {
            TestEnvironment env(20);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(5, 10);

            IndexedFileLineIterator iter(config);

            std::vector<std::string> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 6);
            CHECK(lines[0] == "{\"id\": 5, \"message\": \"Test message 5\"}");
            CHECK(lines[5] == "{\"id\": 10, \"message\": \"Test message 10\"}");
        }

        SUBCASE("Read single line") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(7, 7);

            IndexedFileLineIterator iter(config);

            CHECK(iter.has_next());
            Line line = iter.next();
            CHECK(std::string(line.content) ==
                  "{\"id\": 7, \"message\": \"Test message 7\"}");
            CHECK(line.line_number == 7);

            CHECK_FALSE(iter.has_next());
        }

        SUBCASE("Auto-detect end line") {
            TestEnvironment env(15);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // When end_line is 0, it should auto-detect the file's total lines
            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(10, 0);

            IndexedFileLineIterator iter(config);

            std::vector<std::string> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 6);  // Lines 10-15
            CHECK(lines[0] == "{\"id\": 10, \"message\": \"Test message 10\"}");
            CHECK(lines[5] == "{\"id\": 15, \"message\": \"Test message 15\"}");
        }
    }

    TEST_CASE("IndexedFileLineIterator - Configuration Builder") {
        SUBCASE("Configuration with reader") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // Create reader first
            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Configure with existing reader
            auto config = IndexedFileLineIteratorConfig()
                              .with_reader(reader)
                              .with_line_range(2, 4)
                              .with_buffer_size(4096);

            CHECK(config.reader() == reader);
            CHECK(config.range_type() == RangeType::LINE_RANGE);
            CHECK(config.start() == 2);
            CHECK(config.end() == 4);
            CHECK(config.buffer_size() == 4096);

            IndexedFileLineIterator iter(config);

            int count = 0;
            while (iter.has_next()) {
                iter.next();
                count++;
            }

            CHECK(count == 3);  // Lines 2, 3, 4
        }

        SUBCASE("Configuration with byte range") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_byte_range(0, 200)  // Read first 200 bytes
                              .with_buffer_size(8192);

            CHECK(config.range_type() == RangeType::BYTE_RANGE);
            CHECK(config.start() == 0);
            CHECK(config.end() == 200);

            IndexedFileLineIterator iter(config);

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
            CHECK(config.range_type() == RangeType::LINE_RANGE);
            CHECK(config.start() == 1);
            CHECK(config.end() == 0);
            CHECK(config.buffer_size() == 1024 * 1024);  // 1MB default
        }
    }

    TEST_CASE("IndexedFileLineIterator - STL Iterator Interface") {
        SUBCASE("Range-based for loop") {
            TestEnvironment env(8);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(3, 6);

            IndexedFileLineIterator iter(config);

            std::vector<std::string> lines;
            for (const auto& line : iter) {
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 4);
            CHECK(lines[0] == "{\"id\": 3, \"message\": \"Test message 3\"}");
            CHECK(lines[3] == "{\"id\": 6, \"message\": \"Test message 6\"}");
        }

        SUBCASE("Iterator comparison") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig().with_file(
                gz_path, gz_path + ".idx");

            IndexedFileLineIterator iter(config);

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
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(3, 7);

            IndexedFileLineIterator iter(config);

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
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(5, 15);

            IndexedFileLineIterator iter(config);

            CHECK(iter.total_count() == 11);  // Lines 5-15 inclusive
            CHECK(iter.range_type() == RangeType::LINE_RANGE);
        }
    }

    TEST_CASE("IndexedFileLineIterator - Error Handling") {
        SUBCASE("Invalid line range") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // Start line < 1
            auto config1 = IndexedFileLineIteratorConfig()
                               .with_file(gz_path, gz_path + ".idx")
                               .with_line_range(0, 5);

            CHECK_THROWS_AS((IndexedFileLineIterator{config1}),
                            std::invalid_argument);

            // End < Start
            auto config2 = IndexedFileLineIteratorConfig()
                               .with_file(gz_path, gz_path + ".idx")
                               .with_line_range(10, 5);

            CHECK_THROWS_AS((IndexedFileLineIterator{config2}),
                            std::invalid_argument);
        }

        SUBCASE("Invalid byte range") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // End < Start for byte range
            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_byte_range(1000, 500);

            CHECK_THROWS_AS((IndexedFileLineIterator{config}),
                            std::invalid_argument);
        }

        SUBCASE("Null reader") {
            auto config =
                IndexedFileLineIteratorConfig().with_line_range(1, 10);

            CHECK_THROWS_AS((IndexedFileLineIterator{config}),
                            std::invalid_argument);
        }

        SUBCASE("Calling next() when no more lines") {
            TestEnvironment env(3);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(1, 2);

            IndexedFileLineIterator iter(config);

            iter.next();
            iter.next();
            CHECK_FALSE(iter.has_next());

            CHECK_THROWS_AS(iter.next(), std::runtime_error);
        }

        SUBCASE("Non-existent file") {
            // ReaderFactory::create should throw when file doesn't exist
            bool exception_thrown = false;
            try {
                auto config = IndexedFileLineIteratorConfig().with_file(
                    "non_existent.gz", "non_existent.gz.idx");
                IndexedFileLineIterator iter(config);
            } catch (const std::exception& e) {
                exception_thrown = true;
                // Check that the exception message contains expected text
                std::string msg = e.what();
                CHECK(msg.find("does not exist") != std::string::npos);
            }
            CHECK(exception_thrown);
        }
    }

    TEST_CASE("IndexedFileLineIterator - Large Files") {
        SUBCASE("Large line range") {
            TestEnvironment env(1000);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 4096, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(100, 200)
                              .with_buffer_size(16384);

            IndexedFileLineIterator iter(config);

            std::vector<std::string> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 101);  // Lines 100-200 inclusive
            CHECK(lines[0] ==
                  "{\"id\": 100, \"message\": \"Test message 100\"}");
            CHECK(lines[100] ==
                  "{\"id\": 200, \"message\": \"Test message 200\"}");
        }

        SUBCASE("Different buffer sizes") {
            TestEnvironment env(50);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // Small buffer
            {
                auto config = IndexedFileLineIteratorConfig()
                                  .with_file(gz_path, gz_path + ".idx")
                                  .with_line_range(1, 10)
                                  .with_buffer_size(256);

                IndexedFileLineIterator iter(config);

                int count = 0;
                while (iter.has_next()) {
                    iter.next();
                    count++;
                }
                CHECK(count == 10);
            }

            // Large buffer
            {
                auto config = IndexedFileLineIteratorConfig()
                                  .with_file(gz_path, gz_path + ".idx")
                                  .with_line_range(1, 10)
                                  .with_buffer_size(1024 * 1024);  // 1MB

                IndexedFileLineIterator iter(config);

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
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig().with_file(
                gz_path, gz_path + ".idx");

            IndexedFileLineIterator iter(config);

            // Get first line
            CHECK(iter.has_next());
            Line line1 = iter.next();
            std::string line1_copy(line1.content);  // Must copy immediately

            // Get second line
            CHECK(iter.has_next());
            Line line2 = iter.next();
            std::string line2_copy(line2.content);

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

    //         auto indexer = IndexerFactory::create(tar_gz_path, "", 1024,
    //         true); REQUIRE(indexer != nullptr); indexer->build();

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
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            auto config = IndexedFileLineIteratorConfig().with_reader(reader);

            IndexedFileLineIterator iter(config);

            CHECK(iter.get_reader() == reader);
            CHECK(iter.get_reader() != nullptr);
        }
    }
}