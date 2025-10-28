#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/text/line_filter.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::text;
using namespace dftracer::utils::utilities::io::lines;

TEST_CASE("LineFilterUtility - Basic functionality") {
    auto filter = std::make_shared<LineFilterUtility>();

    SUBCASE("Filter line that matches predicate") {
        auto predicate = [](const Line& line) {
            return line.content.find("ERROR") != std::string::npos;
        };

        FilterableLine input{Line{"ERROR: Something went wrong", 1}, predicate};
        auto result = filter->process(input);

        REQUIRE(result.has_value());
        CHECK(result->content == "ERROR: Something went wrong");
        CHECK(result->line_number == 1);
    }

    SUBCASE("Filter line that doesn't match predicate") {
        auto predicate = [](const Line& line) {
            return line.content.find("ERROR") != std::string::npos;
        };

        FilterableLine input{Line{"INFO: Everything is fine", 1}, predicate};
        auto result = filter->process(input);

        CHECK_FALSE(result.has_value());
    }

    SUBCASE("No predicate - pass through") {
        FilterableLine input{Line{"Any content", 1}, nullptr};
        auto result = filter->process(input);

        REQUIRE(result.has_value());
        CHECK(result->content == "Any content");
    }
}

TEST_CASE("LineFilterUtility - Different predicates") {
    auto filter = std::make_shared<LineFilterUtility>();

    SUBCASE("Filter by prefix") {
        auto has_prefix = [](const Line& line) {
            return line.content.rfind("ERROR", 0) == 0;  // starts with "ERROR"
        };

        FilterableLine match{Line{"ERROR: Bad thing", 1}, has_prefix};
        FilterableLine no_match{Line{"Warning: ERROR occurred", 2}, has_prefix};

        CHECK(filter->process(match).has_value());
        CHECK_FALSE(filter->process(no_match).has_value());
    }

    SUBCASE("Filter by line number") {
        auto is_even_line = [](const Line& line) {
            return line.line_number % 2 == 0;
        };

        FilterableLine odd{Line{"Content", 1}, is_even_line};
        FilterableLine even{Line{"Content", 2}, is_even_line};

        CHECK_FALSE(filter->process(odd).has_value());
        CHECK(filter->process(even).has_value());
    }

    SUBCASE("Filter by length") {
        auto is_long = [](const Line& line) {
            return line.content.length() > 10;
        };

        FilterableLine short_line{Line{"Short", 1}, is_long};
        FilterableLine long_line{Line{"This is a very long line", 2}, is_long};

        CHECK_FALSE(filter->process(short_line).has_value());
        CHECK(filter->process(long_line).has_value());
    }

    SUBCASE("Filter empty lines") {
        auto is_not_empty = [](const Line& line) {
            return !line.content.empty();
        };

        FilterableLine empty{Line{"", 1}, is_not_empty};
        FilterableLine not_empty{Line{"Content", 2}, is_not_empty};

        CHECK_FALSE(filter->process(empty).has_value());
        CHECK(filter->process(not_empty).has_value());
    }
}

TEST_CASE("LineFilterUtility - Complex filtering scenarios") {
    auto filter = std::make_shared<LineFilterUtility>();

    SUBCASE("Multiple conditions - AND") {
        auto error_and_critical = [](const Line& line) {
            return line.content.find("ERROR") != std::string::npos &&
                   line.content.find("CRITICAL") != std::string::npos;
        };

        FilterableLine both{Line{"ERROR: CRITICAL system failure", 1},
                            error_and_critical};
        FilterableLine only_error{Line{"ERROR: Minor issue", 2},
                                  error_and_critical};
        FilterableLine only_critical{Line{"CRITICAL warning", 3},
                                     error_and_critical};

        CHECK(filter->process(both).has_value());
        CHECK_FALSE(filter->process(only_error).has_value());
        CHECK_FALSE(filter->process(only_critical).has_value());
    }

    SUBCASE("Multiple conditions - OR") {
        auto error_or_warning = [](const Line& line) {
            return line.content.find("ERROR") != std::string::npos ||
                   line.content.find("WARNING") != std::string::npos;
        };

        FilterableLine error{Line{"ERROR occurred", 1}, error_or_warning};
        FilterableLine warning{Line{"WARNING: Check this", 2},
                               error_or_warning};
        FilterableLine info{Line{"INFO: All good", 3}, error_or_warning};

        CHECK(filter->process(error).has_value());
        CHECK(filter->process(warning).has_value());
        CHECK_FALSE(filter->process(info).has_value());
    }

    SUBCASE("Negation filter") {
        auto not_debug = [](const Line& line) {
            return line.content.find("DEBUG") == std::string::npos;
        };

        FilterableLine debug{Line{"DEBUG: Trace info", 1}, not_debug};
        FilterableLine info{Line{"INFO: Something", 2}, not_debug};

        CHECK_FALSE(filter->process(debug).has_value());
        CHECK(filter->process(info).has_value());
    }
}

