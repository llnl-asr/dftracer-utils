// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/trace/index/extensions/bloom_filter.h>
#include <dftracer/utils/trace/index/extensions/scalable_bloom_filter.h>
#include <doctest/doctest.h>
#include <task_result.h>

using namespace dftracer::utils;

using namespace dftracer::utils::trace::index::extensions;

namespace {

double false_positive_rate(const ScalableBloomFilter& bf, std::size_t inserted,
                           std::size_t probes) {
    std::size_t hits = 0;
    for (std::size_t i = 0; i < probes; ++i) {
        if (bf.possibly_contains("absent-" + dstd::to_string(inserted + i))) {
            ++hits;
        }
    }
    return static_cast<double>(hits) / static_cast<double>(probes);
}

}  // namespace

TEST_SUITE("ScalableBloomFilter") {
    TEST_CASE("No false negatives past the initial capacity") {
        ScalableBloomFilter bf(64, 0.01);
        const std::size_t n = 20000;
        for (std::size_t i = 0; i < n; ++i) {
            bf.add("value-" + dstd::to_string(i));
        }
        for (std::size_t i = 0; i < n; ++i) {
            REQUIRE(bf.possibly_contains("value-" + dstd::to_string(i)));
        }
        CHECK(bf.level_count() > 1);
    }

    TEST_CASE("Stays selective where a fixed filter saturates") {
        const std::size_t n = 50000;

        BloomFilter fixed(1024, 0.01);
        ScalableBloomFilter scalable(1024, 0.01);
        for (std::size_t i = 0; i < n; ++i) {
            auto v = "value-" + dstd::to_string(i);
            fixed.add(v);
            scalable.add(v);
        }

        std::size_t fixed_hits = 0;
        for (std::size_t i = 0; i < 2000; ++i) {
            if (fixed.possibly_contains("absent-" + dstd::to_string(n + i))) {
                ++fixed_hits;
            }
        }
        // The whole point: 50k distinct values in a filter sized for 1024
        // matches essentially everything, so it prunes nothing.
        CHECK(fixed_hits > 1900);
        CHECK(false_positive_rate(scalable, n, 2000) < 0.05);
    }

    TEST_CASE("Distinct count ignores repeats") {
        ScalableBloomFilter bf(1024, 0.01);
        for (std::size_t rep = 0; rep < 10; ++rep) {
            for (std::size_t i = 0; i < 500; ++i) {
                bf.add("value-" + dstd::to_string(i));
            }
        }
        CHECK(bf.entry_count() == 500);
    }

    TEST_CASE("Round-trips through serialization") {
        ScalableBloomFilter bf(64, 0.01);
        const std::size_t n = 5000;
        for (std::size_t i = 0; i < n; ++i) {
            bf.add("value-" + dstd::to_string(i));
        }

        auto blob = bf.serialize();
        auto restored = dftu_utils_test::ok(
            ScalableBloomFilter::from_blob(blob.data(), blob.size()));

        CHECK(restored.level_count() == bf.level_count());
        CHECK(restored.entry_count() == bf.entry_count());
        for (std::size_t i = 0; i < n; ++i) {
            REQUIRE(restored.possibly_contains("value-" + dstd::to_string(i)));
        }
    }

    TEST_CASE("Refuses bytes that are not a chain") {
        BloomFilter bare(128, 0.01);
        bare.add("alpha");
        const auto blob = bare.serialize();
        const auto loaded =
            ScalableBloomFilter::from_blob(blob.data(), blob.size());
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().is_code(common::ErrorCode::Parse));
    }

    TEST_CASE("A chain grows the same after a reload") {
        ScalableBloomFilter chunk(1024, 0.01);
        chunk.add("seed");
        ScalableBloomFilter file(1024, 0.01);
        file.merge_from(chunk);
        const std::size_t levels = file.level_count();
        const std::size_t bytes = file.size_bytes();
        for (int round = 0; round < 50; ++round) {
            const auto blob = file.serialize();
            file = dftu_utils_test::ok(
                ScalableBloomFilter::from_blob(blob.data(), blob.size()));
            file.merge_from(chunk);
        }
        CHECK(file.level_count() == levels);
        CHECK(file.size_bytes() == bytes);
        CHECK(file.possibly_contains("seed"));

        const auto blob = file.serialize();
        auto loaded = dftu_utils_test::ok(
            ScalableBloomFilter::from_blob(blob.data(), blob.size()));
        ScalableBloomFilter direct(1024, 0.01);
        direct.add("seed");
        for (std::size_t i = 0; i < 5000; ++i) {
            const auto value = "value-" + dstd::to_string(i);
            loaded.add(value);
            direct.add(value);
        }
        CHECK(loaded.level_count() == direct.level_count());
        CHECK(loaded.size_bytes() == direct.size_bytes());
        for (std::size_t i = 0; i < 5000; ++i) {
            REQUIRE(loaded.possibly_contains("value-" + dstd::to_string(i)));
        }
    }

    TEST_CASE("Merge unions both sides") {
        ScalableBloomFilter a(1024, 0.01);
        ScalableBloomFilter b(1024, 0.01);
        for (std::size_t i = 0; i < 3000; ++i) {
            a.add("a-" + dstd::to_string(i));
            b.add("b-" + dstd::to_string(i));
        }

        a.merge_from(b);
        for (std::size_t i = 0; i < 3000; ++i) {
            REQUIRE(a.possibly_contains("a-" + dstd::to_string(i)));
            REQUIRE(a.possibly_contains("b-" + dstd::to_string(i)));
        }
    }

    TEST_CASE("Repeated merges keep the chain logarithmic") {
        ScalableBloomFilter parent(1024, 0.01);
        for (std::size_t slice = 0; slice < 32; ++slice) {
            ScalableBloomFilter s(1024, 0.01);
            for (std::size_t i = 0; i < 2000; ++i) {
                s.add("s" + dstd::to_string(slice) + "-" + dstd::to_string(i));
            }
            parent.merge_from(s);
        }
        // Levels are keyed by capacity, so merging 32 slices must not grow
        // the chain 32-fold.
        CHECK(parent.level_count() <= 8);
    }
}
