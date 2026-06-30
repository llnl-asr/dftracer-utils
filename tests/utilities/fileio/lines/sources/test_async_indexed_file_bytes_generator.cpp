#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_bytes_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/indexed_file_bytes_iterator.h>
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

TEST_SUITE("AsyncIndexedFileBytesGenerator") {
    TEST_CASE("Basic Byte Range Operations" * doctest::test_suite("vg")) {
        SUBCASE("Read entire file via byte range") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            auto gen = async_indexed_file_bytes(reader, 0, file_size);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

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
            auto gen = async_indexed_file_bytes(reader, 0, 300);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

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
            auto gen = async_indexed_file_bytes(reader, 200, 400);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.size() > 0);
        }
    }

    TEST_CASE("Line Number Tracking") {
        SUBCASE("Line numbers start at 1 and increment") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            auto gen = async_indexed_file_bytes(reader, 0, file_size);
            auto task = collect_line_copies(std::move(gen));
            auto lines = task.get();

            REQUIRE(lines.size() == 10);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                CHECK(lines[i].line_number == i + 1);
            }
        }
    }

    TEST_CASE("Buffer Size Configuration") {
        SUBCASE("Small buffer size") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            auto gen = async_indexed_file_bytes(reader, 0, file_size, 256);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.size() == 10);
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

            auto gen =
                async_indexed_file_bytes(reader, 0, file_size, 1024 * 1024);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.size() == 10);
        }
    }

    TEST_CASE("Error Handling") {
        SUBCASE("Null reader throws on first next()") {
            auto gen = async_indexed_file_bytes(nullptr, 0, 100);

            auto task = [](AsyncGenerator<Line> g) -> CoroTask<void> {
                (void)co_await g.next();
                co_return;
            }(std::move(gen));

            CHECK_THROWS_AS(task.get(), dftracer::utils::DFTUtilsException);
        }

        SUBCASE("Invalid byte range throws on first next()") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // start >= end
            auto gen = async_indexed_file_bytes(reader, 100, 100);

            auto task = [](AsyncGenerator<Line> g) -> CoroTask<void> {
                (void)co_await g.next();
                co_return;
            }(std::move(gen));

            CHECK_THROWS_AS(task.get(), dftracer::utils::DFTUtilsException);
        }

        SUBCASE("Byte range beyond file size handles gracefully") {
            TestEnvironment env(5);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            auto gen = async_indexed_file_bytes(reader, 0, file_size * 2);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.size() == 5);
        }
    }

    TEST_CASE("Large Files") {
        SUBCASE("Large file with specific byte range") {
            TestEnvironment env(valgrind_scale(1000, 3));
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 4096, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Read bytes 5000-10000
            auto gen = async_indexed_file_bytes(reader, 5000, 10000);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.size() > 0);
            CHECK(lines.size() < 1000);
        }
    }

    TEST_CASE("Consistency with Sync Iterator") {
        SUBCASE("Async and sync produce same results") {
            TestEnvironment env(20);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            // Sync path
            IndexedFileBytesIterator sync_iter(reader, 0, file_size);
            std::vector<std::string> sync_lines;
            while (sync_iter.has_next()) {
                Line line = sync_iter.next();
                sync_lines.push_back(std::string(line.content));
            }

            // Need a fresh reader for async path (stream state)
            auto reader2 = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader2 != nullptr);

            // Async path
            auto gen = async_indexed_file_bytes(reader2, 0, file_size);
            auto task = collect_lines(std::move(gen));
            auto async_lines = task.get();

            REQUIRE(sync_lines.size() == async_lines.size());
            for (std::size_t i = 0; i < sync_lines.size(); ++i) {
                CHECK(sync_lines[i] == async_lines[i]);
            }
        }
    }

    TEST_CASE("Generator Lifecycle" * doctest::test_suite("vg")) {
        SUBCASE("Generator done() after exhaustion") {
            TestEnvironment env(3);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            auto gen = async_indexed_file_bytes(reader, 0, file_size);

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

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            std::size_t file_size = reader->get_max_bytes();

            auto gen1 = async_indexed_file_bytes(reader, 0, file_size);
            auto gen2 = std::move(gen1);  // Move construct

            auto task = collect_lines(std::move(gen2));
            auto lines = task.get();

            REQUIRE(lines.size() == 5);
        }
    }

    TEST_CASE("Byte Range Edge Cases") {
        SUBCASE("Very small byte range") {
            TestEnvironment env(10);
            std::string gz_path = env.create_test_gzip_file();

            auto indexer = IndexerFactory::create(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            indexer->build();

            auto reader = ReaderFactory::create(gz_path, gz_path + ".idx");
            REQUIRE(reader != nullptr);

            // Read just 50 bytes
            auto gen = async_indexed_file_bytes(reader, 0, 50);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.size() > 0);
            CHECK(lines[0] == "{\"id\": 1, \"message\": \"Test message 1\"}");
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
            auto gen = async_indexed_file_bytes(reader, 50, file_size);
            auto task = collect_lines(std::move(gen));
            auto lines = task.get();

            CHECK(lines.size() > 0);
        }
    }
}
