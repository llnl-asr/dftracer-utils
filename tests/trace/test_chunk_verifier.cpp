// SPDX-License-Identifier: MIT
// Suppress GCC 14.3.0 false positive warnings
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/hasher.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/chunk_verifier_utility.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <numeric>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::trace;
// Test data structures
struct TestChunk {
    std::size_t id;
    dstd::Vector<int> data;

    TestChunk() : id(0) {}
    TestChunk(std::size_t i, dstd::Vector<int> d) : id(i), data(std::move(d)) {}
};

struct TestMetadata {
    dstd::String name;
    std::size_t total_events;

    TestMetadata() : name(""), total_events(0) {}
    TestMetadata(const dstd::String& n, std::size_t t)
        : name(n), total_events(t) {}
};

using TestEvent = int;

// Helper: run a ChunkVerifierUtility via Runtime + coro::with_scope.
template <typename ChunkType, typename MetadataType, typename EventType>
static ChunkVerificationUtilityOutput run_verifier(
    dstd::SharedPtr<ChunkVerifierUtility<ChunkType, MetadataType, EventType>>
        verifier,
    const ChunkVerificationUtilityInput<ChunkType, MetadataType>& input,
    std::size_t threads = 4) {
    Runtime rt(threads);
    ChunkVerificationUtilityOutput output;
    auto* out_ptr = &output;

    auto task = coro::with_scope(
        [verifier, input, out_ptr](coro::Scope& scope) -> coro::CoroTask<void> {
            *out_ptr = co_await (*verifier)(scope, input);
        });

    dftu_utils_test::ok(rt.submit(std::move(task), "chunk-verify").wait());
    rt.shutdown();
    return output;
}

