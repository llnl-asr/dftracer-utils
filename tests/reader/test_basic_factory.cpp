#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/indexer/indexer_factory.h>
#include <dftracer/utils/reader/reader_factory.h>
#include <doctest/doctest.h>

#include "testing_utilities.h"

using namespace dftracer::utils;
using namespace dft_utils_test;

TEST_CASE("Factory Pattern - Basic GZIP functionality") {
    TestEnvironment env(100, Format::GZIP);
    REQUIRE(env.is_valid());

    std::string gz_file = env.create_test_file();
    REQUIRE(!gz_file.empty());

    std::string idx_file = env.get_index_path(gz_file);

    SUBCASE("IndexerFactory creates valid indexer") {
        auto indexer = IndexerFactory::create(gz_file, idx_file, 1024 * 1024);
        REQUIRE(indexer != nullptr);
        CHECK(indexer->get_archive_path() == gz_file);
        CHECK(indexer->get_idx_path() == idx_file);
    }

    SUBCASE("ReaderFactory creates valid reader") {
        auto indexer = IndexerFactory::create(gz_file, idx_file, 1024 * 1024);
        REQUIRE(indexer != nullptr);
        indexer->build();

        auto reader = ReaderFactory::create(indexer);
        REQUIRE(reader != nullptr);
        CHECK(reader->is_valid());
        CHECK(reader->get_archive_path() == gz_file);
        CHECK(reader->get_idx_path() == idx_file);
    }

    SUBCASE("Reader factory from files") {
        // First create index
        auto indexer = IndexerFactory::create(gz_file, idx_file, 1024 * 1024);
        REQUIRE(indexer != nullptr);
        indexer->build();

        // Then create reader from files
        auto reader = ReaderFactory::create(gz_file, idx_file, 1024 * 1024);
        REQUIRE(reader != nullptr);
        CHECK(reader->is_valid());
    }
}

TEST_CASE("Factory Pattern - Basic TAR.GZ functionality") {
    TestEnvironment env(100, Format::TAR_GZIP);
    REQUIRE(env.is_valid());

    std::string tar_gz_file = env.create_test_file();
    REQUIRE(!tar_gz_file.empty());

    std::string idx_file = env.get_index_path(tar_gz_file);

    SUBCASE("IndexerFactory creates valid TAR.GZ indexer") {
        auto indexer =
            IndexerFactory::create(tar_gz_file, idx_file, 1024 * 1024);
        REQUIRE(indexer != nullptr);
        CHECK(indexer->get_archive_path() == tar_gz_file);
        CHECK(indexer->get_idx_path() == idx_file);
    }

    SUBCASE("ReaderFactory creates valid TAR.GZ reader") {
        auto indexer =
            IndexerFactory::create(tar_gz_file, idx_file, 1024 * 1024);
        REQUIRE(indexer != nullptr);
        indexer->build();

        auto reader = ReaderFactory::create(indexer);
        REQUIRE(reader != nullptr);
        CHECK(reader->is_valid());
        CHECK(reader->get_archive_path() == tar_gz_file);
        CHECK(reader->get_idx_path() == idx_file);
    }
}

TEST_CASE("Basic Reading Operations") {
    TestEnvironment env(50, Format::GZIP);
    REQUIRE(env.is_valid());

    std::string gz_file = env.create_test_file();
    REQUIRE(!gz_file.empty());

    std::string idx_file = env.get_index_path(gz_file);

    auto indexer = IndexerFactory::create(gz_file, idx_file, 512 * 1024);
    REQUIRE(indexer != nullptr);
    indexer->build();

    auto reader = ReaderFactory::create(indexer);
    REQUIRE(reader != nullptr);

    SUBCASE("Basic metadata access") {
        CHECK(reader->get_num_lines() > 0);
        CHECK(reader->get_max_bytes() > 0);
        MESSAGE("Lines: " << reader->get_num_lines()
                          << ", Bytes: " << reader->get_max_bytes());
    }

    SUBCASE("Line-based reading") {
        std::size_t num_lines = reader->get_num_lines();
        if (num_lines > 0) {
            std::string content = reader->read_lines(
                1, std::min(num_lines, static_cast<std::size_t>(5)));
            CHECK(!content.empty());
            CHECK(content.find("\"id\":") != std::string::npos);
        }
    }

    SUBCASE("Byte-based reading") {
        std::size_t max_bytes = reader->get_max_bytes();
        if (max_bytes > 0) {
            std::vector<char> buffer(1024);
            std::size_t bytes_read = reader->read(
                0, std::min(max_bytes, static_cast<std::size_t>(1024)),
                buffer.data(), buffer.size());
            CHECK(bytes_read > 0);
        }
    }
}
