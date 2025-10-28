#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/indexer/indexer_factory.h>
#include <dftracer/utils/reader/reader_factory.h>
#include <dftracer/utils/utilities/io/lines/sources/indexed_file_bytes_iterator.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::io::lines::sources;
using namespace dftracer::utils::utilities::io::lines;
using namespace dftracer::utils;
using namespace dft_utils_test;

TEST_SUITE("IndexedFileBytesIterator") {
    TEST_CASE("IndexedFileBytesIterator - Basic Byte Range Operations") {
        SUBCASE("Read entire file via byte range") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            // Create index
            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // Create reader
            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Get file size to determine byte range
            std::size_t file_size = reader->get_max_bytes();

            IndexedFileBytesIterator iter(reader, 0, file_size);

            std::vector<std::string> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 10);
            CHECK(lines[0] == "{\"id\": 1, \"message\": \"Test message 1\"}");
            CHECK(lines[9] == "{\"id\": 10, \"message\": \"Test message 10\"}");
        }

        SUBCASE("Read partial byte range") {
            TestEnvironment env(20);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Read first 300 bytes
            IndexedFileBytesIterator iter(reader, 0, 300);

            std::vector<std::string> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(std::string(line.content));
            }

            // Should get some lines (exact count depends on line lengths)
            CHECK(lines.size() > 0);
            CHECK(lines.size() <= 20);
            CHECK(lines[0] == "{\"id\": 1, \"message\": \"Test message 1\"}");
        }

        SUBCASE("Read middle byte range") {
            TestEnvironment env(20);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Read bytes 200-400 (should skip some initial lines)
            IndexedFileBytesIterator iter(reader, 200, 400);

            std::vector<std::string> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(std::string(line.content));
            }

            // Should get some lines in the middle range
            CHECK(lines.size() > 0);
        }

        SUBCASE("Very small byte range") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Read just 50 bytes
            IndexedFileBytesIterator iter(reader, 0, 50);

            CHECK(iter.has_next());
            Line line = iter.next();
            CHECK(std::string(line.content) ==
                  "{\"id\": 1, \"message\": \"Test message 1\"}");

            // May or may not have more lines depending on line length
        }
    }

    TEST_CASE("IndexedFileBytesIterator - Buffer Size Configuration") {
        SUBCASE("Small buffer size") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            // Use small buffer (256 bytes)
            IndexedFileBytesIterator iter(reader, 0, file_size, 256);

            int count = 0;
            while (iter.has_next()) {
                iter.next();
                count++;
            }

            CHECK(count == 10);
        }

        SUBCASE("Large buffer size") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            // Use large buffer (1MB)
            IndexedFileBytesIterator iter(reader, 0, file_size, 1024 * 1024);

            int count = 0;
            while (iter.has_next()) {
                iter.next();
                count++;
            }

            CHECK(count == 10);
        }
    }

    TEST_CASE("IndexedFileBytesIterator - STL Iterator Interface") {
        SUBCASE("Range-based for loop") {
            TestEnvironment env(8);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            IndexedFileBytesIterator iter(reader, 0, file_size);

            std::vector<std::string> lines;
            for (const auto& line : iter) {
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 8);
            CHECK(lines[0] == "{\"id\": 1, \"message\": \"Test message 1\"}");
            CHECK(lines[7] == "{\"id\": 8, \"message\": \"Test message 8\"}");
        }

        SUBCASE("Iterator comparison") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            IndexedFileBytesIterator iter(reader, 0, file_size);

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

        SUBCASE("Iterator with partial byte range") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Read first 200 bytes
            IndexedFileBytesIterator iter(reader, 0, 200);

            std::vector<std::string> lines;
            for (const auto& line : iter) {
                lines.push_back(std::string(line.content));
            }

            CHECK(lines.size() > 0);
            CHECK(lines[0] == "{\"id\": 1, \"message\": \"Test message 1\"}");
        }
    }

    TEST_CASE("IndexedFileBytesIterator - Position Tracking") {
        SUBCASE("Current position updates correctly") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            IndexedFileBytesIterator iter(reader, 0, file_size);

            CHECK(iter.current_position() == 1);

            iter.next();
            CHECK(iter.current_position() == 2);

            iter.next();
            CHECK(iter.current_position() == 3);

            // Skip to end
            while (iter.has_next()) {
                iter.next();
            }
            CHECK(iter.current_position() == 11);  // After reading 10 lines
        }

        SUBCASE("Position with mid-range start") {
            TestEnvironment env(20);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Start at byte 200
            IndexedFileBytesIterator iter(reader, 200, 400);

            // Position should still start at 1 (relative to the iterator's
            // range)
            CHECK(iter.current_position() == 1);
        }
    }

    TEST_CASE("IndexedFileBytesIterator - Error Handling") {
        SUBCASE("Null reader") {
            CHECK_THROWS_AS(IndexedFileBytesIterator(nullptr, 0, 100),
                            std::invalid_argument);
        }

        SUBCASE("Invalid byte range - start >= end") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // start >= end
            CHECK_THROWS_AS(IndexedFileBytesIterator(reader, 100, 100),
                            std::invalid_argument);
            CHECK_THROWS_AS(IndexedFileBytesIterator(reader, 100, 50),
                            std::invalid_argument);
        }

        SUBCASE("Calling next() when no more lines") {
            TestEnvironment env(3);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Read small byte range that contains only 1-2 lines
            IndexedFileBytesIterator iter(reader, 0, 50);

            // Consume all available lines
            while (iter.has_next()) {
                iter.next();
            }

            CHECK_FALSE(iter.has_next());
            CHECK_THROWS_AS(iter.next(), std::runtime_error);
        }

        SUBCASE("Byte range beyond file size") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            // Try to read beyond file size (should handle gracefully)
            IndexedFileBytesIterator iter(reader, 0, file_size * 2);

            int count = 0;
            while (iter.has_next()) {
                iter.next();
                count++;
            }

            CHECK(count == 5);  // Should read all 5 lines
        }
    }

    TEST_CASE("IndexedFileBytesIterator - Large Files") {
        SUBCASE("Large file with specific byte range") {
            TestEnvironment env(1000);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 4096, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Read bytes 5000-10000
            IndexedFileBytesIterator iter(reader, 5000, 10000);

            int count = 0;
            while (iter.has_next()) {
                iter.next();
                count++;
            }

            // Should get some lines
            CHECK(count > 0);
            CHECK(count < 1000);
        }

        SUBCASE("Multiple small byte ranges") {
            TestEnvironment env(100);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Test multiple small ranges
            std::vector<std::pair<std::size_t, std::size_t>> ranges = {
                {0, 500}, {500, 1000}, {1000, 1500}};

            for (const auto& [start, end] : ranges) {
                IndexedFileBytesIterator iter(reader, start, end);

                int count = 0;
                while (iter.has_next()) {
                    iter.next();
                    count++;
                }

                // Each range should have some lines
                CHECK(count > 0);
            }
        }
    }

    TEST_CASE("IndexedFileBytesIterator - String View Lifetime") {
        SUBCASE("String view validity") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            IndexedFileBytesIterator iter(reader, 0, file_size);

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

    TEST_CASE("IndexedFileBytesIterator - Reader Access") {
        SUBCASE("Get underlying reader") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            IndexedFileBytesIterator iter(reader, 0, file_size);

            CHECK(iter.get_reader() == reader);
            CHECK(iter.get_reader() != nullptr);
        }
    }

    TEST_CASE("IndexedFileBytesIterator - Byte Range Edge Cases") {
        SUBCASE("Single byte range") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Read just one byte
            IndexedFileBytesIterator iter(reader, 10, 11);

            // Should handle gracefully (may or may not have content)
            if (iter.has_next()) {
                Line line = iter.next();
                CHECK(line.content.length() <= 1);
            }
        }

        SUBCASE("Byte range ending mid-line") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Read first 100 bytes (will likely cut mid-line)
            IndexedFileBytesIterator iter(reader, 0, 100);

            std::vector<std::string> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(std::string(line.content));
            }

            // Should get at least one line
            CHECK(lines.size() > 0);
        }

        SUBCASE("Byte range starting mid-line") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            // Start at byte 50 (likely mid-line)
            IndexedFileBytesIterator iter(reader, 50, file_size);

            std::vector<std::string> lines;
            while (iter.has_next()) {
                Line line = iter.next();
                lines.push_back(std::string(line.content));
            }

            // Should skip partial first line and read complete lines after
            CHECK(lines.size() > 0);
        }
    }

    TEST_CASE(
        "IndexedFileBytesIterator - Comparison with Different Buffer "
        "Sizes") {
        SUBCASE("Same results with different buffer sizes") {
            TestEnvironment env(20);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader1 = ReaderFactory::create(gz_path, gz_path + ".idx");
            auto reader2 = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader1 != nullptr);
            REQUIRE(reader2 != nullptr);

            std::size_t file_size = reader1->get_max_bytes();

            // Read with small buffer
            IndexedFileBytesIterator iter1(reader1, 0, file_size, 256);
            std::vector<std::string> lines1;
            while (iter1.has_next()) {
                lines1.push_back(std::string(iter1.next().content));
            }

            // Read with large buffer
            IndexedFileBytesIterator iter2(reader2, 0, file_size, 1024 * 1024);
            std::vector<std::string> lines2;
            while (iter2.has_next()) {
                lines2.push_back(std::string(iter2.next().content));
            }

            // Should get same results
            REQUIRE(lines1.size() == lines2.size());
            for (std::size_t i = 0; i < lines1.size(); ++i) {
                CHECK(lines1[i] == lines2[i]);
            }
        }
    }
}