TEST_SUITE("ChunkVerifier") {
    TEST_CASE("ChunkVerifier - Basic Verification") {
        SUBCASE("Verify matching chunks") {
            printf("Starting test: Verify matching chunks\n");

            auto input_hasher = [](const dstd::Vector<TestMetadata>& metadata)
                -> std::uint64_t {
                std::uint64_t hash = 0;
                for (const auto& meta : metadata) {
                    hash ^= dstd::Hash<dstd::String>{}(meta.name);
                    hash ^= dstd::Hash<std::size_t>{}(meta.total_events);
                }
                printf("Input hasher calculated hash: %lu\n",
                       (unsigned long)hash);
                return hash;
            };

            auto event_collector =
                [](coro::Scope&,
                   const TestChunk& chunk) -> dstd::Vector<TestEvent> {
                printf("Collecting events from chunk %zu\n", chunk.id);
                return chunk.data;
            };

            auto event_hasher =
                [](const dstd::Vector<TestEvent>& /*events*/) -> std::uint64_t {
                std::uint64_t hash = 0;
                hash ^= dstd::Hash<dstd::String>{}("test");
                hash ^= dstd::Hash<std::size_t>{}(9);  // total events
                printf("Event hasher calculated hash: %lu\n",
                       (unsigned long)hash);
                return hash;
            };

            printf("Creating verifier...\n");
            auto verifier = dstd::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            dstd::Vector<TestChunk> chunks = {TestChunk(1, {1, 2, 3}),
                                              TestChunk(2, {4, 5, 6}),
                                              TestChunk(3, {7, 8, 9})};
            dstd::Vector<TestMetadata> metadata = {TestMetadata("test", 9)};
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
                [](const dstd::Vector<TestMetadata>&) -> std::uint64_t {
                return 12345;
            };

            auto event_collector = [](coro::Scope&, const TestChunk& chunk)
                -> dstd::Vector<TestEvent> { return chunk.data; };

            auto event_hasher =
                [](const dstd::Vector<TestEvent>&) -> std::uint64_t {
                return 67890;
            };

            auto verifier = dstd::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            dstd::Vector<TestChunk> chunks = {TestChunk(1, {1, 2, 3})};
            dstd::Vector<TestMetadata> metadata = {TestMetadata("test", 3)};
            ChunkVerificationUtilityInput<TestChunk, TestMetadata> input(
                chunks, metadata);

            auto result = run_verifier(verifier, input, 4);

            CHECK(result.passed == false);
            CHECK(result.input_hash != result.output_hash);
            CHECK(result.error_message.find("Hash mismatch") !=
                  dstd::String::npos);
        }
    }

    TEST_CASE("ChunkVerifier - Parallel Processing") {
        SUBCASE("Process multiple chunks in parallel") {
            auto input_hasher =
                [](const dstd::Vector<TestMetadata>&) -> std::uint64_t {
                return 435;  // Expected sum of all event values
            };

            auto event_collector =
                [](coro::Scope&,
                   const TestChunk& chunk) -> dstd::Vector<TestEvent> {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return chunk.data;
            };

            auto event_hasher =
                [](const dstd::Vector<TestEvent>& events) -> std::uint64_t {
                std::uint64_t hash = 0;
                for (const auto& event : events) {
                    hash += event;
                }
                return hash;
            };

            auto verifier = dstd::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            dstd::Vector<TestChunk> chunks;
            dstd::Vector<TestEvent> all_events;
            for (int i = 0; i < 10; ++i) {
                dstd::Vector<int> data = {i * 3, i * 3 + 1, i * 3 + 2};
                chunks.emplace_back(i, data);
                all_events.insert(all_events.end(), data.begin(), data.end());
            }

            dstd::Vector<TestMetadata> metadata = {
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
                [](const dstd::Vector<TestMetadata>&) -> std::uint64_t {
                return 0;
            };

            auto event_collector =
                [](coro::Scope&, const TestChunk&) -> dstd::Vector<TestEvent> {
                return {};
            };

            auto event_hasher =
                [](const dstd::Vector<TestEvent>&) -> std::uint64_t {
                return 0;
            };

            auto verifier = dstd::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            dstd::Vector<TestChunk> chunks;
            dstd::Vector<TestMetadata> metadata;
            ChunkVerificationUtilityInput<TestChunk, TestMetadata> input(
                chunks, metadata);

            auto result = run_verifier(verifier, input, 2);

            CHECK(result.passed == true);
            CHECK(result.input_hash == 0);
            CHECK(result.output_hash == 0);
        }

        SUBCASE("Single chunk") {
            printf("Starting single chunk test\n");

            auto input_hasher = [](const dstd::Vector<TestMetadata>& metadata)
                -> std::uint64_t {
                auto hash = metadata.empty() ? 0 : metadata[0].total_events;
                printf("Input hash: %lu\n", (unsigned long)hash);
                return hash;
            };

            auto event_collector =
                [](coro::Scope&,
                   const TestChunk& chunk) -> dstd::Vector<TestEvent> {
                printf("Collecting from chunk %zu\n", chunk.id);
                return chunk.data;
            };

            auto event_hasher =
                [](const dstd::Vector<TestEvent>& events) -> std::uint64_t {
                auto hash = events.size();
                printf("Event hash: %zu (from %zu events)\n", hash,
                       events.size());
                return hash;
            };

            printf("Creating verifier\n");
            auto verifier = dstd::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            dstd::Vector<TestChunk> chunks = {TestChunk(1, {10, 20, 30})};
            dstd::Vector<TestMetadata> metadata = {TestMetadata("single", 3)};
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
                [](const dstd::Vector<TestMetadata>&) -> std::uint64_t {
                return 100;
            };

            auto event_collector = [](coro::Scope&, const TestChunk& chunk)
                -> dstd::Vector<TestEvent> { return chunk.data; };

            auto event_hasher =
                [](const dstd::Vector<TestEvent>&) -> std::uint64_t {
                return 100;  // Match input hash
            };

            auto verifier = dstd::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            dstd::Vector<TestChunk> chunks = {TestChunk(1, {1, 2}),
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
