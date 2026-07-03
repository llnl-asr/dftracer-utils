// Suppress GCC 14.3.0 false positive warnings
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/utilities/composites/chunk_verifier_utility.h>
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <numeric>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::behaviors;

namespace tags = dftracer::utils::utilities::tags;

// Test data structures
struct TestChunk {
    std::size_t id;
    std::vector<int> data;

    TestChunk() : id(0) {}
    TestChunk(std::size_t i, std::vector<int> d) : id(i), data(std::move(d)) {}
};

struct TestMetadata {
    std::string name;
    std::size_t total_events;

    TestMetadata() : name(""), total_events(0) {}
    TestMetadata(const std::string& n, std::size_t t)
        : name(n), total_events(t) {}
};

using TestEvent = int;

// Helper: run a ChunkVerifierUtility via Runtime + run_coro_scope.
template <typename ChunkType, typename MetadataType, typename EventType>
static ChunkVerificationUtilityOutput run_verifier(
    std::shared_ptr<ChunkVerifierUtility<ChunkType, MetadataType, EventType>>
        verifier,
    const ChunkVerificationUtilityInput<ChunkType, MetadataType>& input,
    std::size_t threads = 4) {
    Runtime rt(threads);
    ChunkVerificationUtilityOutput output;
    auto* out_ptr = &output;

    auto task = run_coro_scope(
        rt.executor(),
        [verifier, input, out_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            UtilityExecutor<
                ChunkVerificationUtilityInput<ChunkType, MetadataType>,
                ChunkVerificationUtilityOutput, tags::NeedsContext>
                exec(verifier);
            *out_ptr = co_await exec.execute(scope, input);
        });

    rt.submit(std::move(task), "chunk-verify").wait();
    rt.shutdown();
    return output;
}

