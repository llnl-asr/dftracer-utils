#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/hash/hash.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::hash;

TEST_CASE("StdHasherUtility - Basic functionality") {
    auto hasher = std::make_shared<StdHasherUtility>();

    SUBCASE("Hash empty string") {
        hasher->reset();
        hasher->update("");
        Hash result = hasher->get_hash();

        // After updating with empty string, should have non-zero hash
        CHECK(result.value != 0);
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

TEST_CASE("StdHasherUtility - Streaming hashing") {
    auto hasher = std::make_shared<StdHasherUtility>();

    SUBCASE("Chunk order matters") {
        hasher->reset();
        hasher->update("AB");
        hasher->update("CD");
        Hash hash1 = hasher->get_hash();

        hasher->reset();
        hasher->update("CD");
        hasher->update("AB");
        Hash hash2 = hasher->get_hash();

        CHECK(hash1 != hash2);
    }

    SUBCASE("Multiple updates") {
        hasher->reset();
        hasher->update("Hello");
        hasher->update("World");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
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

TEST_CASE("StdHasherUtility - process() interface") {
    auto hasher = std::make_shared<StdHasherUtility>();

    SUBCASE("process() returns hash") {
        hasher->reset();
        Hash result = hasher->process(std::string("test"));

        CHECK(result.value != 0);
        CHECK(result == hasher->get_hash());
    }

    SUBCASE("process() with POD types") {
        hasher->reset();
        int value = 42;
        Hash result = hasher->process(value);

        CHECK(result.value != 0);
    }

    SUBCASE("process() multiple values") {
        hasher->reset();
        int a = 1;
        int b = 2;
        int c = 3;
        Hash result = hasher->process(a, b, c);

        CHECK(result.value != 0);
    }
}

TEST_CASE("StdHasherUtility - Consistency") {
    auto hasher = std::make_shared<StdHasherUtility>();

    SUBCASE("Same input always produces same hash") {
        std::string test_input = "The quick brown fox jumps over the lazy dog";

        hasher->reset();
        hasher->update(test_input);
        Hash hash1 = hasher->get_hash();

        hasher->reset();
        hasher->update(test_input);
        Hash hash2 = hasher->get_hash();

        CHECK(hash1 == hash2);
    }

    SUBCASE("Repeated resets produce same result") {
        for (int i = 0; i < 10; ++i) {
            hasher->reset();
            hasher->update("test");
        }
        Hash first = hasher->get_hash();

        hasher->reset();
        hasher->update("test");
        Hash second = hasher->get_hash();

        CHECK(first == second);
    }
}
