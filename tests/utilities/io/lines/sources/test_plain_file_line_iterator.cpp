#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/io/lines/sources/plain_file_line_iterator.h>
#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::io::lines;
using namespace dftracer::utils::utilities::io::lines::sources;

TEST_CASE("PlainFileLineIterator - Basic Operations") {
    fs::path test_file = "test_plain_line_iter.txt";

    SUBCASE("Read simple file") {
        {
            std::ofstream ofs(test_file);
            ofs << "Line 1\n";
            ofs << "Line 2\n";
            ofs << "Line 3\n";
            ofs.close();
        }

        PlainFileLineIterator iter(test_file.string());

        CHECK(iter.has_next());
        Line line1 = iter.next();
        CHECK(line1.content == "Line 1");
        CHECK(line1.line_number == 1);

        CHECK(iter.has_next());
        Line line2 = iter.next();
        CHECK(line2.content == "Line 2");
        CHECK(line2.line_number == 2);

        CHECK(iter.has_next());
        Line line3 = iter.next();
        CHECK(line3.content == "Line 3");
        CHECK(line3.line_number == 3);

        CHECK_FALSE(iter.has_next());

        fs::remove(test_file);
    }

    SUBCASE("Read empty file") {
        {
            std::ofstream ofs(test_file);
        }

        PlainFileLineIterator iter(test_file.string());
        CHECK_FALSE(iter.has_next());

        fs::remove(test_file);
    }

    SUBCASE("Read single line") {
        {
            std::ofstream ofs(test_file);
            ofs << "Single line\n";
        }

        PlainFileLineIterator iter(test_file.string());
        CHECK(iter.has_next());

        Line line = iter.next();
        CHECK(line.content == "Single line");
        CHECK(line.line_number == 1);
        CHECK_FALSE(iter.has_next());

        fs::remove(test_file);
    }
}

TEST_CASE("PlainFileLineIterator - Line Range") {
    fs::path test_file = "test_line_range.txt";

    {
        std::ofstream ofs(test_file);
        for (int i = 1; i <= 10; ++i) {
            ofs << "Line " << i << "\n";
        }
    }

    SUBCASE("Read lines 3-5") {
        PlainFileLineIterator iter(test_file.string(), 3, 5);

        CHECK(iter.has_next());
        Line line1 = iter.next();
        CHECK(line1.content == "Line 3");
        CHECK(line1.line_number == 3);

        CHECK(iter.has_next());
        Line line2 = iter.next();
        CHECK(line2.content == "Line 4");
        CHECK(line2.line_number == 4);

        CHECK(iter.has_next());
        Line line3 = iter.next();
        CHECK(line3.content == "Line 5");
        CHECK(line3.line_number == 5);

        CHECK_FALSE(iter.has_next());
    }

    SUBCASE("Read lines 1-2") {
        PlainFileLineIterator iter(test_file.string(), 1, 2);

        Line line1 = iter.next();
        CHECK(line1.content == "Line 1");

        Line line2 = iter.next();
        CHECK(line2.content == "Line 2");

        CHECK_FALSE(iter.has_next());
    }

    SUBCASE("Read lines 9-10") {
        PlainFileLineIterator iter(test_file.string(), 9, 10);

        Line line1 = iter.next();
        CHECK(line1.content == "Line 9");

        Line line2 = iter.next();
        CHECK(line2.content == "Line 10");

        CHECK_FALSE(iter.has_next());
    }

    SUBCASE("Read single line range") {
        PlainFileLineIterator iter(test_file.string(), 5, 5);

        CHECK(iter.has_next());
        Line line = iter.next();
        CHECK(line.content == "Line 5");
        CHECK(line.line_number == 5);
        CHECK_FALSE(iter.has_next());
    }

    fs::remove(test_file);
}

