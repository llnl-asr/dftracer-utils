// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/index/extensions/bloom_filter.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <task_result.h>

#include <cstdint>

using namespace dftracer::utils;

using namespace dftracer::utils::index::extensions;

TEST_SUITE("BloomFilter") {
    TEST_CASE("BloomFilter - Add and query") {
        BloomFilter bf(100, 0.01);

        SUBCASE("Empty filter contains nothing") {
            CHECK_FALSE(bf.possibly_contains("hello"));
            CHECK_FALSE(bf.possibly_contains("world"));
            CHECK(bf.entry_count() == 0);
        }

        SUBCASE("Added elements are found (no false negatives)") {
            bf.add("hello");
            bf.add("world");
            bf.add("foo");
            bf.add("bar");

            CHECK(bf.possibly_contains("hello"));
            CHECK(bf.possibly_contains("world"));
            CHECK(bf.possibly_contains("foo"));
            CHECK(bf.possibly_contains("bar"));
            CHECK(bf.entry_count() == 4);
        }

        SUBCASE("Single entry") {
            bf.add("only_one");
            CHECK(bf.possibly_contains("only_one"));
            CHECK(bf.entry_count() == 1);
        }
    }

    TEST_CASE("BloomFilter - False positive rate") {
        const std::size_t N = 10000;
        BloomFilter bf(N, 0.01);

        // Insert N elements
        for (std::size_t i = 0; i < N; ++i) {
            bf.add("item_" + dstd::to_string(i));
        }

        // Verify no false negatives
        for (std::size_t i = 0; i < N; ++i) {
            REQUIRE(bf.possibly_contains("item_" + dstd::to_string(i)));
        }

        // Check false positive rate with elements we didn't add
        std::size_t false_positives = 0;
        const std::size_t test_count = 10000;
        for (std::size_t i = 0; i < test_count; ++i) {
            if (bf.possibly_contains("absent_" + dstd::to_string(i))) {
                false_positives++;
            }
        }

        double fp_rate = static_cast<double>(false_positives) /
                         static_cast<double>(test_count);
        // Allow some margin: target is 1%, accept up to 5%
        CHECK(fp_rate < 0.05);
    }

    TEST_CASE("BloomFilter - Serialize and deserialize round-trip") {
        BloomFilter bf(256, 0.01);

        bf.add("alpha");
        bf.add("beta");
        bf.add("gamma");

        auto blob = bf.serialize();
        CHECK(blob.size() > 12);  // At least header (12 bytes) + some bits

        auto bf2 = dftu_utils_test::ok(
            BloomFilter::from_blob(blob.data(), blob.size()));

        CHECK(bf2.possibly_contains("alpha"));
        CHECK(bf2.possibly_contains("beta"));
        CHECK(bf2.possibly_contains("gamma"));
        CHECK(bf2.entry_count() == 3);
        CHECK(bf2.hash_function_count() == bf.hash_function_count());
        CHECK(bf2.bit_count() == bf.bit_count());
    }

    TEST_CASE("BloomFilter - Merge correctness") {
        BloomFilter a(100, 0.01);
        BloomFilter b(100, 0.01);

        a.add("x1");
        a.add("x2");
        a.add("x3");

        b.add("y1");
        b.add("y2");

        a.merge_from(b);

        // Merged filter should contain everything from both
        CHECK(a.possibly_contains("x1"));
        CHECK(a.possibly_contains("x2"));
        CHECK(a.possibly_contains("x3"));
        CHECK(a.possibly_contains("y1"));
        CHECK(a.possibly_contains("y2"));
        CHECK(a.entry_count() == 5);
    }

    TEST_CASE("BloomFilter - Size parameters") {
        SUBCASE("Default parameters") {
            BloomFilter bf;
            CHECK(bf.bit_count() > 0);
            CHECK(bf.size_bytes() > 0);
            CHECK(bf.hash_function_count() > 0);
        }

        SUBCASE("Custom parameters") {
            BloomFilter bf(2048, 0.001);
            CHECK(bf.bit_count() > 0);
            CHECK(bf.hash_function_count() > 0);
        }
    }

    TEST_CASE("BloomFilter - from_blob error handling") {
        SUBCASE("Too small data") {
            unsigned char data[4] = {0};
            const auto blob = BloomFilter::from_blob(data, 4);
            REQUIRE_FALSE(blob.has_value());
            CHECK(blob.error().is_code(common::ErrorCode::Parse));
        }
    }

    TEST_CASE("BloomFilter - Merge incompatible filters") {
        BloomFilter a(100, 0.01);
        BloomFilter b(200,
                      0.01);  // Different expected entries = different size

        REQUIRE(a.size_bytes() != b.size_bytes());
        auto result = fresh_process::run(
            "merging incompatible bloom filters ends the process");
        CHECK(result.aborted_with("incompatible filter parameters"));
    }

    TEST_CASE("merging incompatible bloom filters ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        BloomFilter a(100, 0.01);
        BloomFilter b(200, 0.01);
        a.merge_from(b);
    }
}
