// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::index::gzip;
using namespace dftracer::utils::utilities::reader::internal;
using namespace dftu_utils_test;

TEST_CASE("Factory Pattern - Basic GZIP functionality") {
    TestEnvironment env(100, Format::Gzip);
    REQUIRE(env.is_valid());

    dstd::String gz_file = env.create_test_file();
    REQUIRE(!gz_file.empty());

    dstd::String idx_file = env.get_index_path(gz_file);
    dstd::String db_root = dstd::String(determine_index_path(gz_file, ""));

    SUBCASE("CheckpointIndexerFactory creates valid indexer") {
        auto indexer =
            CheckpointIndexerFactory::create(gz_file, idx_file, 1024 * 1024);
        REQUIRE(indexer != nullptr);
        CHECK(dstd::StringView(indexer->archive_path()) == gz_file);
        CHECK(dstd::StringView(indexer->index_path()) == db_root);
    }

    SUBCASE("ReaderFactory creates valid reader") {
        auto indexer =
            CheckpointIndexerFactory::create(gz_file, idx_file, 1024 * 1024);
        REQUIRE(indexer != nullptr);
        indexer->build();

        auto reader = ReaderFactory::create(indexer);
        REQUIRE(reader != nullptr);
        CHECK(reader->is_valid());
        CHECK(dstd::StringView(reader->archive_path()) == gz_file);
        CHECK(dstd::StringView(reader->index_path()) == db_root);
    }

    SUBCASE("Reader factory from files") {
        // First create index
        auto indexer =
            CheckpointIndexerFactory::create(gz_file, idx_file, 1024 * 1024);
        REQUIRE(indexer != nullptr);
        indexer->build();

        // Then create reader from files
        auto reader = ReaderFactory::create(gz_file, idx_file, 1024 * 1024);
        REQUIRE(reader != nullptr);
        CHECK(reader->is_valid());
    }
}

TEST_CASE("Basic Reading Operations") {
    TestEnvironment env(50, Format::Gzip);
    REQUIRE(env.is_valid());

    dstd::String gz_file = env.create_test_file();
    REQUIRE(!gz_file.empty());

    dstd::String idx_file = env.get_index_path(gz_file);

    auto indexer =
        CheckpointIndexerFactory::create(gz_file, idx_file, 512 * 1024);
    REQUIRE(indexer != nullptr);
    indexer->build();

    auto reader = ReaderFactory::create(indexer);
    REQUIRE(reader != nullptr);

    SUBCASE("Basic metadata access") {
        CHECK(reader->line_count() > 0);
        CHECK(reader->max_bytes() > 0);
        MESSAGE("Lines: " << reader->line_count()
                          << ", Bytes: " << reader->max_bytes());
    }

    SUBCASE("Line-based reading") {
        std::size_t line_count = reader->line_count();
        if (line_count > 0) {
            dstd::String content = dftu_utils_test::ok(reader->read_lines(
                1, std::min(line_count, static_cast<std::size_t>(5))));
            CHECK(!content.empty());
            CHECK(content.find("\"id\":") != dstd::String::npos);
        }
    }

    SUBCASE("Byte-based reading") {
        std::size_t max_bytes = reader->max_bytes();
        if (max_bytes > 0) {
            dstd::Vector<char> buffer(1024);
            std::size_t bytes_read = dftu_utils_test::ok(reader->read(
                0, std::min(max_bytes, static_cast<std::size_t>(1024)),
                buffer.data(), buffer.size()));
            CHECK(bytes_read > 0);
        }
    }
}
