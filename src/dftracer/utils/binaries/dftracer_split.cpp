#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/task_graph/task_graph.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/utility_adapter.h>
#include <dftracer/utils/utilities/composites/composites.h>
#include <dftracer/utils/utilities/composites/dft/chunk_extractor_utility.h>
#include <dftracer/utils/utilities/fileio/types/types.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <unistd.h>

#include <argparse/argparse.hpp>
#include <chrono>
#include <cinttypes>

using namespace dftracer::utils;
using namespace dftracer::utils::task_graph;
using Metadata = utilities::composites::dft::MetadataCollectorUtilityOutput;
using ChunkManifest =
    utilities::composites::dft::internal::DFTracerChunkManifest;
using ExtractInput = utilities::composites::dft::ChunkExtractorUtilityInput;
using ExtractResult = utilities::composites::dft::ChunkExtractorUtilityOutput;

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    auto default_checkpoint_size_str =
        std::to_string(dftracer::utils::utilities::indexer::internal::Indexer::
                           DEFAULT_CHECKPOINT_SIZE) +
        " B (" +
        std::to_string(dftracer::utils::utilities::indexer::internal::Indexer::
                           DEFAULT_CHECKPOINT_SIZE /
                       (1024 * 1024)) +
        " MB)";

    argparse::ArgumentParser program("dftracer_split",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Split DFTracer traces into equal-sized chunks using explicit pipeline "
        "with maximum parallelism");

    program.add_argument("-n", "--app-name")
        .help("Application name for output files")
        .default_value<std::string>("app");

    program.add_argument("-d", "--directory")
        .help("Input directory containing .pfw or .pfw.gz files")
        .default_value<std::string>(".");

    program.add_argument("-o", "--output")
        .help("Output directory for split files")
        .default_value<std::string>("./split");

    program.add_argument("-s", "--chunk-size")
        .help("Chunk size in MB")
        .scan<'d', int>()
        .default_value(4);

    program.add_argument("-f", "--force")
        .help("Override existing files and force index recreation")
        .flag();

    program.add_argument("-c", "--compress")
        .help("Compress output files with gzip")
        .flag()
        .default_value(true);

    program.add_argument("-v", "--verbose").help("Enable verbose mode").flag();

    program.add_argument("--checkpoint-size")
        .help("Checkpoint size for indexing in bytes (default: " +
              default_checkpoint_size_str + ")")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(
            dftracer::utils::utilities::indexer::internal::Indexer::
                DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--executor-threads")
        .help(
            "Number of executor threads for parallel processing (default: "
            "number "
            "of CPU cores)")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(dftracer_utils_hardware_concurrency()));

    program.add_argument("--index-dir")
        .help("Directory to store index files (default: system temp directory)")
        .default_value<std::string>("");

    program.add_argument("--verify")
        .help("Verify output chunks match input by comparing event IDs")
        .flag();

    program.add_argument("--disable-watchdog")
        .help("Disable watchdog for hang detection")
        .flag();

    program.add_argument("--watchdog-global-timeout")
        .help(
            "Watchdog global timeout for pipeline execution in seconds (0 = no "
            "timeout)")
        .scan<'d', int>()
        .default_value(0);

    program.add_argument("--watchdog-task-timeout")
        .help("Watchdog default task timeout in seconds (0 = no timeout)")
        .scan<'d', int>()
        .default_value(0);

    program.add_argument("--watchdog-interval")
        .help("Watchdog check interval in seconds")
        .scan<'d', int>()
        .default_value(1);

    program.add_argument("--watchdog-warning-threshold")
        .help("Watchdog long-running task warning threshold in seconds")
        .scan<'d', int>()
        .default_value(300);

    program.add_argument("--watchdog-idle-timeout")
        .help("Watchdog idle timeout in seconds (0 = use default)")
        .scan<'d', int>()
        .default_value(300);

    program.add_argument("--watchdog-deadlock-timeout")
        .help("Watchdog deadlock timeout in seconds (0 = use default)")
        .scan<'d', int>()
        .default_value(600);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::cerr << program << std::endl;
        return 1;
    }

    // Parse arguments
    std::string app_name = program.get<std::string>("--app-name");
    std::string log_dir = program.get<std::string>("--directory");
    std::string output_dir = program.get<std::string>("--output");
    int chunk_size_mb = program.get<int>("--chunk-size");
    bool force = program.get<bool>("--force");
    bool compress = program.get<bool>("--compress");
    bool verify = program.get<bool>("--verify");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    std::string index_dir = program.get<std::string>("--index-dir");
    bool disable_watchdog = program.get<bool>("--disable-watchdog");
    int global_timeout = program.get<int>("--watchdog-global-timeout");
    int task_timeout = program.get<int>("--watchdog-task-timeout");
    int watchdog_interval = program.get<int>("--watchdog-interval");
    int warning_threshold = program.get<int>("--watchdog-warning-threshold");
    int idle_timeout = program.get<int>("--watchdog-idle-timeout");
    int deadlock_timeout = program.get<int>("--watchdog-deadlock-timeout");

    // Setup temp index directory
    std::string temp_index_dir;
    if (index_dir.empty()) {
        temp_index_dir = fs::temp_directory_path() /
                         ("dftracer_idx_" + std::to_string(std::time(nullptr)) +
                          "_" + std::to_string(getpid()));
        fs::create_directories(temp_index_dir);
        index_dir = temp_index_dir;
        DFTRACER_UTILS_LOG_INFO("Created temporary index directory: %s",
                                index_dir.c_str());
    }

    log_dir = fs::absolute(log_dir).string();
    output_dir = fs::absolute(output_dir).string();

    std::printf("==========================================\n");
    std::printf("DFTracer Split (Explicit Pipeline)\n");
    std::printf("==========================================\n");
    std::printf("Arguments:\n");
    std::printf("  App name: %s\n", app_name.c_str());
    std::printf("  Override: %s\n", force ? "true" : "false");
    std::printf("  Compress: %s\n", compress ? "true" : "false");
    std::printf("  Data dir: %s\n", log_dir.c_str());
    std::printf("  Output dir: %s\n", output_dir.c_str());
    std::printf("  Chunk size: %d MB\n", chunk_size_mb);
    std::printf("  Executor threads: %zu\n", executor_threads);
    std::printf("==========================================\n\n");

    if (!fs::exists(output_dir)) {
        fs::create_directories(output_dir);
    }

    // Create pipeline with configuration
    auto pipeline_config =
        PipelineConfig()
            .with_name("DFTracer Split")
            .with_compute_threads(executor_threads)
            .with_watchdog(!disable_watchdog)
            .with_global_timeout(std::chrono::seconds(global_timeout))
            .with_task_timeout(std::chrono::seconds(task_timeout))
            .with_watchdog_interval(std::chrono::seconds(watchdog_interval))
            .with_warning_threshold(std::chrono::seconds(warning_threshold))
            .with_executor_idle_timeout(std::chrono::seconds(idle_timeout))
            .with_executor_deadlock_timeout(
                std::chrono::seconds(deadlock_timeout));

    Pipeline pipeline(pipeline_config);

    auto start_time = std::chrono::high_resolution_clock::now();

    // Phase 1: Discover input files
    DFTRACER_UTILS_LOG_INFO("%s", "Discovering input files...");

    std::vector<std::string> input_files;
    for (const auto& entry : fs::directory_iterator(log_dir)) {
        if (entry.is_regular_file()) {
            std::string path = entry.path().string();
            if (path.ends_with(".pfw.gz") || path.ends_with(".pfw")) {
                input_files.push_back(path);
            }
        }
    }

    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in %s",
                                 log_dir.c_str());
        return 1;
    }

    DFTRACER_UTILS_LOG_INFO("Found %zu input files", input_files.size());

    if (force) {
        const std::string shared_index_path =
            utilities::composites::dft::internal::determine_index_path(
                input_files.front(), index_dir);
        if (fs::exists(shared_index_path)) {
            DFTRACER_UTILS_LOG_INFO("Clearing shared index store: %s",
                                    shared_index_path.c_str());
            fs::remove_all(shared_index_path);
        }
    }

    // Phase 2: Build TaskGraph for file processing
    auto graph = TaskGraph::builder(
        {.name = "DFTracerSplit", .max_concurrency = executor_threads});

    DFTRACER_UTILS_LOG_INFO("%s", "Creating file processing tasks...");

    auto* input_files_ptr = &input_files;
    auto file_metadata = graph.parallel<Metadata>(
        input_files.size(),
        [input_files_ptr, checkpoint_size, force, index_dir, verify](
            CoroScope&, std::size_t idx) -> coro::CoroTask<Metadata> {
            const auto& file_path = (*input_files_ptr)[idx];

            // Determine index path
            std::string index_path =
                utilities::composites::dft::internal::determine_index_path(
                    file_path, index_dir);

            // Build index
            auto idx_input =
                utilities::indexer::IndexBuildConfig::for_file(file_path)
                    .with_checkpoint_size(checkpoint_size)
                    .with_force_rebuild(false)
                    .with_index_dir(index_dir);
            co_await utilities::indexer::IndexBuilderUtility{}.process(
                idx_input);

            // Collect metadata
            auto meta_input =
                utilities::composites::dft::MetadataCollectorUtilityInput::
                    from_file(file_path)
                        .with_checkpoint_size(checkpoint_size)
                        .with_force_rebuild(false)
                        .with_index(index_path)
                        .with_compute_hash(verify);

            co_return co_await utilities::composites::dft::
                MetadataCollectorUtility{}
                    .process(meta_input);
        },
        {.name = "ProcessFile"});

    DFTRACER_UTILS_LOG_INFO("%s", "Creating chunk mapping task...");

    auto manifests_group = graph.reduce<std::vector<ChunkManifest>>(
        file_metadata, split_every{input_files.size()},
        [chunk_size_mb](CoroScope&, std::vector<Metadata> all_metadata)
            -> coro::CoroTask<std::vector<ChunkManifest>> {
            DFTRACER_UTILS_LOG_INFO("Creating chunk mappings from %zu files...",
                                    all_metadata.size());

            utilities::composites::dft::ChunkManifestMapperUtility mapper;
            auto mapper_input =
                utilities::composites::dft::ChunkManifestMapperUtilityInput::
                    from_metadata(all_metadata)
                        .with_target_size(static_cast<double>(chunk_size_mb));

            auto manifests = co_await mapper.process(mapper_input);
            DFTRACER_UTILS_LOG_INFO("Created %zu chunks", manifests.size());
            co_return manifests;
        },
        {.name = "CreateManifests"});

    DFTRACER_UTILS_LOG_INFO("%s", "Creating extraction task...");

    using ExtractChunksOutput = std::vector<ExtractResult>;

    auto* app_name_ptr = &app_name;
    auto* output_dir_ptr = &output_dir;

    auto task_extract_chunks = make_task(
        [app_name_ptr, output_dir_ptr, compress, verify, executor_threads](
            CoroScope& scope, std::vector<ChunkManifest> manifests)
            -> coro::CoroTask<ExtractChunksOutput> {
            DFTRACER_UTILS_LOG_INFO("Extracting %zu chunks in parallel...",
                                    manifests.size());

            auto permits = coro::make_channel<bool>(executor_threads * 2);
            for (std::size_t i = 0; i < executor_threads * 2; ++i) {
                permits->try_send(true);
            }

            std::vector<coro::SpawnFuture<ExtractResult>> futures;
            futures.reserve(manifests.size());

            for (std::size_t i = 0; i < manifests.size(); ++i) {
                auto input = ExtractInput::from_manifest(
                                 static_cast<int>(i + 1), manifests[i])
                                 .with_output_dir(*output_dir_ptr)
                                 .with_app_name(*app_name_ptr)
                                 .with_compression(compress)
                                 .with_compute_hash(verify);

                futures.push_back(scope.spawn(
                    [input = std::move(input),
                     permits](CoroScope& s) -> coro::CoroTask<ExtractResult> {
                        co_await s.receive(permits);
                        try {
                            utilities::composites::dft::ChunkExtractorUtility
                                extractor;
                            auto result = co_await extractor.process(input);
                            permits->try_send(true);
                            co_return result;
                        } catch (...) {
                            permits->try_send(true);
                            throw;
                        }
                    }));
            }

            ExtractChunksOutput results;
            results.reserve(futures.size());
            for (auto& future : futures) {
                results.push_back(co_await future);
            }

            // Sort by chunk index
            std::sort(results.begin(), results.end(),
                      [](const ExtractResult& a, const ExtractResult& b) {
                          return a.chunk_index < b.chunk_index;
                      });

            co_return results;
        },
        "ExtractChunks");

    task_extract_chunks->depends_on(manifests_group.task());
    graph.add(task_extract_chunks);

    // Phase 3: Optional verification
    std::shared_ptr<Task> final_task = task_extract_chunks;
    std::shared_ptr<Task> task_verify_chunks = nullptr;

    if (verify) {
        DFTRACER_UTILS_LOG_INFO("%s", "Configuring verification...");

        struct VerifyInput {
            ExtractChunksOutput chunks;
            std::vector<Metadata> all_metadata;
        };

        // Verification task receives both extraction results and metadata.
        // Both are passed via combiner so the scheduler keeps parent
        // results alive until this task consumes them.
        task_verify_chunks = make_task(
            [](CoroScope&, const VerifyInput& input)
                -> coro::CoroTask<
                    utilities::composites::ChunkVerificationUtilityOutput> {
                // Sum output hashes from extraction results
                std::size_t output_hash = 0;
                for (const auto& chunk : input.chunks) {
                    output_hash += chunk.event_hash;
                }

                // Sum input hashes from metadata (computed during collection)
                std::size_t input_hash = 0;
                for (const auto& meta : input.all_metadata) {
                    if (!meta.success) continue;
                    input_hash += meta.event_hash;
                }

                co_return utilities::composites::
                    ChunkVerificationUtilityOutput::success(
                        static_cast<std::uint64_t>(input_hash),
                        static_cast<std::uint64_t>(output_hash));
            },
            "VerifyChunks");

        // Depend on both extract results and metadata tasks.
        // The combiner collects parent outputs into the typed struct.
        task_verify_chunks->depends_on(task_extract_chunks);
        for (const auto& meta_task : file_metadata.tasks()) {
            task_verify_chunks->depends_on(meta_task);
        }

        task_verify_chunks->with_combiner(
            [](const std::vector<std::any>& inputs) -> std::any {
                // inputs[0] = ExtractChunksOutput (from extract task)
                // inputs[1..N] = Metadata (from each metadata task)
                auto chunks = std::any_cast<ExtractChunksOutput>(inputs[0]);

                std::vector<Metadata> all_metadata;
                all_metadata.reserve(inputs.size() - 1);
                for (std::size_t i = 1; i < inputs.size(); ++i) {
                    all_metadata.push_back(std::any_cast<Metadata>(inputs[i]));
                }

                VerifyInput vi{std::move(chunks), std::move(all_metadata)};
                return std::make_any<VerifyInput>(std::move(vi));
            });

        graph.add(task_verify_chunks);
        final_task = task_verify_chunks;
    }

    // Phase 4: Execute Pipeline
    DFTRACER_UTILS_LOG_INFO("%s", "Executing pipeline...");

    pipeline.set_source(file_metadata.tasks());
    pipeline.set_destination(final_task);
    pipeline.execute();

    // Get results from the destination task only (intermediate task values
    // are released after pipeline execution)
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Split Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Input: %zu files\n", input_files.size());

    int exit_code = 0;

    if (verify && task_verify_chunks) {
        auto verify_result =
            task_verify_chunks
                ->get<utilities::composites::ChunkVerificationUtilityOutput>();

        if (verify_result.input_hash == verify_result.output_hash) {
            std::printf(
                "  Verification: PASSED - all events present in output\n");
        } else {
            std::printf("  Verification: FAILED - event mismatch detected\n");
            exit_code = 1;
        }
        std::printf("    Input hash:  0x%016" PRIx64 "\n",
                    verify_result.input_hash);
        std::printf("    Output hash: 0x%016" PRIx64 "\n",
                    verify_result.output_hash);
    } else {
        // Without verification: extract task IS the destination, safe to read
        auto extraction_results =
            task_extract_chunks->get<ExtractChunksOutput>();

        std::size_t successful_chunks = 0;
        std::size_t total_events = 0;

        for (const auto& result : extraction_results) {
            if (result.success) {
                successful_chunks++;
                total_events += result.events;
            } else {
                DFTRACER_UTILS_LOG_ERROR("Failed to create chunk %d",
                                         result.chunk_index);
            }
        }

        std::printf("  Output: %zu/%zu chunks, %zu events\n", successful_chunks,
                    extraction_results.size(), total_events);

        if (successful_chunks != extraction_results.size()) {
            exit_code = 1;
        }
    }

    std::printf("==========================================\n");

    // Cleanup temporary index directory if created
    if (!temp_index_dir.empty() && fs::exists(temp_index_dir)) {
        DFTRACER_UTILS_LOG_INFO("Cleaning up temporary index directory: %s",
                                temp_index_dir.c_str());
        fs::remove_all(temp_index_dir);
    }

    return exit_code;
}
