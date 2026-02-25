#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_line_generator.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <string>
#include <vector>

using namespace dftracer::utils::utilities::fileio::lines::sources;
using namespace dftracer::utils::utilities::fileio::lines;
using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::utilities::indexer::internal;
using namespace dftracer::utils::utilities::reader::internal;
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

// Helper: consume an AsyncGenerator<Line> into a vector of Line copies
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

TEST_SUITE("AsyncIndexedFileLineGenerator") {
    TEST_CASE("Basic Operations with Line Range") {
        SUBCASE("Read entire indexed file") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(1, 10);

            auto gen = async_indexed_file_lines(config);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

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

            auto gen = async_indexed_file_lines(config);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

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

            auto gen = async_indexed_file_lines(config);
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 1);
            CHECK(lines[0].content ==
                  "{\"id\": 7, \"message\": \"Test message 7\"}");
            CHECK(lines[0].line_number == 7);
        }

        SUBCASE("Auto-detect end line") {
            TestEnvironment env(15);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            // end=0 should auto-detect total lines
            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(10, 0);

            auto gen = async_indexed_file_lines(config);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 6);  // Lines 10-15
            CHECK(lines[0] == "{\"id\": 10, \"message\": \"Test message 10\"}");
            CHECK(lines[5] == "{\"id\": 15, \"message\": \"Test message 15\"}");
        }
    }

    TEST_CASE("Configuration with Reader") {
        SUBCASE("Use existing reader") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            auto config = IndexedFileLineIteratorConfig()
                              .with_reader(reader)
                              .with_line_range(2, 4)
                              .with_buffer_size(4096);

            auto gen = async_indexed_file_lines(config);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 3);  // Lines 2, 3, 4
            CHECK(lines[0] == "{\"id\": 2, \"message\": \"Test message 2\"}");
            CHECK(lines[2] == "{\"id\": 4, \"message\": \"Test message 4\"}");
        }
    }

    TEST_CASE("Byte Range") {
        SUBCASE("Read with byte range") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_byte_range(0, 200);

            auto gen = async_indexed_file_lines(config);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.size() > 0);
        }
    }

    TEST_CASE("Line Number Tracking") {
        SUBCASE("Line numbers match expected positions") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(3, 7);

            auto gen = async_indexed_file_lines(config);
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 5);
            CHECK(lines[0].line_number == 3);
            CHECK(lines[1].line_number == 4);
            CHECK(lines[2].line_number == 5);
            CHECK(lines[3].line_number == 6);
            CHECK(lines[4].line_number == 7);
        }
    }

    TEST_CASE("Error Handling") {
        SUBCASE("Null reader throws on first next()") {
            auto config =
                IndexedFileLineIteratorConfig().with_line_range(1, 10);

            auto gen = async_indexed_file_lines(config);

            // Exception is thrown inside the coroutine body,
            // which executes on the first co_await gen.next()
            auto task = [](AsyncGenerator<Line> g) -> CoroTask<void> {
                (void)co_await g.next();
                co_return;
            }(std::move(gen));

            CHECK_THROWS_AS(task.get(), std::invalid_argument);
        }

        SUBCASE("Non-existent file throws") {
            bool exception_thrown = false;
            try {
                auto config = IndexedFileLineIteratorConfig().with_file(
                    "non_existent.gz", "non_existent.gz.idx");
                auto gen = async_indexed_file_lines(config);
                // Consume to trigger the coroutine body
                auto task = collect_lines(std::move(gen));
                task.get();
            } catch (const std::exception&) {
                exception_thrown = true;
            }
            CHECK(exception_thrown);
        }
    }

    TEST_CASE("Large Files") {
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

            auto gen = async_indexed_file_lines(config);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 101);  // Lines 100-200 inclusive
            CHECK(lines[0] ==
                  "{\"id\": 100, \"message\": \"Test message 100\"}");
            CHECK(lines[100] ==
                  "{\"id\": 200, \"message\": \"Test message 200\"}");
        }
    }

    TEST_CASE("Consistency with Sync Iterator") {
        SUBCASE("Async and sync produce same results") {
            TestEnvironment env(20);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(5, 15);

            // Sync path
            IndexedFileLineIterator sync_iter(config);
            std::vector<std::string> sync_lines;
            while (sync_iter.has_next()) {
                Line line = sync_iter.next();
                sync_lines.push_back(std::string(line.content));
            }

            // Async path
            auto gen = async_indexed_file_lines(config);
            auto task = collect_lines(std::move(gen));
            auto async_lines = task.get();

            REQUIRE(sync_lines.size() == async_lines.size());
            for (std::size_t i = 0; i < sync_lines.size(); ++i) {
                CHECK(sync_lines[i] == async_lines[i]);
            }
        }
    }

    TEST_CASE("Generator Lifecycle") {
        SUBCASE("Generator done() after exhaustion") {
            TestEnvironment env(3);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(1, 3);

            auto gen = async_indexed_file_lines(config);

            auto task = [](AsyncGenerator<Line> g) -> CoroTask<void> {
                // Consume all lines
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
        }

        SUBCASE("Move semantics") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto config = IndexedFileLineIteratorConfig()
                              .with_file(gz_path, gz_path + ".idx")
                              .with_line_range(1, 5);

            auto gen1 = async_indexed_file_lines(config);
            auto gen2 = std::move(gen1);  // Move construct

            auto task = collect_lines(std::move(gen2));
            auto lines = task.get();

            REQUIRE(lines.size() == 5);
        }
    }
}
