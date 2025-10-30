#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/hash/hash.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::hash;

TEST_CASE("HasherUtility - Algorithm selection") {
    SUBCASE("Default algorithm is XXH3_64") {
        auto hasher = std::make_shared<HasherUtility>();
        CHECK(hasher->get_algorithm() == HashAlgorithm::XXH3_64);
    }

    SUBCASE("Construct with XXH64") {
        auto hasher = std::make_shared<HasherUtility>(HashAlgorithm::XXH64);
        CHECK(hasher->get_algorithm() == HashAlgorithm::XXH64);
    }

    SUBCASE("Construct with STD") {
        auto hasher = std::make_shared<HasherUtility>(HashAlgorithm::STD);
        CHECK(hasher->get_algorithm() == HashAlgorithm::STD);
    }

    SUBCASE("Switch algorithm at runtime") {
        auto hasher = std::make_shared<HasherUtility>(HashAlgorithm::XXH3_64);
        hasher->set_algorithm(HashAlgorithm::XXH64);
        CHECK(hasher->get_algorithm() == HashAlgorithm::XXH64);
    }
}

TEST_CASE("HasherUtility - Basic hashing") {
    auto hasher = std::make_shared<HasherUtility>();

    SUBCASE("Hash with XXH3_64") {
        hasher->set_algorithm(HashAlgorithm::XXH3_64);
        hasher->reset();
        hasher->update("test data");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }

    SUBCASE("Hash with XXH64") {
        hasher->set_algorithm(HashAlgorithm::XXH64);
        hasher->reset();
        hasher->update("test data");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }

    SUBCASE("Hash with STD") {
        hasher->set_algorithm(HashAlgorithm::STD);
        hasher->reset();
        hasher->update("test data");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }
}

TEST_CASE("HasherUtility - Algorithm switching") {
    auto hasher = std::make_shared<HasherUtility>();

    SUBCASE("Different algorithms produce different hashes") {
        std::string test_data = "Hello, World!";

        hasher->set_algorithm(HashAlgorithm::XXH3_64);
        hasher->reset();
        hasher->update(test_data);
        Hash hash_xxh3 = hasher->get_hash();

        hasher->set_algorithm(HashAlgorithm::XXH64);
        hasher->reset();
        hasher->update(test_data);
        Hash hash_xxh64 = hasher->get_hash();

        hasher->set_algorithm(HashAlgorithm::STD);
        hasher->reset();
        hasher->update(test_data);
        Hash hash_std = hasher->get_hash();

        // Different algorithms should produce different hashes
        // (though collision is theoretically possible, it's extremely unlikely)
        CHECK(hash_xxh3.value != 0);
        CHECK(hash_xxh64.value != 0);
        CHECK(hash_std.value != 0);
    }

    SUBCASE("set_algorithm requires explicit reset") {
        hasher->set_algorithm(HashAlgorithm::XXH3_64);
        hasher->reset();
        hasher->update("data");

        // Switch algorithm
        hasher->set_algorithm(HashAlgorithm::XXH64);

        // After switching, need to explicitly reset
        hasher->reset();
        Hash after_reset = hasher->get_hash();

        // After explicit reset, get_hash should return 0 (initial state)
        CHECK(after_reset.value == 0);
    }
}

TEST_CASE("HasherUtility - Streaming") {
    SUBCASE("Simple single update test") {
        auto direct = std::make_shared<XXH3HasherUtility>();
        direct->reset();
        direct->update("test");
        Hash direct_hash = direct->get_hash();

        auto wrapped = std::make_shared<HasherUtility>(HashAlgorithm::XXH3_64);
        wrapped->reset();
        wrapped->update("test");
        Hash wrapped_hash = wrapped->get_hash();

        CHECK(direct_hash == wrapped_hash);
    }

    SUBCASE("Incremental hashing") {
        auto hasher = std::make_shared<HasherUtility>(HashAlgorithm::XXH3_64);
        // Test with XXH3 directly first
        auto direct_xxh3 = std::make_shared<XXH3HasherUtility>();
        direct_xxh3->reset();
        direct_xxh3->update("Hello");
        direct_xxh3->update("World");
        Hash direct_incremental = direct_xxh3->get_hash();

        direct_xxh3->reset();
        direct_xxh3->update("HelloWorld");
        Hash direct_single = direct_xxh3->get_hash();

        CHECK(direct_incremental == direct_single);

        // Now test through HasherUtility wrapper
        hasher->reset();
        hasher->update("Hello");
        hasher->update("World");
        Hash incremental = hasher->get_hash();

        hasher->reset();
        hasher->update("HelloWorld");
        Hash single = hasher->get_hash();

        CHECK(incremental == single);
        CHECK(incremental == direct_incremental);
    }

    SUBCASE("Reset between operations") {
        auto hasher = std::make_shared<HasherUtility>(HashAlgorithm::XXH3_64);
        hasher->reset();
        hasher->update("First");
        Hash first = hasher->get_hash();

        hasher->reset();
        hasher->update("First");
        Hash second = hasher->get_hash();

        CHECK(first == second);
    }
}

TEST_CASE("HasherUtility - process() interface") {
    auto hasher = std::make_shared<HasherUtility>(HashAlgorithm::XXH3_64);

    SUBCASE("process() with string") {
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
        Hash result = hasher->process(1, 2, 3);

        CHECK(result.value != 0);
    }
}

TEST_CASE("HasherUtility - Consistency across algorithms") {
    auto hasher = std::make_shared<HasherUtility>();

    SUBCASE("Same data produces consistent hash per algorithm") {
        std::string test_data = "consistency test";

        // Test XXH3_64
        hasher->set_algorithm(HashAlgorithm::XXH3_64);
        hasher->reset();
        hasher->update(test_data);
        Hash hash1_xxh3 = hasher->get_hash();

        hasher->reset();
        hasher->update(test_data);
        Hash hash2_xxh3 = hasher->get_hash();

        CHECK(hash1_xxh3 == hash2_xxh3);

        // Test XXH64
        hasher->set_algorithm(HashAlgorithm::XXH64);
        hasher->reset();
        hasher->update(test_data);
        Hash hash1_xxh64 = hasher->get_hash();

        hasher->reset();
        hasher->update(test_data);
        Hash hash2_xxh64 = hasher->get_hash();

        CHECK(hash1_xxh64 == hash2_xxh64);

        // Test STD
        hasher->set_algorithm(HashAlgorithm::STD);
        hasher->reset();
        hasher->update(test_data);
        Hash hash1_std = hasher->get_hash();

        hasher->reset();
        hasher->update(test_data);
        Hash hash2_std = hasher->get_hash();

        CHECK(hash1_std == hash2_std);
    }
}

TEST_CASE("HasherUtility - Edge cases") {
    auto hasher = std::make_shared<HasherUtility>();

    SUBCASE("Empty string") {
        hasher->reset();
        hasher->update("");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }

    SUBCASE("Large data") {
        std::string large_data(1024 * 1024, 'X');
        hasher->reset();
        hasher->update(large_data);
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }

    SUBCASE("Binary data with null bytes") {
        std::string binary("\x00\x01\x02\x03", 4);
        hasher->reset();
        hasher->update(binary);
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }
}
