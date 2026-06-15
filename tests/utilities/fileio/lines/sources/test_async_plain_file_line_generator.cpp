#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_line_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/plain_file_line_iterator.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::fileio::lines::sources;
using namespace dftracer::utils::utilities::fileio::lines;
using namespace dftracer::utils::coro;
using namespace dft_utils_test;

// Helper: consume an AsyncGenerator<Line> into a vector of strings
static CoroTask<std::vector<std::string>> collect_lines(
    AsyncGenerator<Line> gen) {
    std::vector<std::string> result;
    while (auto line = co_await gen.next()) {
        result.push_back(std::string(line->content));
    }
    co_return result;
}

// Helper: consume an AsyncGenerator<Line> into a vector of LineCopy
struct LineCopy {
    std::string content;
    std::size_t line_number;
};

static CoroTask<std::vector<LineCopy>> collect_line_copies(
    AsyncGenerator<Line> gen) {
    std::vector<LineCopy> result;
    while (auto line = co_await gen.next()) {
        result.push_back(
            LineCopy{std::string(line->content), line->line_number});
    }
    co_return result;
}

TEST_SUITE("AsyncPlainFileLineGenerator") {
    TEST_CASE("Basic Operations") {
        SUBCASE("Read simple file") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_simple.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2\n";
                ofs << "Line 3\n";
            }

            auto gen = async_plain_file_lines(test_file.string());
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 3);
            CHECK(lines[0].content == "Line 1");
            CHECK(lines[0].line_number == 1);
            CHECK(lines[1].content == "Line 2");
            CHECK(lines[1].line_number == 2);
            CHECK(lines[2].content == "Line 3");
            CHECK(lines[2].line_number == 3);

            fs::remove(test_file);
        }

        SUBCASE("Read empty file") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_empty.txt");
            {
                std::ofstream ofs(test_file);
            }

            auto gen = async_plain_file_lines(test_file.string());
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.empty());

            fs::remove(test_file);
        }

        SUBCASE("Read single line") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_single.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "Single line\n";
            }

            auto gen = async_plain_file_lines(test_file.string());
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 1);
            CHECK(lines[0].content == "Single line");
            CHECK(lines[0].line_number == 1);

            fs::remove(test_file);
        }
    }

    TEST_CASE("Line Range Filtering") {
        fs::path test_file =
            make_unique_test_path("test_async_plain_range.txt");
        {
            std::ofstream ofs(test_file);
            for (int i = 1; i <= 10; ++i) {
                ofs << "Line " << i << "\n";
            }
        }

        SUBCASE("Read lines 3-5") {
            auto gen = async_plain_file_lines(test_file.string(), 3, 5);
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 3);
            CHECK(lines[0].content == "Line 3");
            CHECK(lines[0].line_number == 3);
            CHECK(lines[1].content == "Line 4");
            CHECK(lines[1].line_number == 4);
            CHECK(lines[2].content == "Line 5");
            CHECK(lines[2].line_number == 5);
        }

        SUBCASE("Read lines 1-2") {
            auto gen = async_plain_file_lines(test_file.string(), 1, 2);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 2);
            CHECK(lines[0] == "Line 1");
            CHECK(lines[1] == "Line 2");
        }

        SUBCASE("Read lines 9-10") {
            auto gen = async_plain_file_lines(test_file.string(), 9, 10);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 2);
            CHECK(lines[0] == "Line 9");
            CHECK(lines[1] == "Line 10");
        }

        SUBCASE("Read single line range") {
            auto gen = async_plain_file_lines(test_file.string(), 5, 5);
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 1);
            CHECK(lines[0].content == "Line 5");
            CHECK(lines[0].line_number == 5);
        }

        SUBCASE("Read all lines (no range)") {
            auto gen = async_plain_file_lines(test_file.string());
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 10);
            CHECK(lines[0] == "Line 1");
            CHECK(lines[9] == "Line 10");
        }

        SUBCASE("Read with start_line only (end_line=0)") {
            auto gen = async_plain_file_lines(test_file.string(), 8, 0);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 3);  // Lines 8, 9, 10
            CHECK(lines[0] == "Line 8");
            CHECK(lines[2] == "Line 10");
        }

        SUBCASE("Out-of-range line numbers") {
            auto gen = async_plain_file_lines(test_file.string(), 20, 30);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.empty());
        }

        fs::remove(test_file);
    }

    TEST_CASE("Special Cases") {
        SUBCASE("File without trailing newline") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_no_newline.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2";  // No trailing newline
            }

            auto gen = async_plain_file_lines(test_file.string());
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 2);
            CHECK(lines[0].content == "Line 1");
            CHECK(lines[0].line_number == 1);
            CHECK(lines[1].content == "Line 2");
            CHECK(lines[1].line_number == 2);

            fs::remove(test_file);
        }

        SUBCASE("Empty lines") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_empty_lines.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "\n";
                ofs << "Line 3\n";
            }

            auto gen = async_plain_file_lines(test_file.string());
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 3);
            CHECK(lines[0].content == "Line 1");
            CHECK(lines[1].content == "");
            CHECK(lines[2].content == "Line 3");

            fs::remove(test_file);
        }

        SUBCASE("Long lines") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_long.txt");
            {
                std::ofstream ofs(test_file);
                std::string long_line(10000, 'x');
                ofs << long_line << "\n";
            }

            auto gen = async_plain_file_lines(test_file.string());
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 1);
            CHECK(lines[0].size() == 10000);

            fs::remove(test_file);
        }

        SUBCASE("Many lines") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_many.txt");
            const int n = static_cast<int>(valgrind_scale(1000, 10));
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= n; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            auto gen = async_plain_file_lines(test_file.string());
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == static_cast<std::size_t>(n));
            CHECK(lines[0] == "Line 1");
            CHECK(lines[n - 1] == "Line " + std::to_string(n));

            fs::remove(test_file);
        }
    }

    TEST_CASE("Error Handling") {
        SUBCASE("Non-existent file throws") {
            auto gen = async_plain_file_lines("non_existent_file_12345.txt");

            // Exception is thrown inside the coroutine body,
            // which executes on the first co_await gen.next()
            auto task = [](AsyncGenerator<Line> g) -> CoroTask<void> {
                (void)co_await g.next();
                co_return;
            }(std::move(gen));

            CHECK_THROWS_AS(task.get(), std::runtime_error);
        }
    }

    TEST_CASE("Line Number Tracking") {
        SUBCASE("Line numbers match expected positions") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_linenum.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 10; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            auto gen = async_plain_file_lines(test_file.string(), 3, 7);
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 5);
            CHECK(lines[0].line_number == 3);
            CHECK(lines[1].line_number == 4);
            CHECK(lines[2].line_number == 5);
            CHECK(lines[3].line_number == 6);
            CHECK(lines[4].line_number == 7);

            fs::remove(test_file);
        }
    }

    TEST_CASE("Generator Lifecycle") {
        SUBCASE("Generator done() after exhaustion") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_lifecycle.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2\n";
                ofs << "Line 3\n";
            }

            auto gen = async_plain_file_lines(test_file.string());

            auto task = [](AsyncGenerator<Line> g) -> CoroTask<void> {
                auto val1 = co_await g.next();
                CHECK(val1.has_value());

                auto val2 = co_await g.next();
                CHECK(val2.has_value());

                auto val3 = co_await g.next();
                CHECK(val3.has_value());

                // Should be done now
                auto val4 = co_await g.next();
                CHECK(!val4.has_value());

                CHECK(g.done());
                co_return;
            }(std::move(gen));

            task.get();

            fs::remove(test_file);
        }

        SUBCASE("Move semantics") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_move.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 5; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            auto gen1 = async_plain_file_lines(test_file.string());
            auto gen2 = std::move(gen1);  // Move construct

            auto task = collect_lines(std::move(gen2));
            auto lines = task.get();

            REQUIRE(lines.size() == 5);

            fs::remove(test_file);
        }
    }

    TEST_CASE("Consistency with Sync Iterator") {
        SUBCASE("Async and sync produce same results") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_consistency.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 20; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            // Sync path
            PlainFileLineIterator sync_iter(test_file.string());
            std::vector<std::string> sync_lines;
            while (sync_iter.has_next()) {
                Line line = sync_iter.next();
                sync_lines.push_back(std::string(line.content));
            }

            // Async path
            auto gen = async_plain_file_lines(test_file.string());
            auto task = collect_lines(std::move(gen));
            auto async_lines = task.get();

            REQUIRE(sync_lines.size() == async_lines.size());
            for (std::size_t i = 0; i < sync_lines.size(); ++i) {
                CHECK(sync_lines[i] == async_lines[i]);
            }

            fs::remove(test_file);
        }

        SUBCASE("Async and sync produce same results with range") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_consistency2.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 20; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            // Sync path with range
            PlainFileLineIterator sync_iter(test_file.string(), 5, 15);
            std::vector<std::string> sync_lines;
            while (sync_iter.has_next()) {
                Line line = sync_iter.next();
                sync_lines.push_back(std::string(line.content));
            }

            // Async path with range
            auto gen = async_plain_file_lines(test_file.string(), 5, 15);
            auto task = collect_lines(std::move(gen));
            auto async_lines = task.get();

            REQUIRE(sync_lines.size() == async_lines.size());
            for (std::size_t i = 0; i < sync_lines.size(); ++i) {
                CHECK(sync_lines[i] == async_lines[i]);
            }

            fs::remove(test_file);
        }
    }

    TEST_CASE("JSONL File") {
        SUBCASE("Read JSON lines") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_jsonl.txt");
            {
                std::ofstream ofs(test_file);
                ofs << R"({"id": 1, "name": "Alice"})" << "\n";
                ofs << R"({"id": 2, "name": "Bob"})" << "\n";
                ofs << R"({"id": 3, "name": "Charlie"})" << "\n";
            }

            auto gen = async_plain_file_lines(test_file.string());
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 3);
            CHECK(lines[0].find("\"id\": 1") != std::string::npos);
            CHECK(lines[1].find("\"id\": 2") != std::string::npos);
            CHECK(lines[2].find("\"id\": 3") != std::string::npos);

            fs::remove(test_file);
        }
    }
}
