#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/text/line_splitter.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::text;
using namespace dftracer::utils::utilities::io::lines;

TEST_CASE("LineSplitterUtility - Basic functionality") {
    auto splitter = std::make_shared<LineSplitterUtility>();

    SUBCASE("Split simple multi-line text") {
        Text input{"Line 1\nLine 2\nLine 3"};
        Lines output = splitter->process(input);

        REQUIRE(output.lines.size() == 3);
        CHECK(output.lines[0].content == "Line 1");
        CHECK(output.lines[0].line_number == 1);
        CHECK(output.lines[1].content == "Line 2");
        CHECK(output.lines[1].line_number == 2);
        CHECK(output.lines[2].content == "Line 3");
        CHECK(output.lines[2].line_number == 3);
    }

    SUBCASE("Split text with empty lines") {
        Text input{"Line 1\n\nLine 3\n\nLine 5"};
        Lines output = splitter->process(input);

        REQUIRE(output.lines.size() == 5);
        CHECK(output.lines[0].content == "Line 1");
        CHECK(output.lines[1].content == "");
        CHECK(output.lines[1].line_number == 2);
        CHECK(output.lines[2].content == "Line 3");
        CHECK(output.lines[3].content == "");
        CHECK(output.lines[3].line_number == 4);
        CHECK(output.lines[4].content == "Line 5");
    }

    SUBCASE("Split single line") {
        Text input{"Single line without newline"};
        Lines output = splitter->process(input);

        REQUIRE(output.lines.size() == 1);
        CHECK(output.lines[0].content == "Single line without newline");
        CHECK(output.lines[0].line_number == 1);
    }

    SUBCASE("Split empty text") {
        Text input{""};
        Lines output = splitter->process(input);

        CHECK(output.lines.empty());
    }

    SUBCASE("Split text ending with newline") {
        Text input{"Line 1\nLine 2\n"};
        Lines output = splitter->process(input);

        // std::getline doesn't include the last empty line after trailing \n
        REQUIRE(output.lines.size() == 2);
        CHECK(output.lines[0].content == "Line 1");
        CHECK(output.lines[1].content == "Line 2");
    }

    SUBCASE("Split text with Windows line endings (\\r\\n)") {
        Text input{"Line 1\r\nLine 2\r\nLine 3"};
        Lines output = splitter->process(input);

        REQUIRE(output.lines.size() == 3);
        // \r will be included in the content since we split on \n
        CHECK(output.lines[0].content == "Line 1\r");
        CHECK(output.lines[1].content == "Line 2\r");
        CHECK(output.lines[2].content == "Line 3");
    }

    SUBCASE("Split text with only newlines") {
        Text input{"\n\n\n"};
        Lines output = splitter->process(input);

        REQUIRE(output.lines.size() == 3);
        for (const auto& line : output.lines) {
            CHECK(line.content == "");
        }
    }

    SUBCASE("Split long multi-line text") {
        std::string long_text;
        for (int i = 1; i <= 100; ++i) {
            long_text += "Line " + std::to_string(i);
            if (i < 100) {
                long_text += "\n";
            }
        }

        Text input{long_text};
        Lines output = splitter->process(input);

        REQUIRE(output.lines.size() == 100);
        for (int i = 0; i < 100; ++i) {
            CHECK(output.lines[i].content == "Line " + std::to_string(i + 1));
            CHECK(output.lines[i].line_number ==
                  static_cast<std::size_t>(i + 1));
        }
    }
}

TEST_CASE("LineSplitterUtility - Line numbering") {
    auto splitter = std::make_shared<LineSplitterUtility>();

    SUBCASE("Verify 1-indexed line numbers") {
        Text input{"First\nSecond\nThird"};
        Lines output = splitter->process(input);

        CHECK(output.lines[0].line_number == 1);
        CHECK(output.lines[1].line_number == 2);
        CHECK(output.lines[2].line_number == 3);
    }

    SUBCASE("Line numbers are sequential") {
        Text input{"A\nB\nC\nD\nE"};
        Lines output = splitter->process(input);

        for (std::size_t i = 0; i < output.lines.size(); ++i) {
            CHECK(output.lines[i].line_number == i + 1);
        }
    }
}

TEST_CASE("LineSplitterUtility - Edge cases") {
    auto splitter = std::make_shared<LineSplitterUtility>();

    SUBCASE("Very long single line") {
        std::string very_long_line(10000, 'x');
        Text input{very_long_line};
        Lines output = splitter->process(input);

        REQUIRE(output.lines.size() == 1);
        CHECK(output.lines[0].content == very_long_line);
        CHECK(output.lines[0].line_number == 1);
    }

    SUBCASE("Text with special characters") {
        Text input{
            "Line with\ttab\nLine with special: @#$%\nLine with émoji 🎉"};
        Lines output = splitter->process(input);

        REQUIRE(output.lines.size() == 3);
        CHECK(output.lines[0].content == "Line with\ttab");
        CHECK(output.lines[1].content == "Line with special: @#$%");
        CHECK(output.lines[2].content == "Line with émoji 🎉");
    }
}
