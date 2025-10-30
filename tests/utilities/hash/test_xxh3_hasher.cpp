#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/hash/hash.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::hash;

TEST_CASE("XXH3HasherUtility - Basic functionality") {
    auto hasher = std::make_shared<XXH3HasherUtility>();

    SUBCASE("Hash empty string") {
        hasher->reset();
        hasher->update("");
        Hash result = hasher->get_hash();

        // Empty string should produce a consistent hash
        CHECK(result.value != 0);  // XXH3 doesn't return 0 for empty
    }

    SUBCASE("Hash single string") {
        hasher->reset();
        hasher->update("Hello, World!");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
        CHECK(result.value == hasher->get_hash().value);  // Consistent
    }

    SUBCASE("Hash multiple chunks - incremental") {
        hasher->reset();
        hasher->update("Hello, ");
        hasher->update("World!");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
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

TEST_CASE("XXH3HasherUtility - Streaming hashing") {
    auto hasher = std::make_shared<XXH3HasherUtility>();

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

TEST_CASE("XXH3HasherUtility - process() interface") {
    auto hasher = std::make_shared<XXH3HasherUtility>();

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

TEST_CASE("XXH3HasherUtility - Large data") {
    auto hasher = std::make_shared<XXH3HasherUtility>();

    SUBCASE("Hash large string") {
        std::string large_data(1024 * 1024, 'A');  // 1MB
        hasher->reset();
        hasher->update(large_data);
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }

    SUBCASE("Hash in chunks") {
        std::string chunk(1024, 'B');
        hasher->reset();
        for (int i = 0; i < 1024; ++i) {
            hasher->update(chunk);
        }
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }
}

TEST_CASE("XXH3HasherUtility - Known values") {
    auto hasher = std::make_shared<XXH3HasherUtility>();

    SUBCASE("Empty string has consistent hash") {
        hasher->reset();
        hasher->update("");
        Hash hash1 = hasher->get_hash();

        hasher->reset();
        hasher->update("");
        Hash hash2 = hasher->get_hash();

        CHECK(hash1 == hash2);
    }

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
}

TEST_CASE("XXH3HasherUtility - Edge cases") {
    auto hasher = std::make_shared<XXH3HasherUtility>();

    SUBCASE("Single byte") {
        hasher->reset();
        hasher->update("A");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }

    SUBCASE("Binary data with null bytes") {
        hasher->reset();
        std::string data("\x00\x01\x02\x03", 4);
        hasher->update(data);
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }

    SUBCASE("Unicode characters") {
        hasher->reset();
        hasher->update("Hello 世界 🌍");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }
}
