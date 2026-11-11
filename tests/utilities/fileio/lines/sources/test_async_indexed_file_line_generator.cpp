// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <checkpoint_indexer_helpers.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/trace/reader/async_indexed_file_line_generator.h>
#include <dftracer/utils/trace/reader/internal/reader_factory.h>
#include <doctest/doctest.h>
#include <generator_ok.h>
#include <task_result.h>
#include <testing_utilities.h>

using namespace dftracer::utils::trace::reader;
using namespace dftracer::utils::fileio::lines;
using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::trace::index::gzip;
using namespace dftracer::utils::trace::reader::internal;
using namespace dftu_utils_test;

// The first item of a generator of Results.
static CoroTask<dstd::Result<Line>> first_item(
    AsyncGenerator<dstd::Result<Line>> gen) {
    auto item = co_await gen.next();
    if (!item) {
        co_return common::make_error(common::ErrorCode::Unknown,
                                     "the generator ended without an item");
    }
    co_return std::move(*item);
}

// Helper: consume an AsyncGenerator<Line> into a vector of strings
static CoroTask<dstd::Vector<dstd::String>> collect_lines(
    AsyncGenerator<Line> gen) {
    dstd::Vector<dstd::String> result;
    while (auto line = co_await gen.next()) {
        result.push_back(dstd::String(line->content));
    }
    co_return result;
}

// Helper: consume an AsyncGenerator<Line> into a vector of Line copies
struct LineCopy {
    dstd::String content;
    std::size_t line_number;
};

static CoroTask<dstd::Vector<LineCopy>> collect_line_copies(
    AsyncGenerator<Line> gen) {
    dstd::Vector<LineCopy> result;
    while (auto line = co_await gen.next()) {
        result.push_back(
            LineCopy{dstd::String(line->content), line->line_number});
    }
    co_return result;
}

