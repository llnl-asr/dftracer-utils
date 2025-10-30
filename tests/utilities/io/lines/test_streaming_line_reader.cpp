#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/io/lines/streaming_line_reader.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::io::lines;
using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer::internal;
using namespace dft_utils_test;

TEST_SUITE("StreamingLineReader") {
    fs::path test_file = "test_streaming_line_reader.txt";
    fs::path gz_file = "test_streaming_line_reader.gz";
    fs::path tar_gz_file = "test_archive.tar.gz";
    fs::path tgz_file = "test_archive.tgz";

    TEST_CASE("StreamingLineReader - Basic Plain File Reading") {
        SUBCASE("Read entire plain text file") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2\n";
                ofs << "Line 3\n";
            }

            auto config =
                StreamingLineReaderConfig().with_file(test_file.string());

            auto range = StreamingLineReader::read(config);

            std::vector<std::string> lines;
            while (range.has_next()) {
                Line line = range.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 3);
            CHECK(lines[0] == "Line 1");
            CHECK(lines[1] == "Line 2");
            CHECK(lines[2] == "Line 3");

            fs::remove(test_file);
        }

        SUBCASE("Read plain file with line range") {
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 10; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            auto config = StreamingLineReaderConfig()
                              .with_file(test_file.string())
                              .with_line_range(3, 7);  // Read lines 3-7

            auto range = StreamingLineReader::read(config);

            std::vector<std::string> lines;
            while (range.has_next()) {
                Line line = range.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 5);
            CHECK(lines[0] == "Line 3");
            CHECK(lines[2] == "Line 5");
            CHECK(lines[4] == "Line 7");

            fs::remove(test_file);
        }

        SUBCASE("Read empty file") {
            {
                std::ofstream ofs(test_file);  // Create empty file
            }

            auto config =
                StreamingLineReaderConfig().with_file(test_file.string());

            auto range = StreamingLineReader::read(config);

            CHECK_FALSE(range.has_next());

            fs::remove(test_file);
        }

        SUBCASE("Direct read_plain method") {
            {
                std::ofstream ofs(test_file);
                ofs << "Direct line 1\n";
                ofs << "Direct line 2\n";
            }

            auto range = StreamingLineReader::read_plain(test_file.string());

            CHECK(range.has_next());
            {
                Line line = range.next();
                CHECK(std::string(line.content) == "Direct line 1");
            }

            CHECK(range.has_next());
            {
                Line line = range.next();
                CHECK(std::string(line.content) == "Direct line 2");
            }

            CHECK_FALSE(range.has_next());

            fs::remove(test_file);
        }

        SUBCASE("Direct read_plain with line range") {
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 5; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            auto range =
                StreamingLineReader::read_plain(test_file.string(), 2, 4);

            std::vector<std::string> lines;
            while (range.has_next()) {
                Line line = range.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 3);
            CHECK(lines[0] == "Line 2");
            CHECK(lines[1] == "Line 3");
            CHECK(lines[2] == "Line 4");

            fs::remove(test_file);
        }
    }

    TEST_CASE("StreamingLineReader - Format Detection") {
        SUBCASE("Detect .gz extension without index") {
            // Create a file with .gz extension (not actually compressed)
            {
                std::ofstream ofs(gz_file);
                ofs << "Fake gz content\n";
            }

            // Ensure no index file exists
            std::string idx_path = gz_file.string() + ".idx";
            if (fs::exists(idx_path)) {
                fs::remove(idx_path);
            }

            auto config =
                StreamingLineReaderConfig().with_file(gz_file.string());

            // Without index, should fall back to plain file reading
            auto range = StreamingLineReader::read(config);

            CHECK(range.has_next());
            {
                Line line = range.next();
                CHECK(std::string(line.content) == "Fake gz content");
            }

            fs::remove(gz_file);
        }

        SUBCASE("Detect .tar.gz extension") {
            {
                std::ofstream ofs(tar_gz_file);
                ofs << "Fake tar.gz content\n";
            }

            auto config =
                StreamingLineReaderConfig().with_file(tar_gz_file.string());

            // Without index, should fall back to plain file reading
            auto range = StreamingLineReader::read(config);

            CHECK(range.has_next());
            {
                Line line = range.next();
                CHECK(std::string(line.content) == "Fake tar.gz content");
            }

            fs::remove(tar_gz_file);
        }

        SUBCASE("Detect .tgz extension") {
            {
                std::ofstream ofs(tgz_file);
                ofs << "Fake tgz content\n";
            }

            auto config =
                StreamingLineReaderConfig().with_file(tgz_file.string());

            auto range = StreamingLineReader::read(config);

            CHECK(range.has_next());
            {
                Line line = range.next();
                CHECK(std::string(line.content) == "Fake tgz content");
            }

            fs::remove(tgz_file);
        }

        SUBCASE("Auto-detect index file with real compressed file") {
            // Create a real compressed file using TestEnvironment
            TestEnvironment env(10);  // Create environment with 10 lines
            std::string gz_path = env.create_test_gzip_file();

            // Create index using IndexerFactory
            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();  // Actually build the index

            std::string idx_path = gz_path + ".idx";

            // Verify index file was created
            CHECK(fs::exists(idx_path));

            auto config = StreamingLineReaderConfig().with_file(gz_path);

            // Should use indexed reading since both gz and idx exist
            auto range = StreamingLineReader::read(config);

            // Read first few lines
            CHECK(range.has_next());
            {
                Line line = range.next();
                // TestEnvironment creates JSON lines
                CHECK(std::string(line.content) ==
                      "{\"id\": 1, \"message\": \"Test message 1\"}");
            }

            CHECK(range.has_next());
            {
                Line line = range.next();
                CHECK(std::string(line.content) ==
                      "{\"id\": 2, \"message\": \"Test message 2\"}");
            }

            // TestEnvironment destructor will clean up files
        }

        SUBCASE("Explicit index path with real compressed file") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            // Create index
            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            std::string idx_path = gz_path + ".idx";

            // Test with explicit index path
            auto config =
                StreamingLineReaderConfig().with_file(gz_path).with_index(
                    idx_path);

            CHECK(config.index_path() == idx_path);

            auto range = StreamingLineReader::read(config);

            CHECK(range.has_next());
            {
                Line line = range.next();
                CHECK(std::string(line.content) ==
                      "{\"id\": 1, \"message\": \"Test message 1\"}");
            }
        }
    }

    TEST_CASE("StreamingLineReader - Configuration API") {
        SUBCASE("Fluent configuration API") {
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 10; ++i) {
                    ofs << "Config line " << i << "\n";
                }
            }

            // Test fluent API chaining
            auto config = StreamingLineReaderConfig()
                              .with_file(test_file.string())
                              .with_line_range(5, 6);

            CHECK(config.file_path() == test_file.string());
            CHECK(config.start_line() == 5);
            CHECK(config.end_line() == 6);
            CHECK(config.index_path() == "");

            auto range = StreamingLineReader::read(config);

            std::vector<std::string> lines;
            while (range.has_next()) {
                Line line = range.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 2);
            CHECK(lines[0] == "Config line 5");
            CHECK(lines[1] == "Config line 6");

            fs::remove(test_file);
        }

        SUBCASE("Configuration with all parameters") {
            auto config = StreamingLineReaderConfig()
                              .with_file("test.gz")
                              .with_index("test.gz.idx")
                              .with_line_range(10, 20);

            CHECK(config.file_path() == "test.gz");
            CHECK(config.index_path() == "test.gz.idx");
            CHECK(config.start_line() == 10);
            CHECK(config.end_line() == 20);
        }

        SUBCASE("Default configuration values") {
            StreamingLineReaderConfig config;

            CHECK(config.file_path() == "");
            CHECK(config.index_path() == "");
            CHECK(config.start_line() == 0);
            CHECK(config.end_line() == 0);
        }
    }

    TEST_CASE("StreamingLineReader - Special Cases") {
        SUBCASE("File with no trailing newline") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2";  // No trailing newline
            }

            auto config =
                StreamingLineReaderConfig().with_file(test_file.string());

            auto range = StreamingLineReader::read(config);

            std::vector<std::string> lines;
            while (range.has_next()) {
                Line line = range.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 2);
            CHECK(lines[0] == "Line 1");
            CHECK(lines[1] == "Line 2");

            fs::remove(test_file);
        }

        SUBCASE("File with empty lines") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "\n";  // Empty line
                ofs << "Line 3\n";
                ofs << "\n";  // Empty line
                ofs << "Line 5\n";
            }

            auto config =
                StreamingLineReaderConfig().with_file(test_file.string());

            auto range = StreamingLineReader::read(config);

            std::vector<std::string> lines;
            while (range.has_next()) {
                Line line = range.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 5);
            CHECK(lines[0] == "Line 1");
            CHECK(lines[1] == "");
            CHECK(lines[2] == "Line 3");
            CHECK(lines[3] == "");
            CHECK(lines[4] == "Line 5");

            fs::remove(test_file);
        }

        SUBCASE("Very long lines") {
            {
                std::ofstream ofs(test_file);
                std::string long_line(10000, 'A');
                ofs << long_line << "\n";
                ofs << "Short line\n";
                std::string another_long(5000, 'B');
                ofs << another_long << "\n";
            }

            auto config =
                StreamingLineReaderConfig().with_file(test_file.string());

            auto range = StreamingLineReader::read(config);

            CHECK(range.has_next());
            {
                Line line = range.next();
                CHECK(line.content.size() == 10000);
                CHECK(line.content[0] == 'A');
                CHECK(line.content[9999] == 'A');
            }

            CHECK(range.has_next());
            {
                Line line = range.next();
                CHECK(std::string(line.content) == "Short line");
            }

            CHECK(range.has_next());
            {
                Line line = range.next();
                CHECK(line.content.size() == 5000);
                CHECK(line.content[0] == 'B');
            }

            CHECK_FALSE(range.has_next());

            fs::remove(test_file);
        }

        SUBCASE("Line range beyond file") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2\n";
                ofs << "Line 3\n";
            }

            auto config = StreamingLineReaderConfig()
                              .with_file(test_file.string())
                              .with_line_range(2, 10);  // End beyond file

            auto range = StreamingLineReader::read(config);

            std::vector<std::string> lines;
            while (range.has_next()) {
                Line line = range.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 2);  // Only lines 2-3 exist
            CHECK(lines[0] == "Line 2");
            CHECK(lines[1] == "Line 3");

            fs::remove(test_file);
        }

        SUBCASE("Line range starting beyond file") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2\n";
            }

            auto config = StreamingLineReaderConfig()
                              .with_file(test_file.string())
                              .with_line_range(10, 20);  // Start beyond file

            auto range = StreamingLineReader::read(config);

            CHECK_FALSE(range.has_next());  // No lines in range

            fs::remove(test_file);
        }
    }

    TEST_CASE("StreamingLineReader - Large Files") {
        SUBCASE("Many lines") {
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 1000; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            auto config = StreamingLineReaderConfig()
                              .with_file(test_file.string())
                              .with_line_range(500, 510);

            auto range = StreamingLineReader::read(config);

            std::vector<int> line_numbers;
            while (range.has_next()) {
                Line line = range.next();
                std::string line_str(line.content);
                // Extract number from "Line XXX"
                int num = std::stoi(line_str.substr(5));
                line_numbers.push_back(num);
            }

            REQUIRE(line_numbers.size() == 11);
            CHECK(line_numbers[0] == 500);
            CHECK(line_numbers[5] == 505);
            CHECK(line_numbers[10] == 510);

            fs::remove(test_file);
        }
    }

    TEST_CASE("StreamingLineReader - Real World Scenarios") {
        SUBCASE("CSV file processing") {
            {
                std::ofstream ofs(test_file);
                ofs << "Name,Age,City\n";
                ofs << "Alice,30,New York\n";
                ofs << "Bob,25,San Francisco\n";
                ofs << "Charlie,35,Chicago\n";
            }

            auto config = StreamingLineReaderConfig()
                              .with_file(test_file.string())
                              .with_line_range(2, 3);  // Skip header

            auto range = StreamingLineReader::read(config);

            std::vector<std::string> records;
            while (range.has_next()) {
                Line line = range.next();
                records.push_back(std::string(line.content));
            }

            REQUIRE(records.size() == 2);
            CHECK(records[0] == "Alice,30,New York");
            CHECK(records[1] == "Bob,25,San Francisco");

            fs::remove(test_file);
        }

        SUBCASE("Log file processing") {
            {
                std::ofstream ofs(test_file);
                ofs << "2024-01-01 INFO: Application started\n";
                ofs << "2024-01-01 DEBUG: Loading configuration\n";
                ofs << "2024-01-01 ERROR: Failed to connect\n";
                ofs << "2024-01-01 INFO: Retrying connection\n";
                ofs << "2024-01-01 INFO: Connected successfully\n";
            }

            auto config =
                StreamingLineReaderConfig().with_file(test_file.string());

            auto range = StreamingLineReader::read(config);

            int info_count = 0, debug_count = 0, error_count = 0;
            while (range.has_next()) {
                Line line = range.next();
                std::string line_str(line.content);
                if (line_str.find("INFO:") != std::string::npos) info_count++;
                if (line_str.find("DEBUG:") != std::string::npos) debug_count++;
                if (line_str.find("ERROR:") != std::string::npos) error_count++;
            }

            CHECK(info_count == 3);
            CHECK(debug_count == 1);
            CHECK(error_count == 1);

            fs::remove(test_file);
        }

        SUBCASE("JSONL file processing") {
            {
                std::ofstream ofs(test_file);
                ofs << R"({"id": 1, "name": "Item 1"})" << "\n";
                ofs << R"({"id": 2, "name": "Item 2"})" << "\n";
                ofs << R"({"id": 3, "name": "Item 3"})" << "\n";
            }

            auto config =
                StreamingLineReaderConfig().with_file(test_file.string());

            auto range = StreamingLineReader::read(config);

            int count = 0;
            while (range.has_next()) {
                Line line = range.next();
                std::string line_str(line.content);
                // Simple check for valid JSON line
                CHECK(line_str.front() == '{');
                CHECK(line_str.back() == '}');
                count++;
            }

            CHECK(count == 3);

            fs::remove(test_file);
        }
    }

    TEST_CASE("StreamingLineReader - Compressed Files with Index") {
        SUBCASE("Read compressed file with line range") {
            TestEnvironment env(20);
            std::string gz_path = env.create_test_gzip_file();

            // Create index
            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            std::string idx_path = gz_path + ".idx";

            auto config =
                StreamingLineReaderConfig().with_file(gz_path).with_line_range(
                    5, 10);

            auto range = StreamingLineReader::read(config);

            std::vector<std::string> lines;
            while (range.has_next()) {
                Line line = range.next();
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 6);
            CHECK(lines[0] ==
                  "{\"id\": 5, \"message\": \"Test message 5\"}");    // Line 5
            CHECK(lines[5] ==
                  "{\"id\": 10, \"message\": \"Test message 10\"}");  // Line 10
        }
    }
}