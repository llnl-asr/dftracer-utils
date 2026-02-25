#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/task_graph/task_graph.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/utility_adapter.h>
#include <dftracer/utils/utilities/composites/composites.h>
#include <dftracer/utils/utilities/fileio/types/types.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <argparse/argparse.hpp>
#include <chrono>
#include <cinttypes>

using namespace dftracer::utils;
using namespace dftracer::utils::task_graph;
using EventId = utilities::composites::dft::EventId;
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
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

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
                         ("dftracer_idx_" + std::to_string(std::time(nullptr)));
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

    // Phase 2: Build TaskGraph for file processing
    auto graph = TaskGraph::builder("DFTracerSplit");

    DFTRACER_UTILS_LOG_INFO("%s", "Creating file processing tasks...");

    auto file_metadata = graph.parallel<Metadata>(
        input_files.size(),
        [&input_files, checkpoint_size, force, &index_dir](
            CoroScope&, std::size_t idx) -> coro::CoroTask<Metadata> {
            const auto& file_path = input_files[idx];

            // Determine index path
            std::string idx_path =
                utilities::composites::dft::internal::determine_index_path(
                    file_path, index_dir);

            // Build index
            auto idx_input =
                utilities::composites::dft::IndexBuildUtilityInput::from_file(
                    file_path)
                    .with_checkpoint_size(checkpoint_size)
                    .with_force_rebuild(force)
                    .with_index(idx_path);
            utilities::composites::dft::IndexBuilderUtility{}.process(
                idx_input);

            // Collect metadata
            auto meta_input =
                utilities::composites::dft::MetadataCollectorUtilityInput::
                    from_file(file_path)
                        .with_checkpoint_size(checkpoint_size)
                        .with_force_rebuild(force)
                        .with_index(idx_path);

            co_return co_await utilities::composites::dft::
                MetadataCollectorUtility{}
                    .process(meta_input);
        },
        "ProcessFile");

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
        "CreateManifests");

    DFTRACER_UTILS_LOG_INFO("%s", "Creating extraction task...");

    using ExtractChunksOutput = std::vector<ExtractResult>;

    auto extractor_workflow =
        std::make_shared<utilities::composites::dft::ChunkExtractorUtility>();

    auto chunk_extractor =
        std::make_shared<utilities::composites::BatchProcessorUtility<
            ExtractInput, ExtractResult>>(extractor_workflow);

    chunk_extractor->with_comparator(
        [](const ExtractResult& a, const ExtractResult& b) {
            return a.chunk_index < b.chunk_index;
        });

    auto task_extract_chunks = utilities::use(chunk_extractor).as_task();
    task_extract_chunks->with_name("ExtractChunks");

    // Combiner: transform manifests to extraction inputs
    task_extract_chunks->with_combiner(
        [&app_name, &output_dir,
         compress](const std::vector<ChunkManifest>& manifests) {
            DFTRACER_UTILS_LOG_INFO("Preparing %zu extraction inputs...",
                                    manifests.size());

            std::vector<ExtractInput> inputs;
            inputs.reserve(manifests.size());

            for (std::size_t i = 0; i < manifests.size(); ++i) {
                auto input = ExtractInput::from_manifest(
                                 static_cast<int>(i + 1), manifests[i])
                                 .with_output_dir(output_dir)
                                 .with_app_name(app_name)
                                 .with_compression(compress);
                inputs.push_back(input);
            }
            return inputs;
        });

    // Connect extraction task to graph
    task_extract_chunks->depends_on(manifests_group.task());
    graph.add(task_extract_chunks);

    // Phase 3: Optional verification
    std::shared_ptr<Task> final_task = task_extract_chunks;
    std::shared_ptr<Task> task_verify_chunks = nullptr;

    if (verify) {
        DFTRACER_UTILS_LOG_INFO("%s", "Configuring verification...");

        using IncrementalHasher =
            utilities::composites::dft::IncrementalEventHasher;

        // Verification task: combine hashes from extraction results
        task_verify_chunks = make_task(
            [&file_metadata](CoroScope&, const ExtractChunksOutput& chunks)
                -> coro::CoroTask<
                    utilities::composites::ChunkVerificationUtilityOutput> {
                // Sum output hashes from extraction results
                std::size_t output_hash = 0;
                for (const auto& chunk : chunks) {
                    output_hash += chunk.event_hash;
                }

                // Hash input events incrementally from metadata
                IncrementalHasher input_hasher;
                for (const auto& task : file_metadata.tasks()) {
                    auto meta = task->get<Metadata>();
                    if (!meta.success) continue;

                    auto collect_input = utilities::composites::dft::
                        EventCollectorFromMetadataCollectorUtilityInput::
                            from_metadata({meta});
                    utilities::composites::dft::
                        EventCollectorFromMetadataUtility collector;
                    auto events = co_await collector.process(collect_input);
                    input_hasher.update(events);
                }

                co_return utilities::composites::
                    ChunkVerificationUtilityOutput::success(
                        static_cast<std::uint64_t>(input_hasher.get_hash()),
                        static_cast<std::uint64_t>(output_hash));
            },
            "VerifyChunks");

        task_verify_chunks->depends_on(task_extract_chunks);
        graph.add(task_verify_chunks);
        final_task = task_verify_chunks;
    }

    // Phase 4: Execute Pipeline
    DFTRACER_UTILS_LOG_INFO("%s", "Executing pipeline...");

    pipeline.set_source(file_metadata.tasks());
    pipeline.set_destination(final_task);
    pipeline.execute();

    // Get results
    auto extraction_results = task_extract_chunks->get<ExtractChunksOutput>();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

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

    std::size_t successful_files = 0;
    double total_size_mb = 0;
    for (const auto& task : file_metadata.tasks()) {
        auto meta = task->get<Metadata>();
        if (meta.success) {
            successful_files++;
            total_size_mb += meta.size_mb;
        }
    }

    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Split Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Input: %zu files, %.2f MB\n", successful_files,
                total_size_mb);
    std::printf("  Output: %zu/%zu chunks, %zu events\n", successful_chunks,
                extraction_results.size(), total_events);

    // Optional verification phase
    if (verify && task_verify_chunks) {
        auto verify_result =
            task_verify_chunks
                ->get<utilities::composites::ChunkVerificationUtilityOutput>();
        if (verify_result.input_hash == verify_result.output_hash) {
            std::printf(
                "  \u2713 Verification: PASSED - all events present in "
                "output\n");
        } else {
            std::printf(
                "  \u2717 Verification: FAILED - event mismatch detected\n");
        }
        std::printf("    Input hash:  0x%016" PRIx64 "\n",
                    verify_result.input_hash);
        std::printf("    Output hash: 0x%016" PRIx64 "\n",
                    verify_result.output_hash);
    }

    std::printf("==========================================\n");

    // Cleanup temporary index directory if created
    if (!temp_index_dir.empty() && fs::exists(temp_index_dir)) {
        DFTRACER_UTILS_LOG_INFO("Cleaning up temporary index directory: %s",
                                temp_index_dir.c_str());
        fs::remove_all(temp_index_dir);
    }

    return successful_chunks == extraction_results.size() ? 0 : 1;
}