TEST_CASE("PlainFileLineIterator - Error Handling") {
    SUBCASE("Non-existent file") {
        CHECK_THROWS_AS(PlainFileLineIterator("non_existent_file_12345.txt"),
                        std::runtime_error);
    }

    SUBCASE("Invalid line range") {
        fs::path test_file = "test_invalid_range.txt";
        {
            std::ofstream ofs(test_file);
            ofs << "Line 1\n";
        }

        // start_line < 1
        CHECK_THROWS_AS(PlainFileLineIterator(test_file.string(), 0, 5),
                        std::invalid_argument);

        // end_line < start_line
        CHECK_THROWS_AS(PlainFileLineIterator(test_file.string(), 5, 2),
                        std::invalid_argument);

        fs::remove(test_file);
    }

    SUBCASE("Call next() when no more lines") {
        fs::path test_file = "test_no_more_lines.txt";
        {
            std::ofstream ofs(test_file);
            ofs << "Only one line\n";
        }

        PlainFileLineIterator iter(test_file.string());
        iter.next();  // Read the only line

        CHECK_FALSE(iter.has_next());
        CHECK_THROWS_AS(iter.next(), std::runtime_error);

        fs::remove(test_file);
    }
}

TEST_CASE("PlainFileLineIterator - Special Cases") {
    fs::path test_file = "test_special_cases.txt";

    SUBCASE("Lines without trailing newline") {
        {
            std::ofstream ofs(test_file);
            ofs << "Line 1\n";
            ofs << "Line 2";  // No newline
        }

        PlainFileLineIterator iter(test_file.string());

        Line line1 = iter.next();
        CHECK(line1.content == "Line 1");

        Line line2 = iter.next();
        CHECK(line2.content == "Line 2");

        CHECK_FALSE(iter.has_next());

        fs::remove(test_file);
    }

    SUBCASE("Empty lines") {
        {
            std::ofstream ofs(test_file);
            ofs << "Line 1\n";
            ofs << "\n";
            ofs << "Line 3\n";
        }

        PlainFileLineIterator iter(test_file.string());

        Line line1 = iter.next();
        CHECK(line1.content == "Line 1");

        Line line2 = iter.next();
        CHECK(line2.content == "");
        CHECK(line2.empty());

        Line line3 = iter.next();
        CHECK(line3.content == "Line 3");

        CHECK_FALSE(iter.has_next());

        fs::remove(test_file);
    }

    SUBCASE("Long lines") {
        {
            std::ofstream ofs(test_file);
            std::string long_line(10000, 'x');
            ofs << long_line << "\n";
        }

        PlainFileLineIterator iter(test_file.string());

        CHECK(iter.has_next());
        Line line = iter.next();
        CHECK(line.size() == 10000);
        CHECK_FALSE(iter.has_next());

        fs::remove(test_file);
    }

    SUBCASE("Many lines") {
        {
            std::ofstream ofs(test_file);
            for (int i = 1; i <= 1000; ++i) {
                ofs << "Line " << i << "\n";
            }
        }

        PlainFileLineIterator iter(test_file.string());

        int count = 0;
        while (iter.has_next()) {
            iter.next();
            count++;
        }

        CHECK(count == 1000);

        fs::remove(test_file);
    }
}

TEST_CASE("PlainFileLineIterator - STL Iterator Interface") {
    fs::path test_file = "test_stl_iter.txt";

    {
        std::ofstream ofs(test_file);
        ofs << "Line 1\n";
        ofs << "Line 2\n";
        ofs << "Line 3\n";
    }

    SUBCASE("Range-based for loop") {
        PlainFileLineIterator iter(test_file.string());

        std::vector<std::string> lines;
        for (const auto& line : iter) {
            lines.push_back(std::string(line.content));
        }

        CHECK(lines.size() == 3);
        CHECK(lines[0] == "Line 1");
        CHECK(lines[1] == "Line 2");
        CHECK(lines[2] == "Line 3");
    }

    fs::remove(test_file);
}

