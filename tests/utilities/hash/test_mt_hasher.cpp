#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/hash/hash.h>
#include <doctest/doctest.h>

#include <memory>
#include <thread>
#include <vector>

using namespace dftracer::utils::utilities::hash;

TEST_CASE("MTHasherUtility - Basic Operations") {
    SUBCASE("Construction and initialization") {
        auto hasher = std::make_shared<MTHasherUtility>();
        Hash initial = hasher->get_hash();
        CHECK(initial.value == 0);
    }

    SUBCASE("Reset functionality") {
        auto hasher = std::make_shared<MTHasherUtility>();
        hasher->update("test");
        Hash after_update = hasher->get_hash();
        CHECK(after_update.value != 0);

        hasher->reset();
        Hash after_reset = hasher->get_hash();
        CHECK(after_reset.value == 0);
    }

    SUBCASE("Algorithm switching") {
        auto hasher = std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        CHECK(hasher->get_algorithm() == HashAlgorithm::XXH3_64);

        hasher->set_algorithm(HashAlgorithm::XXH64);
        CHECK(hasher->get_algorithm() == HashAlgorithm::XXH64);

        hasher->set_algorithm(HashAlgorithm::STD);
        CHECK(hasher->get_algorithm() == HashAlgorithm::STD);
    }
}

TEST_CASE("MTHasherUtility - Hash Correctness") {
    SUBCASE("Single update matches direct hasher") {
        auto mt_hasher =
            std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        auto direct_hasher = std::make_shared<XXH3HasherUtility>();

        mt_hasher->reset();
        direct_hasher->reset();

        mt_hasher->update("test");
        direct_hasher->update("test");

        Hash mt_hash = mt_hasher->get_hash();
        Hash direct_hash = direct_hasher->get_hash();

        CHECK(mt_hash == direct_hash);
    }

    SUBCASE("Incremental hashing") {
        auto mt_hasher =
            std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        auto direct_hasher = std::make_shared<XXH3HasherUtility>();

        mt_hasher->reset();
        direct_hasher->reset();

        // Incremental updates
        mt_hasher->update("Hello");
        mt_hasher->update("World");

        direct_hasher->update("Hello");
        direct_hasher->update("World");

        Hash mt_hash = mt_hasher->get_hash();
        Hash direct_hash = direct_hasher->get_hash();

        CHECK(mt_hash == direct_hash);

        // Single update should match
        mt_hasher->reset();
        direct_hasher->reset();

        mt_hasher->update("HelloWorld");
        direct_hasher->update("HelloWorld");

        Hash mt_single = mt_hasher->get_hash();
        Hash direct_single = direct_hasher->get_hash();

        CHECK(mt_single == direct_single);
        CHECK(mt_hash == mt_single);
    }

    SUBCASE("Process method") {
        auto mt_hasher =
            std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        auto direct_hasher = std::make_shared<XXH3HasherUtility>();

        mt_hasher->reset();
        direct_hasher->reset();

        Hash mt_hash = mt_hasher->process("test data");
        Hash direct_hash = direct_hasher->process("test data");

        CHECK(mt_hash == direct_hash);
    }
}

