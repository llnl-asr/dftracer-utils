#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config_manager.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/utility_adapter.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/utilities/composites/composites.h>
#include <dftracer/utils/utilities/io/types/types.h>

#include <argparse/argparse.hpp>
#include <chrono>
#include <thread>

using namespace dftracer::utils;
using EventId = utilities::composites::dft::EventId;

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    auto default_checkpoint_size_str =
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE) + " B (" +
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE / (1024 * 1024)) +
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
        .default_value(
            static_cast<std::size_t>(Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--executor-threads")
        .help(
            "Number of executor threads for parallel processing (default: "
            "number "
            "of CPU cores)")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    program.add_argument("--scheduler-threads")
        .help("Number of scheduler threads (default: 1, typically not changed)")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(1));

    program.add_argument("--index-dir")
        .help("Directory to store index files (default: system temp directory)")
        .default_value<std::string>("");

    program.add_argument("--verify")
        .help("Verify output chunks match input by comparing event IDs")
        .flag();

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
    std::size_t scheduler_threads =
        program.get<std::size_t>("--scheduler-threads");
    std::string index_dir = program.get<std::string>("--index-dir");

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
    std::printf("  Scheduler threads: %zu\n", scheduler_threads);
    std::printf("==========================================\n\n");

    if (!fs::exists(output_dir)) {
        fs::create_directories(output_dir);
    }

    // Create pipeline with configuration
    auto pipeline_config = PipelineConfigManager()
                               .with_name("DFTracer Split")
                               .with_executor_threads(executor_threads)
                               .with_scheduler_threads(scheduler_threads);

    Pipeline pipeline(pipeline_config);

    auto start_time = std::chrono::high_resolution_clock::now();

    // ========================================================================
    // Task 1: Build Indexes
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s", "Task 1: Building indexes...");

    // Task 1.1: Input - Directory input for file discovery
    auto index_dir_input =
        utilities::composites::DirectoryProcessInput::from_directory(log_dir)
            .with_extensions({".pfw", ".pfw.gz"});

    // Task 1.2: Output - Index build results
    using IndexBuildOutput = utilities::composites::BatchFileProcessOutput<
        utilities::composites::dft::IndexBuildUtilityOutput>;

    // Task 1.3: Utility definition - DirectoryFileProcessorUtility with
    // IndexBuilder
    auto index_builder_processor = [checkpoint_size, force, &index_dir](
                                       TaskContext& /*ctx*/,
                                       const std::string& file_path)
        -> utilities::composites::dft::IndexBuildUtilityOutput {
        std::string idx_path = utilities::composites::dft::determine_index_path(
            file_path, index_dir);
        auto input =
            utilities::composites::dft::IndexBuildUtilityInput::from_file(
                file_path)
                .with_checkpoint_size(checkpoint_size)
                .with_force_rebuild(force)
                .with_index(idx_path);
        return utilities::composites::dft::IndexBuilderUtility{}.process(input);
    };

    auto index_workflow =
        std::make_shared<utilities::composites::DirectoryFileProcessorUtility<
            utilities::composites::dft::IndexBuildUtilityOutput>>(
            index_builder_processor);

    // Task 1.4: Task definition - Convert utility to task
    auto task1_build_indexes = utilities::use(index_workflow).as_task();
    task1_build_indexes->with_name("BuildIndexes");

    // ========================================================================
    // Task 2: Collect Metadata
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s", "Task 2: Collecting metadata...");

    // Task 2.1: Output - Metadata collection results
    using MetadataCollectOutput = utilities::composites::BatchFileProcessOutput<
        utilities::composites::dft::MetadataCollectorUtilityOutput>;

    // Task 2.3: Utility definition - DirectoryFileProcessorUtility with
    // MetadataCollector
    auto metadata_processor = [checkpoint_size, force, &index_dir](
                                  TaskContext& /*ctx*/,
                                  const std::string& file_path)
        -> utilities::composites::dft::MetadataCollectorUtilityOutput {
        std::string idx_path = utilities::composites::dft::determine_index_path(
            file_path, index_dir);

        auto input = utilities::composites::dft::MetadataCollectorUtilityInput::
                         from_file(file_path)
                             .with_checkpoint_size(checkpoint_size)
                             .with_force_rebuild(force)
                             .with_index(idx_path);

        return utilities::composites::dft::MetadataCollectorUtility{}.process(
            input);
    };

    auto metadata_workflow =
        std::make_shared<utilities::composites::DirectoryFileProcessorUtility<
            utilities::composites::dft::MetadataCollectorUtilityOutput>>(
            metadata_processor);

    // Task 2.4: Task definition - Convert utility to task
    auto task2_collect_metadata = utilities::use(metadata_workflow).as_task();
    task2_collect_metadata->with_name("CollectMetadata");

    // Task 2 also needs the same directory input as Task 1, use combiner
    // Combiner transforms Task 1's output (IndexBuildOutput) to Task 2's input
    // (DirectoryProcessInput)
    task2_collect_metadata->with_combiner([&log_dir](const IndexBuildOutput&) {
        // Return fresh directory input for metadata collection
        return utilities::composites::DirectoryProcessInput::from_directory(
                   log_dir)
            .with_extensions({".pfw", ".pfw.gz"});
    });

    // ========================================================================
    // Task 3: Create Chunk Mappings
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s", "Task 3: Creating chunk mappings...");

    // Task 3.1: Input - Metadata from Task 2
    using ChunkMappingInput = MetadataCollectOutput;

    // Task 3.2: Output - Chunk manifests
    using ChunkMappingOutput =
        std::vector<utilities::composites::dft::DFTracerChunkManifest>;

    // Task 3.3: Utility definition - Transform metadata to chunk manifests
    auto create_chunk_mappings_func =
        [chunk_size_mb](
            const ChunkMappingInput& batch_result) -> ChunkMappingOutput {
        DFTRACER_UTILS_LOG_INFO("Creating chunk mappings from %zu files...",
                                batch_result.results.size());

        utilities::composites::dft::ChunkManifestMapperUtility mapper;
        auto mapper_input =
            utilities::composites::dft::ChunkManifestMapperUtilityInput::
                from_metadata(batch_result.results)
                    .with_target_size(static_cast<double>(chunk_size_mb));

        auto manifests = mapper.process(mapper_input);

        DFTRACER_UTILS_LOG_INFO("Created %zu chunks", manifests.size());
        return manifests;
    };

    // Task 3.4: Task definition
    auto task3_create_mappings =
        make_task(create_chunk_mappings_func, "CreateChunkMappings");

    // ========================================================================
    // Task 4: Prepare Chunk Extraction Inputs
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s", "Task 4: Preparing extraction inputs...");

    // Task 4.1: Input - Chunk manifests from Task 3
    using PrepareExtractInput = ChunkMappingOutput;

    // Task 4.2: Output - Vector of extraction inputs
    using PrepareExtractOutput =
        std::vector<utilities::composites::dft::ChunkExtractorUtilityInput>;

    // Task 4.3: Utility definition - Transform manifests to extraction inputs
    auto prepare_extract_inputs_func =
        [&app_name, &output_dir, compress](
            const PrepareExtractInput& manifests) -> PrepareExtractOutput {
        DFTRACER_UTILS_LOG_INFO("Preparing %zu extraction inputs...",
                                manifests.size());

        PrepareExtractOutput chunk_inputs;
        chunk_inputs.reserve(manifests.size());

        for (int i = 0; i < static_cast<int>(manifests.size()); ++i) {
            auto input =
                utilities::composites::dft::ChunkExtractorUtilityInput::
                    from_manifest(i + 1, manifests[i])
                        .with_output_dir(output_dir)
                        .with_app_name(app_name)
                        .with_compression(compress);
            chunk_inputs.push_back(input);
        }

        return chunk_inputs;
    };

    // Task 4.4: Task definition
    auto task4_prepare_inputs =
        make_task(prepare_extract_inputs_func, "PrepareExtractInputs");

    // ========================================================================
    // Task 5: Extract Chunks (INTRA-TASK PARALLELISM via BatchProcessorUtility)
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s", "Task 5: Extracting chunks...");

    // Task 5.1: Output - Extraction results
    using ExtractChunksOutput =
        std::vector<utilities::composites::dft::ChunkExtractorUtilityOutput>;

    // Task 5.3: Utility definition - BatchProcessorUtility with ChunkExtractor
    auto extractor_workflow =
        std::make_shared<utilities::composites::dft::ChunkExtractorUtility>();

    auto chunk_extractor =
        std::make_shared<utilities::composites::BatchProcessorUtility<
            utilities::composites::dft::ChunkExtractorUtilityInput,
            utilities::composites::dft::ChunkExtractorUtilityOutput>>(
            extractor_workflow);

    // Sort results by chunk_index
    chunk_extractor->with_comparator(
        [](const utilities::composites::dft::ChunkExtractorUtilityOutput& a,
           const utilities::composites::dft::ChunkExtractorUtilityOutput& b) {
            return a.chunk_index < b.chunk_index;
        });

    // Task 5.4: Task definition - Convert utility to task
    auto task5_extract_chunks = utilities::use(chunk_extractor).as_task();
    task5_extract_chunks->with_name("ExtractChunks");

    // ========================================================================
    // Task 6: Verify Output Chunks (optional)
    // ========================================================================
    std::shared_ptr<Task> final_task = task5_extract_chunks;
    std::shared_ptr<Task> task6_verify_chunks = nullptr;

    if (verify) {
        DFTRACER_UTILS_LOG_INFO("%s", "Task 6: Configuring verification...");

        // Task 6.1: Create event hasher
        auto hasher =
            std::make_shared<utilities::composites::dft::EventHasher>();

        // Task 6.2: Input hasher - hash events from metadata
        auto input_hasher =
            [hasher](
                const std::vector<
                    utilities::composites::dft::MetadataCollectorUtilityOutput>&
                    metadata) {
                auto collect_input = utilities::composites::dft::
                    EventCollectorFromMetadataCollectorUtilityInput::
                        from_metadata(metadata);

                auto metadata_collector =
                    std::make_shared<utilities::composites::dft::
                                         EventCollectorFromMetadataUtility>();

                auto events = metadata_collector->process(collect_input);
                auto hash_input =
                    utilities::composites::dft::EventHashInput::from_events(
                        std::move(events));
                return hasher->process(hash_input);
            };

        // Task 6.3: Event collector - extract event IDs from chunk results
        auto event_collector =
            [](TaskContext&,
               const utilities::composites::dft::ChunkExtractorUtilityOutput&
                   result) {
                return result.event_ids;  // Return pre-collected event IDs
            };

        // Task 6.4: Event hasher - hash collected events
        auto event_hasher = [hasher](const std::vector<EventId>& events) {
            auto hash_input =
                utilities::composites::dft::EventHashInput::from_events(events);
            return hasher->process(hash_input);
        };

        // Task 6.5: Create chunk verifier utility
        auto verifier =
            std::make_shared<utilities::composites::ChunkVerifierUtility<
                utilities::composites::dft::ChunkExtractorUtilityOutput,
                utilities::composites::dft::MetadataCollectorUtilityOutput,
                EventId>>(input_hasher, event_collector, event_hasher);

        // Task 6.6: Task definition - Use utility adapter pattern
        task6_verify_chunks = utilities::use(verifier).as_task();
        task6_verify_chunks->with_name("VerifyChunks");

        // INTER-TASK dependencies: Task 6 needs Task 5 (chunks) and Task 2
        // (metadata)

        // Task 6.7: Combiner to merge chunks and metadata into verification
        // input
        task6_verify_chunks->with_combiner([](const ExtractChunksOutput& chunks,
                                              const MetadataCollectOutput&
                                                  metadata) {
            return utilities::composites::ChunkVerificationUtilityInput<
                       utilities::composites::dft::ChunkExtractorUtilityOutput,
                       utilities::composites::dft::
                           MetadataCollectorUtilityOutput>::from_chunks(chunks)
                .with_metadata(metadata.results);
        });

        final_task = task6_verify_chunks;
    }

    // ========================================================================
    // Execute Pipeline
    // ========================================================================

    // Define dependencies
    task2_collect_metadata->depends_on(task1_build_indexes);
    task3_create_mappings->depends_on(task2_collect_metadata);
    task4_prepare_inputs->depends_on(task3_create_mappings);
    task5_extract_chunks->depends_on(task4_prepare_inputs);
    if (verify && task6_verify_chunks) {
        task6_verify_chunks->depends_on(task5_extract_chunks);
        task6_verify_chunks->depends_on(task2_collect_metadata);
    }

    // Set up pipeline
    pipeline.set_source(task1_build_indexes);
    pipeline.set_destination(final_task);

    // Execute pipeline with initial input
    pipeline.execute(index_dir_input);

    // Get final results
    auto extraction_results = task5_extract_chunks->get<ExtractChunksOutput>();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    // ========================================================================
    // Print Results
    // ========================================================================

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

    auto metadata_results =
        task2_collect_metadata->get<MetadataCollectOutput>();
    std::size_t successful_files = 0;
    double total_size_mb = 0;
    for (const auto& meta : metadata_results.results) {
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

    // Optional verification phase (Task 6)
    if (verify) {
        auto verify_result =
            task6_verify_chunks
                ->get<utilities::composites::ChunkVerificationUtilityOutput>();
        if (verify_result.input_hash == verify_result.output_hash) {
            std::printf(
                "  \u2713 Verification: PASSED - all events present in "
                "output\n");
        } else {
            std::printf(
                "  \u2717 Verification: FAILED - event mismatch detected\n");
        }
        std::printf("    Input hash:  0x%016llx\n", verify_result.input_hash);
        std::printf("    Output hash: 0x%016llx\n", verify_result.output_hash);
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
