#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/indexer/indexer_factory.h>
#include <dftracer/utils/utilities/composites/line_batch_processor.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <chrono>
#include <fstream>
#include <regex>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::io::lines;
using namespace dft_utils_test;

// Test data structure for processed lines
struct ProcessedLine {
    std::size_t line_number;
    std::string content;
    std::size_t length;

    ProcessedLine(std::size_t num, const std::string& c)
        : line_number(num), content(c), length(c.length()) {}
};

TEST_SUITE("LineBatchProcessor") {
    TEST_CASE("LineBatchProcessor - Basic Line Processing") {
        SUBCASE("Process all lines from plain file") {
            TestEnvironment env(10);
            std::string txt_path = env.get_dir() + "/test.txt";

            // Create plain text file
            std::ofstream ofs(txt_path);
            for (int i = 1; i <= 10; ++i) {
                ofs << "Line " << i << ": This is test content" << std::endl;
            }
            ofs.close();

            // Create processor that captures all lines
            auto processor =
                [](const Line& line) -> std::optional<ProcessedLine> {
                return ProcessedLine(line.line_number,
                                     std::string(line.content));
            };

            LineBatchProcessorUtility<ProcessedLine> batch(processor);

            LineReadInput input;
            input.file_path = txt_path;

            auto results = batch.process(input);

            CHECK(results.size() == 10);
            CHECK(results[0].line_number == 1);
            CHECK(results[0].content.find("Line 1:") != std::string::npos);
            CHECK(results[9].line_number == 10);
            CHECK(results[9].content.find("Line 10:") != std::string::npos);

            // Clean up
            fs::remove(txt_path);
        }

        SUBCASE("Process lines from compressed file") {
            TestEnvironment env(15);
            std::string gz_path = env.create_test_gzip_file();
            std::string idx_path = gz_path + ".idx";

            // Create index
            auto indexer =
                IndexerFactory::create(gz_path, idx_path, 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // Create processor
            auto processor =
                [](const Line& line) -> std::optional<ProcessedLine> {
                return ProcessedLine(line.line_number,
                                     std::string(line.content));
            };

            LineBatchProcessorUtility<ProcessedLine> batch(processor);

            LineReadInput input;
            input.file_path = gz_path;
            input.idx_path = idx_path;

            auto results = batch.process(input);

            CHECK(results.size() == 15);
            CHECK(results[0].line_number == 1);
            CHECK(results[14].line_number == 15);
        }
    }

    TEST_CASE("LineBatchProcessor - Filtered Processing") {
        SUBCASE("Filter lines with optional return") {
            TestEnvironment env(20);
            std::string txt_path = env.get_dir() + "/test.txt";

            // Create test file
            std::ofstream ofs(txt_path);
            for (int i = 1; i <= 20; ++i) {
                if (i % 2 == 0) {
                    ofs << "EVEN: Line " << i << std::endl;
                } else {
                    ofs << "ODD: Line " << i << std::endl;
                }
            }
            ofs.close();

            // Create processor that only returns even lines
            auto processor =
                [](const Line& line) -> std::optional<ProcessedLine> {
                std::string content(line.content);
                if (content.find("EVEN:") != std::string::npos) {
                    return ProcessedLine(line.line_number, content);
                }
                return std::nullopt;  // Skip odd lines
            };

            LineBatchProcessorUtility<ProcessedLine> batch(processor);

            LineReadInput input;
            input.file_path = txt_path;

            auto results = batch.process(input);

            CHECK(results.size() == 10);  // Only even lines
            for (const auto& result : results) {
                CHECK(result.content.find("EVEN:") != std::string::npos);
            }

            // Clean up
            fs::remove(txt_path);
        }

        SUBCASE("Extract JSON lines") {
            TestEnvironment env(10);
            std::string txt_path = env.get_dir() + "/mixed.txt";

            // Create file with mixed content
            std::ofstream ofs(txt_path);
            ofs << "# Comment line" << std::endl;
            ofs << R"({"id": 1, "value": "test"})" << std::endl;
            ofs << "Regular text line" << std::endl;
            ofs << R"({"id": 2, "value": "data"})" << std::endl;
            ofs << "Another text line" << std::endl;
            ofs << R"({"id": 3, "value": "info"})" << std::endl;
            ofs.close();

            // Processor that only extracts JSON lines
            auto processor =
                [](const Line& line) -> std::optional<std::string> {
                std::string content(line.content);
                if (content.find("{") != std::string::npos &&
                    content.find("}") != std::string::npos) {
                    return content;
                }
                return std::nullopt;
            };

            LineBatchProcessorUtility<std::string> batch(processor);

            LineReadInput input;
            input.file_path = txt_path;

            auto results = batch.process(input);

            CHECK(results.size() == 3);  // Only JSON lines
            CHECK(results[0].find(R"("id": 1)") != std::string::npos);
            CHECK(results[1].find(R"("id": 2)") != std::string::npos);
            CHECK(results[2].find(R"("id": 3)") != std::string::npos);

            // Clean up
            fs::remove(txt_path);
        }
    }

    TEST_CASE("LineBatchProcessor - Line Range Processing") {
        SUBCASE("Process specific line range from plain file") {
            TestEnvironment env(20);
            std::string txt_path = env.get_dir() + "/test.txt";

            // Create test file
            std::ofstream ofs(txt_path);
            for (int i = 1; i <= 20; ++i) {
                ofs << "Line " << i << std::endl;
            }
            ofs.close();

            // Process lines 5-10 only
            auto processor =
                [](const Line& line) -> std::optional<std::string> {
                return std::string(line.content);
            };

            LineBatchProcessorUtility<std::string> batch(processor);

            LineReadInput input;
            input.file_path = txt_path;
            input.start_line = 5;
            input.end_line = 10;

            auto results = batch.process(input);

            CHECK(results.size() == 6);  // Lines 5-10 inclusive
            CHECK(results[0].find("Line 5") != std::string::npos);
            CHECK(results[5].find("Line 10") != std::string::npos);

            // Clean up
            fs::remove(txt_path);
        }

        SUBCASE("Process line range from compressed file") {
            TestEnvironment env(20);
            std::string gz_path = env.create_test_gzip_file();
            std::string idx_path = gz_path + ".idx";

            // Create index
            auto indexer =
                IndexerFactory::create(gz_path, idx_path, 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // Process lines 10-15
            auto processor =
                [](const Line& line) -> std::optional<ProcessedLine> {
                return ProcessedLine(line.line_number,
                                     std::string(line.content));
            };

            LineBatchProcessorUtility<ProcessedLine> batch(processor);

            LineReadInput input;
            input.file_path = gz_path;
            input.idx_path = idx_path;
            input.start_line = 10;
            input.end_line = 15;

            auto results = batch.process(input);

            CHECK(results.size() == 6);  // Lines 10-15
            CHECK(results[0].line_number == 10);
            CHECK(results[5].line_number == 15);
        }
    }

    TEST_CASE("LineBatchProcessor - Data Extraction") {
        SUBCASE("Extract numbers from lines") {
            TestEnvironment env(10);
            std::string txt_path = env.get_dir() + "/numbers.txt";

            // Create file with numbers
            std::ofstream ofs(txt_path);
            ofs << "Value: 42" << std::endl;
            ofs << "Count: 100" << std::endl;
            ofs << "No number here" << std::endl;
            ofs << "Score: 85.5" << std::endl;
            ofs << "Items: 3" << std::endl;
            ofs.close();

            // Extract numbers from lines
            auto processor = [](const Line& line) -> std::optional<double> {
                std::string content(line.content);
                std::regex number_regex(R"(:\s*(\d+(?:\.\d+)?))");
                std::smatch match;

                if (std::regex_search(content, match, number_regex)) {
                    return std::stod(match[1]);
                }
                return std::nullopt;
            };

            LineBatchProcessorUtility<double> batch(processor);

            LineReadInput input;
            input.file_path = txt_path;

            auto results = batch.process(input);

            CHECK(results.size() == 4);  // Lines with numbers
            CHECK(results[0] == 42.0);
            CHECK(results[1] == 100.0);
            CHECK(results[2] == 85.5);
            CHECK(results[3] == 3.0);

            // Clean up
            fs::remove(txt_path);
        }

        SUBCASE("Parse CSV-like data") {
            TestEnvironment env(5);
            std::string txt_path = env.get_dir() + "/data.csv";

            // Create CSV file
            std::ofstream ofs(txt_path);
            ofs << "name,age,city" << std::endl;
            ofs << "Alice,30,New York" << std::endl;
            ofs << "Bob,25,Los Angeles" << std::endl;
            ofs << "Charlie,35,Chicago" << std::endl;
            ofs << "David,28,Houston" << std::endl;
            ofs.close();

            struct Person {
                std::string name;
                int age;
                std::string city;
            };

            // Parse CSV lines (skip header)
            bool first_line = true;
            auto processor =
                [&first_line](const Line& line) -> std::optional<Person> {
                if (first_line) {
                    first_line = false;
                    return std::nullopt;  // Skip header
                }

                std::string content(line.content);
                std::stringstream ss(content);
                std::string name, age_str, city;

                if (std::getline(ss, name, ',') &&
                    std::getline(ss, age_str, ',') && std::getline(ss, city)) {
                    return Person{name, std::stoi(age_str), city};
                }
                return std::nullopt;
            };

            LineBatchProcessorUtility<Person> batch(processor);

            LineReadInput input;
            input.file_path = txt_path;

            auto results = batch.process(input);

            CHECK(results.size() == 4);  // Data rows only
            CHECK(results[0].name == "Alice");
            CHECK(results[0].age == 30);
            CHECK(results[1].name == "Bob");
            CHECK(results[1].age == 25);

            // Clean up
            fs::remove(txt_path);
        }
    }

    TEST_CASE("LineBatchProcessor - Empty and Edge Cases") {
        SUBCASE("Empty file") {
            TestEnvironment env(0);
            std::string txt_path = env.get_dir() + "/empty.txt";

            // Create empty file
            std::ofstream ofs(txt_path);
            ofs.close();

            auto processor =
                [](const Line& line) -> std::optional<std::string> {
                return std::string(line.content);
            };

            LineBatchProcessorUtility<std::string> batch(processor);

            LineReadInput input;
            input.file_path = txt_path;

            auto results = batch.process(input);

            CHECK(results.empty());

            // Clean up
            fs::remove(txt_path);
        }

        SUBCASE("All lines filtered out") {
            TestEnvironment env(10);
            std::string txt_path = env.get_dir() + "/test.txt";

            // Create test file
            std::ofstream ofs(txt_path);
            for (int i = 1; i <= 10; ++i) {
                ofs << "Line " << i << std::endl;
            }
            ofs.close();

            // Processor that filters out everything
            auto processor =
                [](const Line& /*line*/) -> std::optional<std::string> {
                return std::nullopt;  // Filter out all lines
            };

            LineBatchProcessorUtility<std::string> batch(processor);

            LineReadInput input;
            input.file_path = txt_path;

            auto results = batch.process(input);

            CHECK(results.empty());

            // Clean up
            fs::remove(txt_path);
        }

        SUBCASE("Single line file") {
            TestEnvironment env(1);
            std::string txt_path = env.get_dir() + "/single.txt";

            // Create single line file
            std::ofstream ofs(txt_path);
            ofs << "Single line content";
            ofs.close();

            auto processor =
                [](const Line& line) -> std::optional<std::string> {
                return std::string(line.content);
            };

            LineBatchProcessorUtility<std::string> batch(processor);

            LineReadInput input;
            input.file_path = txt_path;

            auto results = batch.process(input);

            CHECK(results.size() == 1);
            CHECK(results[0] == "Single line content");

            // Clean up
            fs::remove(txt_path);
        }
    }
}
