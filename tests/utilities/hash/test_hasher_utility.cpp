// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/fileio/hash/fnv1a_hasher_utility.h>
#include <dftracer/utils/fileio/hash/hasher_utility.h>
#include <dftracer/utils/fileio/hash/types.h>
#include <doctest/doctest.h>

using namespace dftracer::utils;

using namespace dftracer::utils::fileio::hash;

TEST_CASE("HasherUtility - Basic hashing") {
    HasherUtility hasher;
    hasher.reset();
    hasher.update("test data");
    CHECK(hasher.hash().value != 0);
}

TEST_CASE("HasherUtility - Streaming") {
    SUBCASE("Incremental hashing (FNV-1a is streaming)") {
        HasherUtility h1;
        h1.update("Hello");
        h1.update("World");
        Hash incremental = h1.hash();

        HasherUtility h2;
        h2.update("HelloWorld");
        CHECK(incremental == h2.hash());

        HasherUtility h3;
        h3.update("Hello");
        h3.update("World");
        CHECK(h3.hash() == incremental);
    }

    SUBCASE("Reset between operations") {
        HasherUtility hasher;
        hasher.reset();
        hasher.update("First");
        Hash first = hasher.hash();

        hasher.reset();
        hasher.update("First");
        CHECK(hasher.hash() == first);
    }
}

TEST_CASE("HasherUtility - process() interface") {
    HasherUtility hasher;

    SUBCASE("process() with string") {
        hasher.reset();
        Hash result = hasher.process(dstd::String("test"));
        CHECK(result.value != 0);
        CHECK(result == hasher.hash());
    }

    SUBCASE("process() with POD types") {
        hasher.reset();
        int value = 42;
        CHECK(hasher.process(value).value != 0);
    }

    SUBCASE("process() multiple values") {
        hasher.reset();
        CHECK(hasher.process(1, 2, 3).value != 0);
    }
}

TEST_CASE("HasherUtility - Consistency") {
    HasherUtility hasher;
    dstd::String test_data = "consistency test";

    hasher.reset();
    hasher.update(test_data);
    Hash hash1 = hasher.hash();

    hasher.reset();
    hasher.update(test_data);
    CHECK(hash1 == hasher.hash());
}

TEST_CASE("HasherUtility - Edge cases") {
    HasherUtility hasher;

    SUBCASE("Empty string") {
        hasher.reset();
        hasher.update("");
        CHECK(hasher.hash().value != 0);
    }

    SUBCASE("Large data") {
        dstd::String large_data(1024 * 1024, 'X');
        hasher.reset();
        hasher.update(large_data);
        CHECK(hasher.hash().value != 0);
    }

    SUBCASE("Binary data with null bytes") {
        dstd::String binary("\x00\x01\x02\x03", 4);
        hasher.reset();
        hasher.update(binary);
        CHECK(hasher.hash().value != 0);
    }
}