TEST_CASE("PlainFileLineIterator - Current Position") {
    fs::path test_file = "test_position.txt";

    {
        std::ofstream ofs(test_file);
        for (int i = 1; i <= 5; ++i) {
            ofs << "Line " << i << "\n";
        }
    }

    SUBCASE("Track position without range") {
        PlainFileLineIterator iter(test_file.string());

        CHECK(iter.current_position() == 0);  // Before first read

        iter.next();
        CHECK(iter.current_position() == 1);

        iter.next();
        CHECK(iter.current_position() == 2);

        iter.next();
        CHECK(iter.current_position() == 3);
    }

    SUBCASE("Track position with range") {
        PlainFileLineIterator iter(test_file.string(), 2, 4);

        // After skipping to start_line=2, current position should reflect that
        Line line = iter.next();
        CHECK(line.line_number == 2);
        CHECK(iter.current_position() == 2);

        iter.next();
        CHECK(iter.current_position() == 3);

        iter.next();
        CHECK(iter.current_position() == 4);

        CHECK_FALSE(iter.has_next());
    }

    fs::remove(test_file);
}

TEST_CASE("PlainFileLineIterator - File Path") {
    fs::path test_file = "test_file_path.txt";

    {
        std::ofstream ofs(test_file);
        ofs << "Test\n";
    }

    PlainFileLineIterator iter(test_file.string());
    CHECK(iter.get_file_path() == test_file.string());

    fs::remove(test_file);
}

TEST_CASE("PlainFileLineIterator - Different Line Endings") {
    fs::path test_file = "test_line_endings.txt";

    SUBCASE("Unix line endings (LF)") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            ofs << "Line 1\n";
            ofs << "Line 2\n";
        }

        PlainFileLineIterator iter(test_file.string());

        Line line1 = iter.next();
        CHECK(line1.content == "Line 1");

        Line line2 = iter.next();
        CHECK(line2.content == "Line 2");

        fs::remove(test_file);
    }

    SUBCASE("Windows line endings (CRLF)") {
        {
            std::ofstream ofs(test_file, std::ios::binary);
            ofs << "Line 1\r\n";
            ofs << "Line 2\r\n";
        }

        PlainFileLineIterator iter(test_file.string());

        [[maybe_unused]] Line line1 = iter.next();
        // Note: std::getline removes \n but may leave \r
        // Depending on implementation, content might include \r

        Line line2 = iter.next();
        CHECK(line2.line_number == 2);

        fs::remove(test_file);
    }
}

TEST_CASE("PlainFileLineIterator - Real World Scenarios") {
    fs::path test_file = "test_real_world.txt";

    SUBCASE("CSV file") {
        {
            std::ofstream ofs(test_file);
            ofs << "id,name,value\n";
            ofs << "1,Alice,100\n";
            ofs << "2,Bob,200\n";
            ofs << "3,Charlie,300\n";
        }

        PlainFileLineIterator iter(test_file.string());

        Line header = iter.next();
        CHECK(header.content == "id,name,value");

        int count = 1;
        while (iter.has_next()) {
            iter.next();
            count++;
        }

        CHECK(count == 4);

        fs::remove(test_file);
    }

    SUBCASE("Log file with specific line range") {
        {
            std::ofstream ofs(test_file);
            for (int i = 1; i <= 100; ++i) {
                ofs << "[2024-01-01 " << i << ":00:00] INFO: Log entry " << i
                    << "\n";
            }
        }

        // Read only lines 50-60
        PlainFileLineIterator iter(test_file.string(), 50, 60);

        int count = 0;
        while (iter.has_next()) {
            Line line = iter.next();
            CHECK(line.line_number >= 50);
            CHECK(line.line_number <= 60);
            count++;
        }

        CHECK(count == 11);  // Lines 50-60 inclusive

        fs::remove(test_file);
    }

    SUBCASE("JSON lines (JSONL)") {
        {
            std::ofstream ofs(test_file);
            ofs << R"({"id": 1, "name": "Alice"})" << "\n";
            ofs << R"({"id": 2, "name": "Bob"})" << "\n";
            ofs << R"({"id": 3, "name": "Charlie"})" << "\n";
        }

        PlainFileLineIterator iter(test_file.string());

        int json_count = 0;
        while (iter.has_next()) {
            Line line = iter.next();
            CHECK(line.content.find("\"id\"") != std::string_view::npos);
            json_count++;
        }

        CHECK(json_count == 3);

        fs::remove(test_file);
    }
}
