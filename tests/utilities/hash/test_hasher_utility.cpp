#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/hash/hash.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::hash;

TEST_CASE("HasherUtility - Algorithm selection") {
    SUBCASE("Default algorithm is FNV1A_64") {
        auto hasher = std::make_shared<HasherUtility>();
        CHECK(hasher->get_algorithm() == HashAlgorithm::FNV1A_64);
    }

    SUBCASE("Construct with STD") {
        auto hasher = std::make_shared<HasherUtility>(HashAlgorithm::STD);
        CHECK(hasher->get_algorithm() == HashAlgorithm::STD);
    }

    SUBCASE("Construct with FNV1A_64") {
        auto hasher = std::make_shared<HasherUtility>(HashAlgorithm::FNV1A_64);
        CHECK(hasher->get_algorithm() == HashAlgorithm::FNV1A_64);
    }
}

TEST_CASE("HasherUtility - Basic hashing") {
    auto hasher = std::make_shared<HasherUtility>();

    SUBCASE("Hash with STD") {
        hasher->set_algorithm(HashAlgorithm::STD);
        hasher->reset();
        hasher->update("test data");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }

    SUBCASE("Hash with FNV1A_64") {
        hasher->set_algorithm(HashAlgorithm::FNV1A_64);
        hasher->reset();
        hasher->update("test data");
        Hash result = hasher->get_hash();

        CHECK(result.value != 0);
    }
}

TEST_CASE("HasherUtility - Streaming") {
    SUBCASE("Simple single update test") {
        auto direct = std::make_shared<StdHasherUtility>();
        direct->reset();
        direct->update("test");
        Hash direct_hash = direct->get_hash();

        auto wrapped = std::make_shared<HasherUtility>(HashAlgorithm::STD);
        wrapped->reset();
        wrapped->update("test");
        Hash wrapped_hash = wrapped->get_hash();

        CHECK(direct_hash == wrapped_hash);
    }

    SUBCASE("Incremental hashing (FNV-1a is streaming)") {
        auto h1 = std::make_shared<HasherUtility>();
        h1->update("Hello");
        h1->update("World");
        Hash incremental = h1->get_hash();

        auto h2 = std::make_shared<HasherUtility>();
        h2->update("HelloWorld");
        Hash single = h2->get_hash();

        CHECK(incremental == single);

        // Also verify determinism
        auto h3 = std::make_shared<HasherUtility>();
        h3->update("Hello");
        h3->update("World");
        CHECK(h3->get_hash() == incremental);
    }

    SUBCASE(
        "STD incremental != single-pass (hash_combine "
        "is not byte-streaming)") {
        auto h1 = std::make_shared<StdHasherUtility>();
        h1->update("Hello");
        h1->update("World");
        Hash hash1 = h1->get_hash();

        auto h2 = std::make_shared<StdHasherUtility>();
        h2->update("HelloWorld");
        Hash single = h2->get_hash();

        CHECK(hash1 != single);
    }

    SUBCASE("Reset between operations") {
        auto hasher = std::make_shared<HasherUtility>(HashAlgorithm::FNV1A_64);
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
    auto hasher = std::make_shared<HasherUtility>(HashAlgorithm::FNV1A_64);

    SUBCASE("process() with string") {
        hasher->reset();
        Hash result = hasher->process(std::string("test")).get();

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

TEST_CASE("HasherUtility - Consistency") {
    auto hasher = std::make_shared<HasherUtility>();

    SUBCASE("Same data produces consistent hash") {
        std::string test_data = "consistency test";

        hasher->set_algorithm(HashAlgorithm::FNV1A_64);
        hasher->reset();
        hasher->update(test_data);
        Hash hash1 = hasher->get_hash();

        hasher->reset();
        hasher->update(test_data);
        Hash hash2 = hasher->get_hash();

        CHECK(hash1 == hash2);
    }

    SUBCASE(
        "Same data produces consistent hash with "
        "STD") {
        std::string test_data = "consistency test";

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