TEST_SUITE("ChunkVerifier") {
    TEST_CASE("ChunkVerifier - Basic Verification") {
        SUBCASE("Verify matching chunks") {
            printf("Starting test: Verify matching chunks\n");

            auto input_hasher =
                [](const std::vector<TestMetadata>& metadata) -> std::uint64_t {
                std::uint64_t hash = 0;
                for (const auto& meta : metadata) {
                    hash ^= std::hash<std::string>{}(meta.name);
                    hash ^= std::hash<std::size_t>{}(meta.total_events);
                }
                printf("Input hasher calculated hash: %lu\n",
                       (unsigned long)hash);
                return hash;
            };

            auto event_collector =
                [](CoroScope&,
                   const TestChunk& chunk) -> std::vector<TestEvent> {
                printf("Collecting events from chunk %zu\n", chunk.id);
                return chunk.data;
            };

            auto event_hasher =
                [](const std::vector<TestEvent>& /*events*/) -> std::uint64_t {
                std::uint64_t hash = 0;
                hash ^= std::hash<std::string>{}("test");
                hash ^= std::hash<std::size_t>{}(9);  // total events
                printf("Event hasher calculated hash: %lu\n",
                       (unsigned long)hash);
                return hash;
            };

            printf("Creating verifier...\n");
            auto verifier = std::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            std::vector<TestChunk> chunks = {TestChunk(1, {1, 2, 3}),
                                             TestChunk(2, {4, 5, 6}),
                                             TestChunk(3, {7, 8, 9})};
            std::vector<TestMetadata> metadata = {TestMetadata("test", 9)};
            ChunkVerificationUtilityInput<TestChunk, TestMetadata> input(
                chunks, metadata);

            printf("Running verifier...\n");
            auto result = run_verifier(verifier, input, 4);

            printf("Checking results...\n");
            CHECK(result.passed == true);
            CHECK(result.input_hash == result.output_hash);

            printf("Test completed successfully\n");
        }

        SUBCASE("Detect mismatched chunks") {
            auto input_hasher =
                [](const std::vector<TestMetadata>& metadata) -> std::uint64_t {
                (void)metadata;
                return 12345;
            };

            auto event_collector = [](CoroScope&, const TestChunk& chunk)
                -> std::vector<TestEvent> { return chunk.data; };

            auto event_hasher =
                [](const std::vector<TestEvent>& events) -> std::uint64_t {
                (void)events;
                return 67890;
            };

            auto verifier = std::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            std::vector<TestChunk> chunks = {TestChunk(1, {1, 2, 3})};
            std::vector<TestMetadata> metadata = {TestMetadata("test", 3)};
            ChunkVerificationUtilityInput<TestChunk, TestMetadata> input(
                chunks, metadata);

            auto result = run_verifier(verifier, input, 4);

            CHECK(result.passed == false);
            CHECK(result.input_hash != result.output_hash);
            CHECK(result.error_message.find("Hash mismatch") !=
                  std::string::npos);
        }
    }

    TEST_CASE("ChunkVerifier - Parallel Processing") {
        SUBCASE("Process multiple chunks in parallel") {
            auto input_hasher =
                [](const std::vector<TestMetadata>& metadata) -> std::uint64_t {
                (void)metadata;
                return 435;  // Expected sum of all event values
            };

            auto event_collector =
                [](CoroScope&,
                   const TestChunk& chunk) -> std::vector<TestEvent> {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return chunk.data;
            };

            auto event_hasher =
                [](const std::vector<TestEvent>& events) -> std::uint64_t {
                std::uint64_t hash = 0;
                for (const auto& event : events) {
                    hash += event;
                }
                return hash;
            };

            auto verifier = std::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            std::vector<TestChunk> chunks;
            std::vector<TestEvent> all_events;
            for (int i = 0; i < 10; ++i) {
                std::vector<int> data = {i * 3, i * 3 + 1, i * 3 + 2};
                chunks.emplace_back(i, data);
                all_events.insert(all_events.end(), data.begin(), data.end());
            }

            std::vector<TestMetadata> metadata = {
                TestMetadata("parallel_test", all_events.size())};
            ChunkVerificationUtilityInput<TestChunk, TestMetadata> input(
                chunks, metadata);

            auto result = run_verifier(verifier, input, 4);

            CHECK(result.passed == true);
            CHECK(result.input_hash == 435);
            CHECK(result.output_hash ==
                  std::accumulate(all_events.begin(), all_events.end(), 0ULL));
        }
    }

    TEST_CASE("ChunkVerifier - Empty and Edge Cases") {
        SUBCASE("Empty chunks") {
            auto input_hasher =
                [](const std::vector<TestMetadata>&) -> std::uint64_t {
                return 0;
            };

            auto event_collector =
                [](CoroScope&, const TestChunk&) -> std::vector<TestEvent> {
                return {};
            };

            auto event_hasher =
                [](const std::vector<TestEvent>&) -> std::uint64_t {
                return 0;
            };

            auto verifier = std::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            std::vector<TestChunk> chunks;
            std::vector<TestMetadata> metadata;
            ChunkVerificationUtilityInput<TestChunk, TestMetadata> input(
                chunks, metadata);

            auto result = run_verifier(verifier, input, 2);

            CHECK(result.passed == true);
            CHECK(result.input_hash == 0);
            CHECK(result.output_hash == 0);
        }

        SUBCASE("Single chunk") {
            printf("Starting single chunk test\n");

            auto input_hasher =
                [](const std::vector<TestMetadata>& metadata) -> std::uint64_t {
                auto hash = metadata.empty() ? 0 : metadata[0].total_events;
                printf("Input hash: %lu\n", (unsigned long)hash);
                return hash;
            };

            auto event_collector =
                [](CoroScope& ctx,
                   const TestChunk& chunk) -> std::vector<TestEvent> {
                (void)ctx;
                printf("Collecting from chunk %zu\n", chunk.id);
                return chunk.data;
            };

            auto event_hasher =
                [](const std::vector<TestEvent>& events) -> std::uint64_t {
                auto hash = events.size();
                printf("Event hash: %zu (from %zu events)\n", hash,
                       events.size());
                return hash;
            };

            printf("Creating verifier\n");
            auto verifier = std::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            std::vector<TestChunk> chunks = {TestChunk(1, {10, 20, 30})};
            std::vector<TestMetadata> metadata = {TestMetadata("single", 3)};
            ChunkVerificationUtilityInput<TestChunk, TestMetadata> input(
                chunks, metadata);

            printf("Running verifier\n");
            auto result = run_verifier(verifier, input, 2);

            printf("Checking result\n");
            CHECK(result.passed == true);
            CHECK(result.input_hash == 3);
            CHECK(result.output_hash == 3);

            printf("Single chunk test completed\n");
        }
    }

    TEST_CASE("ChunkVerifier - Builder Pattern") {
        SUBCASE("Using from_chunks builder") {
            auto input_hasher =
                [](const std::vector<TestMetadata>&) -> std::uint64_t {
                return 100;
            };

            auto event_collector = [](CoroScope&, const TestChunk& chunk)
                -> std::vector<TestEvent> { return chunk.data; };

            auto event_hasher =
                [](const std::vector<TestEvent>&) -> std::uint64_t {
                return 100;  // Match input hash
            };

            auto verifier = std::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            std::vector<TestChunk> chunks = {TestChunk(1, {1, 2}),
                                             TestChunk(2, {3, 4})};

            auto input =
                ChunkVerificationUtilityInput<TestChunk,
                                              TestMetadata>::from_chunks(chunks)
                    .with_metadata({TestMetadata("built", 4)});

            auto result = run_verifier(verifier, input, 2);

            CHECK(result.passed == true);
        }
    }
}
