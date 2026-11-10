// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/gzip_reader.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <testing_utilities.h>

#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::reader::internal;
using namespace dftu_utils_test;

TEST_CASE("a GzipReader without an indexer is fatal") {
    auto result = fresh_process::run("gzip reader null indexer scenario");
    CHECK(result.aborted_with("GzipReader needs an indexer"));
}

TEST_CASE("gzip reader null indexer scenario" * doctest::skip()) {
    fresh_process::announce_abort();
    GzipReader reader(nullptr);
}

TEST_CASE("a moved-from GzipReader used again is fatal") {
    auto result = fresh_process::run("gzip reader moved from scenario");
    CHECK(result.aborted_with("GzipReader used after it was moved from"));
}

TEST_CASE("gzip reader moved from scenario" * doctest::skip()) {
    fresh_process::announce_abort();
    TestEnvironment env(10);
    dstd::String gz_file = env.create_dft_test_gzip_file(10);
    auto indexer = index::gzip::CheckpointIndexerFactory::create(
        gz_file, env.get_index_path(gz_file), 32 * 1024 * 1024, false);
    indexer->build();
    GzipReader first(std::move(indexer));
    GzipReader second(std::move(first));
    (void)first.max_bytes();
}
