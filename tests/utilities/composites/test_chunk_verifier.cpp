#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config_manager.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/utilities/utility_adapter.h>
#include <dftracer/utils/utilities/composites/chunk_verifier.h>
#include <doctest/doctest.h>

#include <any>
#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;

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

    TestMetadata() : total_events(0) {}
    TestMetadata(const std::string& n, std::size_t t)
        : name(n), total_events(t) {}
};

using TestEvent = int;

TEST_SUITE("ChunkVerifier") {
    TEST_CASE("ChunkVerifier - Basic Verification") {
        SUBCASE("Verify matching chunks") {
            std::cout << "Starting test: Verify matching chunks" << std::endl;

            // Create input hasher
            auto input_hasher =
                [](const std::vector<TestMetadata>& metadata) -> std::uint64_t {
                std::uint64_t hash = 0;
                for (const auto& meta : metadata) {
                    hash ^= std::hash<std::string>{}(meta.name);
                    hash ^= std::hash<std::size_t>{}(meta.total_events);
                }
                std::cout << "Input hasher calculated hash: " << hash
                          << std::endl;
                return hash;
            };

            // Create event collector
            auto event_collector =
                [](TaskContext&,
                   const TestChunk& chunk) -> std::vector<TestEvent> {
                std::cout << "Collecting events from chunk " << chunk.id
                          << std::endl;
                return chunk.data;
            };

            // Create event hasher - must match the logic of input_hasher for
            // this test
            auto event_hasher =
                [](const std::vector<TestEvent>& /*events*/) -> std::uint64_t {
                // Since the input hash uses metadata (name="test",
                // total_events=9), and we want the hashes to match, we need to
                // produce the same hash from the collected events
                std::uint64_t hash = 0;
                hash ^= std::hash<std::string>{}("test");
                hash ^= std::hash<std::size_t>{}(9);  // total events
                std::cout << "Event hasher calculated hash: " << hash
                          << std::endl;
                return hash;
            };

            std::cout << "Creating verifier..." << std::endl;
            // Create verifier
            auto verifier = std::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            std::cout << "Setting up pipeline..." << std::endl;
            // Set up pipeline instead of bare executor/scheduler
            // Need N+1 threads: 1 for main task + N for parallel chunk
            // processing
            auto pipeline_config =
                PipelineConfigManager()
                    .with_executor_threads(4)  // 1 main + 3 chunks
                    .with_scheduler_threads(1)
                    .with_watchdog(true)
                    .with_task_timeout(std::chrono::seconds(5));

            Pipeline pipeline(pipeline_config);

            // Create input
            std::vector<TestChunk> chunks = {TestChunk(1, {1, 2, 3}),
                                             TestChunk(2, {4, 5, 6}),
                                             TestChunk(3, {7, 8, 9})};

            std::vector<TestMetadata> metadata = {TestMetadata("test", 9)};

            ChunkVerificationUtilityInput<TestChunk, TestMetadata> input(
                chunks, metadata);

            std::cout << "Creating task adapter..." << std::endl;
            // Use adapter to convert to task
            auto verify_task = use(verifier).as_task();

            std::cout << "Setting up pipeline with single task..." << std::endl;
            // Set up pipeline with single task
            pipeline.set_source(verify_task);
            pipeline.set_destination(verify_task);

            std::cout << "Executing pipeline..." << std::endl;
            // Execute pipeline
            pipeline.execute(input);

            std::cout << "Getting results..." << std::endl;
            // Get results
            auto result = verify_task->get<ChunkVerificationUtilityOutput>();

            std::cout << "Checking results..." << std::endl;
            CHECK(result.passed == true);
            CHECK(result.input_hash == result.output_hash);

            std::cout << "Test completed successfully" << std::endl;
        }

        SUBCASE("Detect mismatched chunks") {
            // Create hashers that will produce different hashes
            auto input_hasher =
                [](const std::vector<TestMetadata>& metadata) -> std::uint64_t {
                (void)metadata;  // Suppress unused warning
                return 12345;    // Fixed input hash
            };

            auto event_collector = [](TaskContext&, const TestChunk& chunk)
                -> std::vector<TestEvent> { return chunk.data; };

            auto event_hasher =
                [](const std::vector<TestEvent>& events) -> std::uint64_t {
                (void)events;  // Suppress unused warning
                return 67890;  // Different fixed output hash
            };

            auto verifier = std::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            auto pipeline_config = PipelineConfigManager()
                                       .with_executor_threads(4)
                                       .with_scheduler_threads(1);
            Pipeline pipeline(pipeline_config);

            std::vector<TestChunk> chunks = {TestChunk(1, {1, 2, 3})};

            std::vector<TestMetadata> metadata = {TestMetadata("test", 3)};

            ChunkVerificationUtilityInput<TestChunk, TestMetadata> input(
                chunks, metadata);

            auto verify_task = use(verifier).as_task();
            pipeline.set_source(verify_task);
            pipeline.set_destination(verify_task);
            pipeline.execute(input);

            auto result = verify_task->get<ChunkVerificationUtilityOutput>();

            CHECK(result.passed == false);
            CHECK(result.input_hash != result.output_hash);
            CHECK(result.error_message.find("Hash mismatch") !=
                  std::string::npos);
        }
    }

    TEST_CASE("ChunkVerifier - Parallel Processing") {
        SUBCASE("Process multiple chunks in parallel") {
            // For this test to pass, input_hash and output_hash must match
            // We'll make input_hasher predict what the sum of events will be
            auto input_hasher =
                [](const std::vector<TestMetadata>& metadata) -> std::uint64_t {
                // The test creates 10 chunks with values:
                // chunk 0: [0,1,2], chunk 1: [3,4,5], ..., chunk 9: [27,28,29]
                // Sum = 0+1+2+...+29 = 435
                (void)metadata;  // Suppress unused warning
                return 435;      // Expected sum of all event values
            };

            // Event collector that simulates work
            auto event_collector =
                [](TaskContext&,
                   const TestChunk& chunk) -> std::vector<TestEvent> {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));  // Simulate work
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

            auto pipeline_config = PipelineConfigManager()
                                       .with_executor_threads(4)
                                       .with_scheduler_threads(1);
            Pipeline pipeline(pipeline_config);

            // Create many chunks
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

            auto verify_task = use(verifier).as_task();
            pipeline.set_source(verify_task);
            pipeline.set_destination(verify_task);
            pipeline.execute(input);

            auto result = verify_task->get<ChunkVerificationUtilityOutput>();

            CHECK(result.passed == true);
            CHECK(result.input_hash == 435);  // Expected sum
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
                [](TaskContext&, const TestChunk&) -> std::vector<TestEvent> {
                return {};
            };

            auto event_hasher =
                [](const std::vector<TestEvent>&) -> std::uint64_t {
                return 0;
            };

            auto verifier = std::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            auto pipeline_config = PipelineConfigManager()
                                       .with_executor_threads(2)
                                       .with_scheduler_threads(1);
            Pipeline pipeline(pipeline_config);

            std::vector<TestChunk> chunks;
            std::vector<TestMetadata> metadata;

            ChunkVerificationUtilityInput<TestChunk, TestMetadata> input(
                chunks, metadata);

            auto verify_task = use(verifier).as_task();
            pipeline.set_source(verify_task);
            pipeline.set_destination(verify_task);
            pipeline.execute(input);

            auto result = verify_task->get<ChunkVerificationUtilityOutput>();

            CHECK(result.passed == true);
            CHECK(result.input_hash == 0);
            CHECK(result.output_hash == 0);
        }

        SUBCASE("Single chunk") {
            std::cout << "Starting single chunk test" << std::endl;

            auto input_hasher =
                [](const std::vector<TestMetadata>& metadata) -> std::uint64_t {
                auto hash = metadata.empty() ? 0 : metadata[0].total_events;
                std::cout << "Input hash: " << hash << std::endl;
                return hash;
            };

            auto event_collector =
                [](TaskContext& ctx,
                   const TestChunk& chunk) -> std::vector<TestEvent> {
                (void)ctx;  // Not used in this simple test
                std::cout << "Collecting from chunk " << chunk.id << std::endl;
                return chunk.data;
            };

            auto event_hasher =
                [](const std::vector<TestEvent>& events) -> std::uint64_t {
                auto hash = events.size();
                std::cout << "Event hash: " << hash << " (from "
                          << events.size() << " events)" << std::endl;
                return hash;
            };

            std::cout << "Creating verifier" << std::endl;
            auto verifier = std::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            std::cout << "Creating pipeline" << std::endl;
            {
                // Need at least 2 threads: 1 for main task + 1 for subtasks
                auto pipeline_config =
                    PipelineConfigManager()
                        .with_executor_threads(
                            2)  // Increased from 1 to avoid deadlock
                        .with_scheduler_threads(1);
                Pipeline pipeline(pipeline_config);

                std::vector<TestChunk> chunks = {TestChunk(1, {10, 20, 30})};

                std::vector<TestMetadata> metadata = {
                    TestMetadata("single", 3)};

                ChunkVerificationUtilityInput<TestChunk, TestMetadata> input(
                    chunks, metadata);

                std::cout << "Creating task" << std::endl;
                auto verify_task = use(verifier).as_task();

                std::cout << "Scheduling task" << std::endl;
                pipeline.set_source(verify_task);
                pipeline.set_destination(verify_task);
                pipeline.execute(input);

                std::cout << "Waiting for completion" << std::endl;
                std::cout << "Getting result" << std::endl;
                auto result =
                    verify_task->get<ChunkVerificationUtilityOutput>();

                std::cout << "Checking result" << std::endl;
                CHECK(result.passed == true);
                CHECK(result.input_hash == 3);
                CHECK(result.output_hash == 3);

                std::cout << "Single chunk test completed" << std::endl;
            }
            // Executor and Scheduler destroyed here
        }
    }

    TEST_CASE("ChunkVerifier - Builder Pattern") {
        SUBCASE("Using from_chunks builder") {
            auto input_hasher =
                [](const std::vector<TestMetadata>&) -> std::uint64_t {
                return 100;
            };

            auto event_collector = [](TaskContext&, const TestChunk& chunk)
                -> std::vector<TestEvent> { return chunk.data; };

            auto event_hasher =
                [](const std::vector<TestEvent>&) -> std::uint64_t {
                return 100;  // Match input hash
            };

            auto verifier = std::make_shared<
                ChunkVerifierUtility<TestChunk, TestMetadata, TestEvent>>(
                input_hasher, event_collector, event_hasher);

            auto pipeline_config = PipelineConfigManager()
                                       .with_executor_threads(2)
                                       .with_scheduler_threads(1);
            Pipeline pipeline(pipeline_config);

            std::vector<TestChunk> chunks = {TestChunk(1, {1, 2}),
                                             TestChunk(2, {3, 4})};

            // Use builder pattern
            auto input =
                ChunkVerificationUtilityInput<TestChunk,
                                              TestMetadata>::from_chunks(chunks)
                    .with_metadata({TestMetadata("built", 4)});

            auto verify_task = use(verifier).as_task();
            pipeline.set_source(verify_task);
            pipeline.set_destination(verify_task);
            pipeline.execute(input);

            auto result = verify_task->get<ChunkVerificationUtilityOutput>();

            CHECK(result.passed == true);
        }
    }
}
