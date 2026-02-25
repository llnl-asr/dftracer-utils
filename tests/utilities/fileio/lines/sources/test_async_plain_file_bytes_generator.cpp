#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_bytes_generator.h>
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

// Sync byte-range reader matching PlainFileBytesIterator behavior exactly.
// Avoids including plain_file_bytes_iterator.h which causes namespace
// collision with async_plain_file_bytes_generator.h.
static std::vector<std::string> sync_read_bytes_range(const std::string& path,
                                                      std::size_t start,
                                                      std::size_t end) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Cannot open file: " + path);

    std::size_t pos = start;
    file.seekg(static_cast<std::streamoff>(start));

    // Align to next line boundary if starting mid-file
    if (start > 0) {
        char c;
        while (pos < end && file.get(c)) {
            pos++;
            if (c == '\n') break;
        }
    }

    std::vector<std::string> result;
    while (pos < end) {
        std::string line_buf;
        bool found_char = false;
        char c;
        while (pos < end && file.get(c)) {
            pos++;
            found_char = true;
            if (c == '\n') break;
            line_buf.push_back(c);
        }
        if (!found_char || (line_buf.empty() && pos >= end)) {
            break;
        }
        result.push_back(std::move(line_buf));
    }
    return result;
}

TEST_SUITE("AsyncPlainFileBytesGenerator") {
    TEST_CASE("Basic Byte Range Operations") {
        SUBCASE("Read entire file via byte range") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_full.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2\n";
                ofs << "Line 3\n";
            }

            // File size: "Line 1\nLine 2\nLine 3\n" = 21 bytes
            auto gen = async_plain_file_bytes(test_file.string(), 0, 100);
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

        SUBCASE("Read partial byte range from start") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_partial.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 10; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            // Read first 20 bytes — should get "Line 1\n" (7) + "Line 2\n"
            // (7) + partial "Line 3" up to byte 20
            auto gen = async_plain_file_bytes(test_file.string(), 0, 20);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.size() > 0);
            CHECK(lines[0] == "Line 1");

            fs::remove(test_file);
        }

        SUBCASE("Read middle byte range") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_middle.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 10; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            // Start at byte 7 (middle of "Line 1\n" boundary)
            // "Line 1\n" = 7 bytes, so byte 7 is start of "Line 2\n"
            auto gen = async_plain_file_bytes(test_file.string(), 7, 100);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            // start_byte=7 is exactly at "Line 2\n", but since start > 0,
            // we align to next newline. Byte 7 is 'L' of "Line 2", so we
            // skip to after the next \n (byte 14), yielding Line 3 onwards.
            CHECK(lines.size() > 0);

            fs::remove(test_file);
        }
    }

    TEST_CASE("Line Boundary Alignment") {
        SUBCASE("Start at byte 0 reads from beginning") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_align0.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "First\n";
                ofs << "Second\n";
                ofs << "Third\n";
            }

            auto gen = async_plain_file_bytes(test_file.string(), 0, 100);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 3);
            CHECK(lines[0] == "First");
            CHECK(lines[1] == "Second");
            CHECK(lines[2] == "Third");

            fs::remove(test_file);
        }

        SUBCASE("Start mid-line skips to next line") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_midline.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "AAAA\n";  // bytes 0-4, \n at 4
                ofs << "BBBB\n";  // bytes 5-9, \n at 9
                ofs << "CCCC\n";  // bytes 10-14, \n at 14
            }

            // Start at byte 2 (mid "AAAA"), should skip to byte 5 ("BBBB")
            auto gen = async_plain_file_bytes(test_file.string(), 2, 100);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 2);
            CHECK(lines[0] == "BBBB");
            CHECK(lines[1] == "CCCC");

            fs::remove(test_file);
        }

        SUBCASE("Start exactly at newline skips to next line") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_atnl.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "AAAA\n";  // bytes 0-4, \n at 4
                ofs << "BBBB\n";  // bytes 5-9, \n at 9
                ofs << "CCCC\n";  // bytes 10-14, \n at 14
            }

            // Start at byte 4 (\n of "AAAA\n"), alignment reads \n then
            // starts at byte 5 ("BBBB")
            auto gen = async_plain_file_bytes(test_file.string(), 4, 100);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 2);
            CHECK(lines[0] == "BBBB");
            CHECK(lines[1] == "CCCC");

            fs::remove(test_file);
        }

        SUBCASE("Start at line boundary (byte after newline)") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_boundary.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "AAAA\n";  // bytes 0-4, \n at 4
                ofs << "BBBB\n";  // bytes 5-9, \n at 9
                ofs << "CCCC\n";  // bytes 10-14, \n at 14
            }

            // Start at byte 5 (start of "BBBB"), but since start > 0,
            // alignment skips to next \n (byte 9), so we get CCCC only
            auto gen = async_plain_file_bytes(test_file.string(), 5, 100);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 1);
            CHECK(lines[0] == "CCCC");

            fs::remove(test_file);
        }
    }

    TEST_CASE("End Byte Handling") {
        SUBCASE("End byte truncates at line boundary") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_endbyte.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "AAAA\n";  // bytes 0-4
                ofs << "BBBB\n";  // bytes 5-9
                ofs << "CCCC\n";  // bytes 10-14
                ofs << "DDDD\n";  // bytes 15-19
            }

            // Read bytes 0-10: should get AAAA and BBBB (complete lines
            // before byte 10)
            auto gen = async_plain_file_bytes(test_file.string(), 0, 10);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.size() >= 2);
            CHECK(lines[0] == "AAAA");
            CHECK(lines[1] == "BBBB");

            fs::remove(test_file);
        }

        SUBCASE("End byte beyond file size reads all") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_beyond.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2\n";
            }

            auto gen = async_plain_file_bytes(test_file.string(), 0, 99999);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 2);
            CHECK(lines[0] == "Line 1");
            CHECK(lines[1] == "Line 2");

            fs::remove(test_file);
        }
    }

    TEST_CASE("Line Number Tracking") {
        SUBCASE("Line numbers start at 1 and increment") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_linenum.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "AAAA\n";
                ofs << "BBBB\n";
                ofs << "CCCC\n";
            }

            auto gen = async_plain_file_bytes(test_file.string(), 0, 100);
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 3);
            CHECK(lines[0].line_number == 1);
            CHECK(lines[1].line_number == 2);
            CHECK(lines[2].line_number == 3);

            fs::remove(test_file);
        }

        SUBCASE("Line numbers are relative to byte range") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_relnum.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "AAAA\n";  // bytes 0-4
                ofs << "BBBB\n";  // bytes 5-9
                ofs << "CCCC\n";  // bytes 10-14
            }

            // Start mid-file, line numbers should still start at 1
            auto gen = async_plain_file_bytes(test_file.string(), 2, 100);
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 2);
            CHECK(lines[0].line_number == 1);
            CHECK(lines[1].line_number == 2);

            fs::remove(test_file);
        }
    }

    TEST_CASE("Buffer Size Configuration") {
        SUBCASE("Small buffer size") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_smallbuf.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 10; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            auto gen = async_plain_file_bytes(test_file.string(), 0, 1000, 16);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 10);
            CHECK(lines[0] == "Line 1");
            CHECK(lines[9] == "Line 10");

            fs::remove(test_file);
        }

        SUBCASE("Large buffer size") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_largebuf.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 10; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            auto gen = async_plain_file_bytes(test_file.string(), 0, 1000,
                                              1024 * 1024);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 10);
            CHECK(lines[0] == "Line 1");
            CHECK(lines[9] == "Line 10");

            fs::remove(test_file);
        }
    }

    TEST_CASE("Error Handling") {
        SUBCASE("Non-existent file throws") {
            auto gen = async_plain_file_bytes(
                "non_existent_file_bytes_12345.txt", 0, 100);

            auto task = [](AsyncGenerator<Line> g) -> CoroTask<void> {
                (void)co_await g.next();
                co_return;
            }(std::move(gen));

            CHECK_THROWS_AS(task.get(), std::runtime_error);
        }

        SUBCASE("Invalid byte range throws") {
            auto gen = async_plain_file_bytes("any_file.txt", 100, 100);

            auto task = [](AsyncGenerator<Line> g) -> CoroTask<void> {
                (void)co_await g.next();
                co_return;
            }(std::move(gen));

            CHECK_THROWS_AS(task.get(), std::invalid_argument);
        }

        SUBCASE("Start greater than end throws") {
            auto gen = async_plain_file_bytes("any_file.txt", 200, 100);

            auto task = [](AsyncGenerator<Line> g) -> CoroTask<void> {
                (void)co_await g.next();
                co_return;
            }(std::move(gen));

            CHECK_THROWS_AS(task.get(), std::invalid_argument);
        }
    }

    TEST_CASE("Special Cases") {
        SUBCASE("File without trailing newline") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_nonl.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2";  // No trailing newline
            }

            auto gen = async_plain_file_bytes(test_file.string(), 0, 100);
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 2);
            CHECK(lines[0].content == "Line 1");
            CHECK(lines[1].content == "Line 2");

            fs::remove(test_file);
        }

        SUBCASE("Empty lines") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_empty_lines.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "\n";
                ofs << "Line 3\n";
            }

            auto gen = async_plain_file_bytes(test_file.string(), 0, 100);
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
                make_unique_test_path("test_async_plain_bytes_long.txt");
            {
                std::ofstream ofs(test_file);
                std::string long_line(10000, 'x');
                ofs << long_line << "\n";
            }

            auto gen = async_plain_file_bytes(test_file.string(), 0, 20000);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 1);
            CHECK(lines[0].size() == 10000);

            fs::remove(test_file);
        }

        SUBCASE("Many lines") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_many.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 1000; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            auto gen = async_plain_file_bytes(test_file.string(), 0, 999999);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 1000);
            CHECK(lines[0] == "Line 1");
            CHECK(lines[999] == "Line 1000");

            fs::remove(test_file);
        }
    }

    TEST_CASE("Consistency with Sync Iterator") {
        SUBCASE("Async and sync produce same results for full file") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_cons.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 20; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            // Get file size
            std::ifstream ifs(test_file, std::ios::ate | std::ios::binary);
            auto file_size = static_cast<std::size_t>(ifs.tellg());
            ifs.close();

            // Sync path
            auto sync_lines =
                sync_read_bytes_range(test_file.string(), 0, file_size);

            // Async path
            auto gen = async_plain_file_bytes(test_file.string(), 0, file_size);
            auto task = collect_lines(std::move(gen));
            auto async_lines = task.get();

            REQUIRE(sync_lines.size() == async_lines.size());
            for (std::size_t i = 0; i < sync_lines.size(); ++i) {
                CHECK(sync_lines[i] == async_lines[i]);
            }

            fs::remove(test_file);
        }

        SUBCASE("Async and sync produce same results for byte range") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_cons2.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 20; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            // Get file size
            std::ifstream ifs(test_file, std::ios::ate | std::ios::binary);
            auto file_size = static_cast<std::size_t>(ifs.tellg());
            ifs.close();

            std::size_t start = 30;
            std::size_t end = file_size - 20;

            // Sync path
            auto sync_lines =
                sync_read_bytes_range(test_file.string(), start, end);

            // Async path
            auto gen = async_plain_file_bytes(test_file.string(), start, end);
            auto task = collect_lines(std::move(gen));
            auto async_lines = task.get();

            REQUIRE(sync_lines.size() == async_lines.size());
            for (std::size_t i = 0; i < sync_lines.size(); ++i) {
                CHECK(sync_lines[i] == async_lines[i]);
            }

            fs::remove(test_file);
        }
    }

    TEST_CASE("Generator Lifecycle") {
        SUBCASE("Generator done() after exhaustion") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_life.txt");
            {
                std::ofstream ofs(test_file);
                ofs << "Line 1\n";
                ofs << "Line 2\n";
                ofs << "Line 3\n";
            }

            auto gen = async_plain_file_bytes(test_file.string(), 0, 100);

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
                make_unique_test_path("test_async_plain_bytes_move.txt");
            {
                std::ofstream ofs(test_file);
                for (int i = 1; i <= 5; ++i) {
                    ofs << "Line " << i << "\n";
                }
            }

            auto gen1 = async_plain_file_bytes(test_file.string(), 0, 1000);
            auto gen2 = std::move(gen1);  // Move construct

            auto task = collect_lines(std::move(gen2));
            auto lines = task.get();

            REQUIRE(lines.size() == 5);

            fs::remove(test_file);
        }
    }

    TEST_CASE("JSONL File") {
        SUBCASE("Read JSON lines via byte range") {
            fs::path test_file =
                make_unique_test_path("test_async_plain_bytes_jsonl.txt");
            {
                std::ofstream ofs(test_file);
                ofs << R"({"id": 1, "name": "Alice"})" << "\n";
                ofs << R"({"id": 2, "name": "Bob"})" << "\n";
                ofs << R"({"id": 3, "name": "Charlie"})" << "\n";
            }

            auto gen = async_plain_file_bytes(test_file.string(), 0, 1000);
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