TEST_CASE("MTHasherUtility - Thread Safety") {
    SUBCASE("Concurrent updates with coordination") {
        auto hasher = std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        hasher->reset();

        const int num_threads = 4;
        const int updates_per_thread = 100;
        std::vector<std::thread> threads;

        // Each thread updates with the same data
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([hasher, updates_per_thread]() {
                for (int i = 0; i < updates_per_thread; ++i) {
                    hasher->update("x");  // Single byte update
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        // Verify we got some hash (not checking specific value since order is
        // not guaranteed)
        Hash result = hasher->get_hash();
        CHECK(result.value != 0);

        // Compare with sequential updates
        auto sequential = std::make_shared<XXH3HasherUtility>();
        sequential->reset();
        for (int i = 0; i < num_threads * updates_per_thread; ++i) {
            sequential->update("x");
        }
        Hash sequential_hash = sequential->get_hash();

        // Note: Due to mutex locking, the MT version should produce the same
        // hash as sequential if all threads are doing the same updates
        CHECK(result == sequential_hash);
    }

    SUBCASE("Concurrent get_hash calls") {
        auto hasher = std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        hasher->reset();
        hasher->update("test data");

        const int num_threads = 10;
        std::vector<std::thread> threads;
        std::vector<Hash> results(num_threads);

        // Multiple threads reading hash simultaneously
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back(
                [hasher, &results, t]() { results[t] = hasher->get_hash(); });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        // All threads should read the same hash value
        Hash expected = results[0];
        for (int i = 1; i < num_threads; ++i) {
            CHECK(results[i] == expected);
        }
    }

    SUBCASE("Concurrent reset and update") {
        auto hasher = std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);

        const int num_iterations = 50;
        std::vector<std::thread> threads;

        // One thread resets, another updates
        threads.emplace_back([hasher, num_iterations]() {
            for (int i = 0; i < num_iterations; ++i) {
                hasher->reset();
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });

        threads.emplace_back([hasher, num_iterations]() {
            for (int i = 0; i < num_iterations; ++i) {
                hasher->update("data");
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });

        for (auto& thread : threads) {
            thread.join();
        }

        // Should not crash - that's the main test
        // Hash value is indeterminate due to race, but should be valid
        CHECK(true);  // If we got here, thread safety worked
    }

    SUBCASE("Concurrent algorithm switching") {
        auto hasher = std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);

        std::vector<std::thread> threads;

        // Each thread switches algorithm
        threads.emplace_back([hasher]() {
            for (int i = 0; i < 20; ++i) {
                hasher->set_algorithm(HashAlgorithm::XXH3_64);
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });

        threads.emplace_back([hasher]() {
            for (int i = 0; i < 20; ++i) {
                hasher->set_algorithm(HashAlgorithm::XXH64);
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });

        threads.emplace_back([hasher]() {
            for (int i = 0; i < 20; ++i) {
                hasher->set_algorithm(HashAlgorithm::STD);
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });

        for (auto& thread : threads) {
            thread.join();
        }

        // Should not crash
        HashAlgorithm final_algo = hasher->get_algorithm();
        CHECK((final_algo == HashAlgorithm::XXH3_64 ||
               final_algo == HashAlgorithm::XXH64 ||
               final_algo == HashAlgorithm::STD));
    }
}

TEST_CASE("MTHasherUtility - Different Algorithms") {
    SUBCASE("XXH3_64 algorithm") {
        auto mt_hasher =
            std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        mt_hasher->reset();
        mt_hasher->update("test");
        Hash result = mt_hasher->get_hash();
        CHECK(result.value != 0);
    }

    SUBCASE("XXH64 algorithm") {
        auto mt_hasher =
            std::make_shared<MTHasherUtility>(HashAlgorithm::XXH64);
        mt_hasher->reset();
        mt_hasher->update("test");
        Hash result = mt_hasher->get_hash();
        CHECK(result.value != 0);
    }

    SUBCASE("STD algorithm") {
        auto mt_hasher = std::make_shared<MTHasherUtility>(HashAlgorithm::STD);
        mt_hasher->reset();
        mt_hasher->update("test");
        Hash result = mt_hasher->get_hash();
        CHECK(result.value != 0);
    }

    SUBCASE("Different algorithms produce different hashes") {
        std::string test_data = "test data for hashing";

        auto xxh3_hasher =
            std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        xxh3_hasher->reset();
        xxh3_hasher->update(test_data);
        Hash xxh3_hash = xxh3_hasher->get_hash();

        auto xxh64_hasher =
            std::make_shared<MTHasherUtility>(HashAlgorithm::XXH64);
        xxh64_hasher->reset();
        xxh64_hasher->update(test_data);
        Hash xxh64_hash = xxh64_hasher->get_hash();

        auto std_hasher = std::make_shared<MTHasherUtility>(HashAlgorithm::STD);
        std_hasher->reset();
        std_hasher->update(test_data);
        Hash std_hash = std_hasher->get_hash();

        // Different algorithms should produce different hashes (very likely)
        CHECK(xxh3_hash.value != xxh64_hash.value);
        CHECK(xxh3_hash.value != std_hash.value);
        CHECK(xxh64_hash.value != std_hash.value);
    }
}

TEST_CASE("MTHasherUtility - Edge Cases") {
    SUBCASE("Empty string") {
        auto hasher = std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        hasher->reset();
        hasher->update("");
        Hash result = hasher->get_hash();
        CHECK(result.value != 0);  // Empty string still has a hash
    }

    SUBCASE("Large data") {
        auto hasher = std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        hasher->reset();

        std::string large_data(1024 * 1024, 'x');  // 1MB of 'x'
        hasher->update(large_data);
        Hash result = hasher->get_hash();
        CHECK(result.value != 0);
    }

    SUBCASE("Many small updates") {
        auto hasher = std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        hasher->reset();

        for (int i = 0; i < 1000; ++i) {
            hasher->update("a");
        }
        Hash result = hasher->get_hash();
        CHECK(result.value != 0);
    }

    SUBCASE("Binary data") {
        auto hasher = std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
        hasher->reset();

        std::vector<unsigned char> binary_data = {0x00, 0x01, 0x02, 0xFF, 0xFE};
        std::string_view binary_view(
            reinterpret_cast<const char*>(binary_data.data()),
            binary_data.size());
        hasher->update(binary_view);
        Hash result = hasher->get_hash();
        CHECK(result.value != 0);
    }
}