TEST_CASE("LineFilterUtility - Edge cases") {
    auto filter = std::make_shared<LineFilterUtility>();

    SUBCASE("Empty line content") {
        auto always_true = [](const Line&) { return true; };
        auto always_false = [](const Line&) { return false; };

        FilterableLine empty_pass{Line{"", 1}, always_true};
        FilterableLine empty_fail{Line{"", 1}, always_false};

        CHECK(filter->process(empty_pass).has_value());
        CHECK_FALSE(filter->process(empty_fail).has_value());
    }

    SUBCASE("Very long line") {
        std::string very_long(10000, 'x');
        auto contains_y = [](const Line& line) {
            return line.content.find('y') != std::string::npos;
        };

        FilterableLine no_y{Line{very_long, 1}, contains_y};
        CHECK_FALSE(filter->process(no_y).has_value());

        std::string with_y = very_long + "y";
        FilterableLine has_y{Line{with_y, 1}, contains_y};
        CHECK(filter->process(has_y).has_value());
    }

    SUBCASE("Case-sensitive vs case-insensitive filtering") {
        auto case_sensitive = [](const Line& line) {
            return line.content.find("ERROR") != std::string::npos;
        };

        auto case_insensitive = [](const Line& line) {
            std::string lower{line.content};
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           ::tolower);
            return lower.find("error") != std::string::npos;
        };

        FilterableLine upper{Line{"ERROR happened", 1}, case_sensitive};
        FilterableLine lower{Line{"error happened", 1}, case_sensitive};
        FilterableLine mixed{Line{"Error happened", 1}, case_sensitive};

        // Case-sensitive
        CHECK(filter->process(upper).has_value());
        CHECK_FALSE(filter->process(lower).has_value());
        CHECK_FALSE(filter->process(mixed).has_value());

        // Case-insensitive
        FilterableLine upper_ci{Line{"ERROR happened", 1}, case_insensitive};
        FilterableLine lower_ci{Line{"error happened", 1}, case_insensitive};
        FilterableLine mixed_ci{Line{"Error happened", 1}, case_insensitive};

        CHECK(filter->process(upper_ci).has_value());
        CHECK(filter->process(lower_ci).has_value());
        CHECK(filter->process(mixed_ci).has_value());
    }
}

TEST_CASE("LineFilterUtility - Practical use cases") {
    auto filter = std::make_shared<LineFilterUtility>();

    SUBCASE("Log level filtering") {
        auto is_error_or_above = [](const Line& line) {
            return line.content.find("[ERROR]") != std::string::npos ||
                   line.content.find("[FATAL]") != std::string::npos;
        };

        std::vector<Line> logs = {Line{"[DEBUG] Starting process", 1},
                                  Line{"[INFO] Connection established", 2},
                                  Line{"[ERROR] Failed to read file", 3},
                                  Line{"[FATAL] System crash", 4},
                                  Line{"[INFO] Cleanup complete", 5}};

        std::vector<Line> filtered;
        for (const auto& log : logs) {
            auto result =
                filter->process(FilterableLine{log, is_error_or_above});
            if (result.has_value()) {
                filtered.push_back(*result);
            }
        }

        REQUIRE(filtered.size() == 2);
        CHECK(filtered[0].line_number == 3);
        CHECK(filtered[1].line_number == 4);
    }

    SUBCASE("Comment line filtering") {
        auto not_comment = [](const Line& line) {
            std::string trimmed{line.content};
            // Remove leading whitespace
            auto first_non_ws = trimmed.find_first_not_of(" \t");
            if (first_non_ws == std::string::npos) {
                // Empty or whitespace-only line - not a comment
                return true;
            }
            trimmed.erase(0, first_non_ws);
            return trimmed[0] != '#';
        };

        std::vector<Line> lines = {
            Line{"# This is a comment", 1}, Line{"actual code", 2},
            Line{"  # Indented comment", 3}, Line{"more code", 4},
            Line{"", 5}  // Empty line
        };

        std::vector<Line> filtered;
        for (const auto& line : lines) {
            auto result = filter->process(FilterableLine{line, not_comment});
            if (result.has_value()) {
                filtered.push_back(*result);
            }
        }

        REQUIRE(filtered.size() ==
                3);  // "actual code", "more code", and empty line
        CHECK(filtered[0].content == "actual code");
        CHECK(filtered[1].content == "more code");
        CHECK(filtered[2].content == "");
    }
}
