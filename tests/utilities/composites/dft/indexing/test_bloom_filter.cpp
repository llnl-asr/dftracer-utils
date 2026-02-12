#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <set>
#include <string>

using namespace dftracer::utils::utilities::composites::dft::indexing;

TEST_SUITE("BloomFilter") {
    TEST_CASE("BloomFilter - Add and query") {
        BloomFilter bf(100, 0.01);

        SUBCASE("Empty filter contains nothing") {
            CHECK_FALSE(bf.possibly_contains("hello"));
            CHECK_FALSE(bf.possibly_contains("world"));
            CHECK(bf.num_entries() == 0);
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
            CHECK(bf.num_entries() == 4);
        }

        SUBCASE("Single entry") {
            bf.add("only_one");
            CHECK(bf.possibly_contains("only_one"));
            CHECK(bf.num_entries() == 1);
        }
    }

    TEST_CASE("BloomFilter - False positive rate") {
        const std::size_t N = 10000;
        BloomFilter bf(N, 0.01);

        // Insert N elements
        for (std::size_t i = 0; i < N; ++i) {
            bf.add("item_" + std::to_string(i));
        }

        // Verify no false negatives
        for (std::size_t i = 0; i < N; ++i) {
            REQUIRE(bf.possibly_contains("item_" + std::to_string(i)));
        }

        // Check false positive rate with elements we didn't add
        std::size_t false_positives = 0;
        const std::size_t test_count = 10000;
        for (std::size_t i = 0; i < test_count; ++i) {
            if (bf.possibly_contains("absent_" + std::to_string(i))) {
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

        auto bf2 = BloomFilter::from_blob(blob.data(), blob.size());

        CHECK(bf2.possibly_contains("alpha"));
        CHECK(bf2.possibly_contains("beta"));
        CHECK(bf2.possibly_contains("gamma"));
        CHECK(bf2.num_entries() == 3);
        CHECK(bf2.num_hash_functions() == bf.num_hash_functions());
        CHECK(bf2.num_bits() == bf.num_bits());
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
        CHECK(a.num_entries() == 5);
    }

    TEST_CASE("BloomFilter - Size parameters") {
        SUBCASE("Default parameters") {
            BloomFilter bf;
            CHECK(bf.num_bits() > 0);
            CHECK(bf.size_bytes() > 0);
            CHECK(bf.num_hash_functions() > 0);
        }

        SUBCASE("Custom parameters") {
            BloomFilter bf(2048, 0.001);
            CHECK(bf.num_bits() > 0);
            CHECK(bf.num_hash_functions() > 0);
        }
    }

    TEST_CASE("BloomFilter - from_blob error handling") {
        SUBCASE("Too small data") {
            unsigned char data[4] = {0};
            CHECK_THROWS(BloomFilter::from_blob(data, 4));
        }
    }

    TEST_CASE("BloomFilter - Merge incompatible filters") {
        BloomFilter a(100, 0.01);
        BloomFilter b(200,
                      0.01);  // Different expected entries = different size

        // Should throw since bit arrays have different sizes
        if (a.size_bytes() != b.size_bytes()) {
            CHECK_THROWS(a.merge_from(b));
        }
    }
}
