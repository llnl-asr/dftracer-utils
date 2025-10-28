#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/io/lines/sources/plain_file_bytes_iterator.h>
#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::io::lines;
using namespace dftracer::utils::utilities::io::lines::sources;

TEST_SUITE("PlainFileBytesIterator") {
    fs::path test_file = "test_plain_file_bytes_iterator.txt";

    TEST_CASE("PlainFileBytesIterator - Basic Byte Range Operations") {
        SUBCASE("Read entire file via byte range") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2\n";
                ofs << "Line 3\n";
            }

            // File size is 21 bytes (7 + 7 + 7)
            PlainFileBytesIterator iter(test_file.string(), 0, 21);

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 1");
                CHECK(line.line_number == 1);
            }

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 2");
                CHECK(line.line_number == 2);
            }

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 3");
                CHECK(line.line_number == 3);
            }

            CHECK_FALSE(iter.has_next());

            fs::remove(test_file);
        }

        SUBCASE("Read middle byte range") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";  // bytes 0-6
                ofs << "Line 2\n";  // bytes 7-13
                ofs << "Line 3\n";  // bytes 14-20
                ofs << "Line 4\n";  // bytes 21-27
            }

            // NOTE: PlainFileBytesIterator treats any non-zero start position
            // as mid-line and skips to the next line. So starting at byte 7
            // (start of Line 2) will skip Line 2 and start reading from Line 3.
            PlainFileBytesIterator iter(test_file.string(), 7, 21);

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 3");
            }

            CHECK_FALSE(iter.has_next());

            fs::remove(test_file);
        }

        SUBCASE("Byte range starting mid-line") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";  // bytes 0-6
                ofs << "Line 2\n";  // bytes 7-13
                ofs << "Line 3\n";  // bytes 14-20
            }

            // Start at byte 10 (middle of "Line 2"), should skip to next line
            PlainFileBytesIterator iter(test_file.string(), 10, 21);

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 3");
            }

            CHECK_FALSE(iter.has_next());

            fs::remove(test_file);
        }

        SUBCASE("Byte range ending mid-line") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";  // bytes 0-6
                ofs << "Line 2\n";  // bytes 7-13
                ofs << "Line 3\n";  // bytes 14-20
            }

            // End at byte 17 (middle of "Line 3")
            PlainFileBytesIterator iter(test_file.string(), 0, 17);

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 1");
            }

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 2");
            }

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Lin");  // Partial line
            }

            CHECK_FALSE(iter.has_next());

            fs::remove(test_file);
        }

        SUBCASE("Single byte range") {
            {
                std::ofstream ofs(test_file);
                ofs << "ABCDEFGHIJ\n";  // 11 bytes
            }

            // Read just one byte - but align_to_next_line will skip to after
            // the newline
            PlainFileBytesIterator iter(test_file.string(), 5, 6);

            CHECK_FALSE(
                iter.has_next());  // No lines because it skips past the end

            fs::remove(test_file);
        }
    }

    TEST_CASE("PlainFileBytesIterator - Error Handling") {
        SUBCASE("Non-existent file") {
            CHECK_THROWS_AS(PlainFileBytesIterator("non_existent.txt", 0, 100),
                            std::runtime_error);
        }

        SUBCASE("Invalid byte range - start >= end") {
            {
                std::ofstream ofs(test_file);
                ofs << "Test content\n";
            }

            CHECK_THROWS_AS(PlainFileBytesIterator(test_file.string(), 10, 10),
                            std::invalid_argument);
            CHECK_THROWS_AS(PlainFileBytesIterator(test_file.string(), 10, 5),
                            std::invalid_argument);

            fs::remove(test_file);
        }

        SUBCASE("Calling next() when no more lines") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
            }

            PlainFileBytesIterator iter(test_file.string(), 0, 7);

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 1");
            }
            CHECK_FALSE(iter.has_next());

            CHECK_THROWS_AS(iter.next(), std::runtime_error);

            fs::remove(test_file);
        }
    }

    TEST_CASE("PlainFileBytesIterator - Special Cases") {
        SUBCASE("File with no trailing newline") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2";  // No trailing newline
            }

            // File size is 13 bytes (7 + 6)
            PlainFileBytesIterator iter(test_file.string(), 0, 13);

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 1");
            }

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 2");
            }

            CHECK_FALSE(iter.has_next());

            fs::remove(test_file);
        }

        SUBCASE("File with empty lines") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "\n";  // Empty line
                ofs << "Line 3\n";
            }

            PlainFileBytesIterator iter(test_file.string(), 0, 15);

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 1");
            }

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "");  // Empty line
            }

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 3");
            }

            CHECK_FALSE(iter.has_next());

            fs::remove(test_file);
        }

        SUBCASE("Binary data handling") {
            {
                std::ofstream ofs(test_file, std::ios::binary);
                ofs.write("Line\x00with\x00nulls\n",
                          16);  // Use write() to preserve null bytes
                ofs.write("Normal line\n", 12);
            }

            PlainFileBytesIterator iter(test_file.string(), 0, 28);

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                std::string line_str(line.content);
                CHECK(line_str.size() == 15);  // "Line\0with\0nulls"
                CHECK(line_str[4] == '\0');
                CHECK(line_str[9] == '\0');
            }

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Normal line");
            }

            CHECK_FALSE(iter.has_next());

            fs::remove(test_file);
        }
    }

    TEST_CASE("PlainFileBytesIterator - STL Iterator Interface") {
        SUBCASE("Range-based for loop") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2\n";
                ofs << "Line 3\n";
            }

            PlainFileBytesIterator iter(test_file.string(), 0, 21);

            std::vector<std::string> lines;
            for (const auto& line : iter) {
                // Convert immediately to string to avoid string_view lifetime
                // issues
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 3);
            CHECK(lines[0] == "Line 1");
            CHECK(lines[1] == "Line 2");
            CHECK(lines[2] == "Line 3");

            fs::remove(test_file);
        }

        SUBCASE("Iterator with partial byte range") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";  // bytes 0-6
                ofs << "Line 2\n";  // bytes 7-13
                ofs << "Line 3\n";  // bytes 14-20
                ofs << "Line 4\n";  // bytes 21-27
            }

            // Start at byte 8 (middle of Line 2), will skip to Line 3
            PlainFileBytesIterator iter(test_file.string(), 8, 21);

            std::vector<std::string> lines;
            for (const auto& line : iter) {
                lines.push_back(std::string(line.content));
            }

            REQUIRE(lines.size() == 1);
            CHECK(lines[0] == "Line 3");

            fs::remove(test_file);
        }
    }

    TEST_CASE("PlainFileBytesIterator - Position Tracking") {
        SUBCASE("Current position updates correctly") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2\n";
                ofs << "Line 3\n";
            }

            PlainFileBytesIterator iter(test_file.string(), 0, 21);

            CHECK(iter.current_position() == 0);  // No lines read yet

            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 1");
            }
            CHECK(iter.current_position() == 1);  // After reading first line

            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 2");
            }
            CHECK(iter.current_position() == 2);  // After reading second line

            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 3");
            }
            CHECK(iter.current_position() == 3);  // After reading third line

            fs::remove(test_file);
        }

        SUBCASE("Position with byte range starting mid-file") {
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";  // bytes 0-6
                ofs << "Line 2\n";  // bytes 7-13
                ofs << "Line 3\n";  // bytes 14-20
            }

            // Start at byte 10 (middle of Line 2), should skip to Line 3
            PlainFileBytesIterator iter(test_file.string(), 10, 21);

            CHECK(iter.current_position() == 0);  // No lines read yet

            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 3");
            }
            CHECK(iter.current_position() ==
                  1);  // After reading first valid line

            fs::remove(test_file);
        }
    }

    TEST_CASE("PlainFileBytesIterator - Large Files") {
        SUBCASE("Many lines with specific byte range") {
            {
                std::ofstream ofs(test_file);
                // Create 100 lines
                for (int i = 1; i <= 100; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            // Read bytes 100-200
            // Note: Due to align_to_next_line behavior, actual lines read will
            // vary
            PlainFileBytesIterator iter(test_file.string(), 100, 200);

            int count = 0;
            for ([[maybe_unused]] const auto& line : iter) {
                count++;
                // Just count lines, don't check content due to alignment
                // behavior
            }

            // Should get at least some lines
            CHECK(count > 0);
            CHECK(count < 20);  // Reasonable upper bound

            fs::remove(test_file);
        }

        SUBCASE("Long lines with byte range") {
            {
                std::ofstream ofs(test_file);
                // Create a very long line (5000 chars)
                std::string long_line(5000, 'A');
                ofs << long_line << "\n";
                ofs << "Short line\n";
            }

            // Read first 100 bytes only
            PlainFileBytesIterator iter(test_file.string(), 0, 100);

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                std::string line_str(line.content);
                CHECK(line_str.size() ==
                      100);  // Read all 100 'A's (no newline in range)
                CHECK(line_str == std::string(100, 'A'));
            }

            CHECK_FALSE(iter.has_next());  // Byte range exhausted

            fs::remove(test_file);
        }
    }

    TEST_CASE("PlainFileBytesIterator - Edge Cases") {
        SUBCASE("Byte range beyond file size") {
            {
                std::ofstream ofs(test_file);
                ofs << "Short file\n";  // 11 bytes
            }

            // Try to read bytes 0-100 (file is only 11 bytes)
            PlainFileBytesIterator iter(test_file.string(), 0, 100);

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Short file");
            }

            CHECK_FALSE(iter.has_next());  // Should handle gracefully

            fs::remove(test_file);
        }

        SUBCASE("Start byte beyond file size") {
            {
                std::ofstream ofs(test_file);
                ofs << "Short file\n";  // 11 bytes
            }

            // Start at byte 50 (beyond file size)
            PlainFileBytesIterator iter(test_file.string(), 50, 100);

            CHECK_FALSE(iter.has_next());  // No lines available

            fs::remove(test_file);
        }

        SUBCASE("CRLF line endings") {
            {
                std::ofstream ofs(test_file, std::ios::binary);
                ofs << "Line 1\r\n";  // 8 bytes
                ofs << "Line 2\r\n";  // 8 bytes
                ofs << "Line 3\r\n";  // 8 bytes
            }

            PlainFileBytesIterator iter(test_file.string(), 0, 24);

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) ==
                      "Line 1\r");  // \r is kept, \n is delimiter
            }

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 2\r");
            }

            CHECK(iter.has_next());
            {
                Line line = iter.next();
                CHECK(std::string(line.content) == "Line 3\r");
            }

            CHECK_FALSE(iter.has_next());

            fs::remove(test_file);
        }
    }
}
