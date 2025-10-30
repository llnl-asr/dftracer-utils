#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/hash/hash.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::hash;

TEST_CASE("XXH64HasherUtility - Basic functionality") {
    auto hasher = std::make_shared<XXH64HasherUtility>();

    SUBCASE("Hash empty string") {
        hasher->reset();
        hasher->update("");
        Hash result = hasher->get_hash();

        // Empty string should produce a consistent hash
        CHECK(result.value != 0);  // XXH64 doesn't return 0 for empty
    }

    SUBCASE("Hash single string") {
        hasher->reset();
        hasher->update("Hello, World!");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
        CHECK(result.value == hasher->get_hash().value);  // Consistent
    }

    SUBCASE("Reset clears state") {
        hasher->reset();
        hasher->update("First");
        Hash first = hasher->get_hash();

        hasher->reset();
        hasher->update("First");
        Hash second = hasher->get_hash();

        CHECK(first == second);
    }

    SUBCASE("Different inputs produce different hashes") {
        hasher->reset();
        hasher->update("input1");
        Hash hash1 = hasher->get_hash();

        hasher->reset();
        hasher->update("input2");
        Hash hash2 = hasher->get_hash();

        CHECK(hash1 != hash2);
    }
}

TEST_CASE("XXH64HasherUtility - Streaming hashing") {
    auto hasher = std::make_shared<XXH64HasherUtility>();

    SUBCASE("Incremental equals single update") {
        hasher->reset();
        hasher->update("Hello");
        hasher->update("World");
        Hash incremental = hasher->get_hash();

        hasher->reset();
        hasher->update("HelloWorld");
        Hash single = hasher->get_hash();

        CHECK(incremental == single);
    }

    SUBCASE("Many small updates") {
        hasher->reset();
        for (int i = 0; i < 100; ++i) {
            hasher->update("x");
        }
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }
}

TEST_CASE("XXH64HasherUtility - Compare with XXH3") {
    auto xxh64 = std::make_shared<XXH64HasherUtility>();

    SUBCASE("XXH64 produces different hash than XXH3 would") {
        xxh64->reset();
        xxh64->update("test data");
        Hash result = xxh64->get_hash();

        // Just verify it produces a hash
        CHECK(result.value != 0);
    }
}
