#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/extensions/bloom_filter.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace dftracer::utils::index::extensions;

namespace {

double false_positive_rate(const ScalableBloomFilter& bf, std::size_t inserted,
                           std::size_t probes) {
    std::size_t hits = 0;
    for (std::size_t i = 0; i < probes; ++i) {
        if (bf.possibly_contains("absent-" + std::to_string(inserted + i))) {
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
            bf.add("value-" + std::to_string(i));
        }
        for (std::size_t i = 0; i < n; ++i) {
            REQUIRE(bf.possibly_contains("value-" + std::to_string(i)));
        }
        CHECK(bf.num_levels() > 1);
    }

    TEST_CASE("Stays selective where a fixed filter saturates") {
        const std::size_t n = 50000;

        BloomFilter fixed(1024, 0.01);
        ScalableBloomFilter scalable(1024, 0.01);
        for (std::size_t i = 0; i < n; ++i) {
            auto v = "value-" + std::to_string(i);
            fixed.add(v);
            scalable.add(v);
        }

        std::size_t fixed_hits = 0;
        for (std::size_t i = 0; i < 2000; ++i) {
            if (fixed.possibly_contains("absent-" + std::to_string(n + i))) {
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
                bf.add("value-" + std::to_string(i));
            }
        }
        CHECK(bf.num_entries() == 500);
    }

    TEST_CASE("Round-trips through serialization") {
        ScalableBloomFilter bf(64, 0.01);
        const std::size_t n = 5000;
        for (std::size_t i = 0; i < n; ++i) {
            bf.add("value-" + std::to_string(i));
        }

        auto blob = bf.serialize();
        auto restored =
            ScalableBloomFilter::from_blob(blob.data(), blob.size());

        CHECK(restored.num_levels() == bf.num_levels());
        CHECK(restored.num_entries() == bf.num_entries());
        for (std::size_t i = 0; i < n; ++i) {
            REQUIRE(restored.possibly_contains("value-" + std::to_string(i)));
        }
    }

    TEST_CASE("Reads a bare BloomFilter blob") {
        BloomFilter legacy(128, 0.01);
        legacy.add("alpha");
        legacy.add("beta");
        auto blob = legacy.serialize();

        auto restored =
            ScalableBloomFilter::from_blob(blob.data(), blob.size());
        CHECK(restored.num_levels() == 1);
        CHECK(restored.possibly_contains("alpha"));
        CHECK(restored.possibly_contains("beta"));
        CHECK_FALSE(restored.possibly_contains("gamma"));
    }

    TEST_CASE("Merge unions both sides") {
        ScalableBloomFilter a(1024, 0.01);
        ScalableBloomFilter b(1024, 0.01);
        for (std::size_t i = 0; i < 3000; ++i) {
            a.add("a-" + std::to_string(i));
            b.add("b-" + std::to_string(i));
        }

        a.merge_from(b);
        for (std::size_t i = 0; i < 3000; ++i) {
            REQUIRE(a.possibly_contains("a-" + std::to_string(i)));
            REQUIRE(a.possibly_contains("b-" + std::to_string(i)));
        }
    }

    TEST_CASE("Repeated merges keep the chain logarithmic") {
        ScalableBloomFilter parent(1024, 0.01);
        for (std::size_t slice = 0; slice < 32; ++slice) {
            ScalableBloomFilter s(1024, 0.01);
            for (std::size_t i = 0; i < 2000; ++i) {
                s.add("s" + std::to_string(slice) + "-" + std::to_string(i));
            }
            parent.merge_from(s);
        }
        // Levels are keyed by capacity, so merging 32 slices must not grow
        // the chain 32-fold.
        CHECK(parent.num_levels() <= 8);
    }
}