TEST_SUITE("AsyncIndexedFileLineGenerator") {
    TEST_CASE("Basic Operations with Line Range" * doctest::test_suite("vg")) {
        SUBCASE("Read entire indexed file") {
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                dftu_utils_test::create_indexer(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            dftu_utils_test::build_indexer(*indexer);

            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                    .with_line_range(1, 10);

            auto gen =
                dftu_utils_test::ok_items(async_indexed_file_lines(config));
            auto task = collect_lines(std::move(gen));
            auto lines = dftu_utils_test::ok(task.get());

            REQUIRE(lines.size() == 10);
            CHECK(lines[0] == "{\"id\": 1, \"message\": \"Test message 1\"}");
            CHECK(lines[9] == "{\"id\": 10, \"message\": \"Test message 10\"}");
        }

        SUBCASE("Read partial line range") {
            TestEnvironment env(20);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                dftu_utils_test::create_indexer(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            dftu_utils_test::build_indexer(*indexer);

            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                    .with_line_range(5, 10);

            auto gen =
                dftu_utils_test::ok_items(async_indexed_file_lines(config));
            auto task = collect_lines(std::move(gen));
            auto lines = dftu_utils_test::ok(task.get());

            REQUIRE(lines.size() == 6);
            CHECK(lines[0] == "{\"id\": 5, \"message\": \"Test message 5\"}");
            CHECK(lines[5] == "{\"id\": 10, \"message\": \"Test message 10\"}");
        }

        SUBCASE("Read single line") {
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                dftu_utils_test::create_indexer(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            dftu_utils_test::build_indexer(*indexer);

            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                    .with_line_range(7, 7);

            auto gen =
                dftu_utils_test::ok_items(async_indexed_file_lines(config));
            auto task = collect_line_copies(std::move(gen));
            auto lines = dftu_utils_test::ok(task.get());

            REQUIRE(lines.size() == 1);
            CHECK(lines[0].content ==
                  "{\"id\": 7, \"message\": \"Test message 7\"}");
            CHECK(lines[0].line_number == 7);
        }

        SUBCASE("Auto-detect end line") {
            TestEnvironment env(15);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                dftu_utils_test::create_indexer(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            dftu_utils_test::build_indexer(*indexer);

            // end=0 should auto-detect total lines
            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                    .with_line_range(10, 0);

            auto gen =
                dftu_utils_test::ok_items(async_indexed_file_lines(config));
            auto task = collect_lines(std::move(gen));
            auto lines = dftu_utils_test::ok(task.get());

            REQUIRE(lines.size() == 6);  // Lines 10-15
            CHECK(lines[0] == "{\"id\": 10, \"message\": \"Test message 10\"}");
            CHECK(lines[5] == "{\"id\": 15, \"message\": \"Test message 15\"}");
        }
    }

    TEST_CASE("Configuration with Reader") {
        SUBCASE("Use existing reader") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                dftu_utils_test::create_indexer(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            dftu_utils_test::build_indexer(*indexer);

            auto reader = dftu_utils_test::ok(
                ReaderFactory::create(gz_path, dstd::fmt<"{}.idx">(gz_path)));
            REQUIRE(reader != nullptr);

            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_reader(reader)
                    .with_line_range(2, 4)
                    .with_buffer_size(4096);

            auto gen =
                dftu_utils_test::ok_items(async_indexed_file_lines(config));
            auto task = collect_lines(std::move(gen));
            auto lines = dftu_utils_test::ok(task.get());

            REQUIRE(lines.size() == 3);  // Lines 2, 3, 4
            CHECK(lines[0] == "{\"id\": 2, \"message\": \"Test message 2\"}");
            CHECK(lines[2] == "{\"id\": 4, \"message\": \"Test message 4\"}");
        }
    }

    TEST_CASE("Byte Range") {
        SUBCASE("Read with byte range") {
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                dftu_utils_test::create_indexer(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            dftu_utils_test::build_indexer(*indexer);

            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                    .with_byte_range(0, 200);

            auto gen =
                dftu_utils_test::ok_items(async_indexed_file_lines(config));
            auto task = collect_lines(std::move(gen));
            auto lines = dftu_utils_test::ok(task.get());

            CHECK(lines.size() > 0);
        }
    }

    TEST_CASE("Line Number Tracking") {
        SUBCASE("Line numbers match expected positions") {
            TestEnvironment env(10);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                dftu_utils_test::create_indexer(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            dftu_utils_test::build_indexer(*indexer);

            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                    .with_line_range(3, 7);

            auto gen =
                dftu_utils_test::ok_items(async_indexed_file_lines(config));
            auto task = collect_line_copies(std::move(gen));
            auto lines = dftu_utils_test::ok(task.get());

            REQUIRE(lines.size() == 5);
            CHECK(lines[0].line_number == 3);
            CHECK(lines[1].line_number == 4);
            CHECK(lines[2].line_number == 5);
            CHECK(lines[3].line_number == 6);
            CHECK(lines[4].line_number == 7);
        }
    }

    TEST_CASE("Error Handling") {
        SUBCASE("Null reader is an error item") {
            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_line_range(1, 10);

            auto task = first_item(async_indexed_file_lines(config));

            dftu_utils_test::expect_error(task.get(),
                                          common::ErrorCode::InvalidArgument,
                                          "Reader cannot be null");
        }

        SUBCASE("Non-existent file is an error item") {
            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_file("non_existent.gz", "non_existent.gz.idx");
            REQUIRE(config.error().has_value());

            auto task = first_item(async_indexed_file_lines(config));
            auto first = task.get();
            REQUIRE(!first.has_value());
            CHECK(dstd::StringView(first.error().c_str())
                      .find("does not exist") != dstd::StringView::npos);
        }
    }

    TEST_CASE("Large Files") {
        SUBCASE("Large line range") {
            TestEnvironment env(valgrind_scale(1000, 4));
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                dftu_utils_test::create_indexer(gz_path, "", 4096, true);
            REQUIRE(indexer != nullptr);
            dftu_utils_test::build_indexer(*indexer);

            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                    .with_line_range(100, 200)
                    .with_buffer_size(16384);

            auto gen =
                dftu_utils_test::ok_items(async_indexed_file_lines(config));
            auto task = collect_lines(std::move(gen));
            auto lines = dftu_utils_test::ok(task.get());

            REQUIRE(lines.size() == 101);  // Lines 100-200 inclusive
            CHECK(lines[0] ==
                  "{\"id\": 100, \"message\": \"Test message 100\"}");
            CHECK(lines[100] ==
                  "{\"id\": 200, \"message\": \"Test message 200\"}");
        }
    }

    TEST_CASE("Generator Lifecycle" * doctest::test_suite("vg")) {
        SUBCASE("Generator done() after exhaustion") {
            TestEnvironment env(3);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                dftu_utils_test::create_indexer(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            dftu_utils_test::build_indexer(*indexer);

            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                    .with_line_range(1, 3);

            auto gen =
                dftu_utils_test::ok_items(async_indexed_file_lines(config));

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

            dftu_utils_test::ok(task.get());
        }

        SUBCASE("Move semantics") {
            TestEnvironment env(5);
            dstd::String gz_path = env.create_test_gzip_file();

            auto indexer =
                dftu_utils_test::create_indexer(gz_path, "", 1024, true);
            REQUIRE(indexer != nullptr);
            dftu_utils_test::build_indexer(*indexer);

            auto config =
                dftracer::utils::trace::reader::IndexedFileLineIteratorConfig()
                    .with_file(gz_path, dstd::fmt<"{}.idx">(gz_path))
                    .with_line_range(1, 5);

            auto gen1 =
                dftu_utils_test::ok_items(async_indexed_file_lines(config));
            auto gen2 = std::move(gen1);  // Move construct

            auto task = collect_lines(std::move(gen2));
            auto lines = dftu_utils_test::ok(task.get());

            REQUIRE(lines.size() == 5);
        }
    }
}
