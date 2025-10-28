#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config_manager.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/utility_adapter.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/utilities/composites/batch_processor.h>
#include <dftracer/utils/utilities/composites/chunk_verifier.h>
#include <dftracer/utils/utilities/composites/dft/event_collector.h>
#include <dftracer/utils/utilities/composites/dft/event_hasher.h>
#include <dftracer/utils/utilities/composites/dft/index_builder.h>
#include <dftracer/utils/utilities/composites/dft/utils.h>
#include <dftracer/utils/utilities/composites/file_merger.h>

#include <argparse/argparse.hpp>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites;
using EventId = utilities::composites::dft::EventId;

// ============================================================================
// Main Function
// ============================================================================

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    auto default_checkpoint_size_str =
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE) + " B (" +
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE / (1024 * 1024)) +
        " MB)";

    argparse::ArgumentParser program("dftracer_merge",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Merge DFTracer .pfw or .pfw.gz files into a single JSON array file "
        "using composable utilities and pipeline processing");

    program.add_argument("-d", "--directory")
        .help("Directory containing .pfw or .pfw.gz files")
        .default_value<std::string>(".");

    program.add_argument("-o", "--output")
        .help("Output file path (should have .pfw extension)")
        .default_value<std::string>("combined.pfw");

    program.add_argument("-f", "--force")
        .help("Override existing output file and force index recreation")
        .flag();

    program.add_argument("-c", "--compress")
        .help("Compress output file with gzip")
        .flag();

    program.add_argument("-v", "--verbose").help("Enable verbose mode").flag();

    program.add_argument("-g", "--gzip-only")
        .help("Process only .pfw.gz files")
        .flag();

    program.add_argument("--checkpoint-size")
        .help("Checkpoint size for indexing in bytes (default: " +
              default_checkpoint_size_str + ")")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--executor-threads")
        .help(
            "Number of executor threads for parallel processing (default: "
            "number of CPU cores)")
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
        .help(
            "Verify merged output contains all input events by comparing "
            "hashes")
        .flag();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::cerr << program << std::endl;
        return 1;
    }

    // Parse arguments
    std::string input_dir = program.get<std::string>("--directory");
    std::string output_file = program.get<std::string>("--output");
    bool force_override = program.get<bool>("--force");
    bool compress_output = program.get<bool>("--compress");
    [[maybe_unused]] bool verbose = program.get<bool>("--verbose");
    bool gzip_only = program.get<bool>("--gzip-only");
    bool verify = program.get<bool>("--verify");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    std::size_t scheduler_threads =
        program.get<std::size_t>("--scheduler-threads");
    std::string index_dir = program.get<std::string>("--index-dir");

    input_dir = fs::absolute(input_dir).string();
    output_file = fs::absolute(output_file).string();

    // Setup temp index directory if not specified
    std::string temp_index_dir;
    if (index_dir.empty()) {
        temp_index_dir = fs::temp_directory_path() /
                         ("dftracer_idx_" + std::to_string(std::time(nullptr)));
        fs::create_directories(temp_index_dir);
        index_dir = temp_index_dir;
        DFTRACER_UTILS_LOG_INFO("Created temporary index directory: %s",
                                index_dir.c_str());
    }

    // Validate output file extension
    if (output_file.size() < 4 ||
        output_file.substr(output_file.size() - 4) != ".pfw") {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "Output file should have .pfw extension");
        return 1;
    }

    std::string final_output =
        compress_output ? output_file + ".gz" : output_file;
    if (fs::exists(final_output) && !force_override) {
        DFTRACER_UTILS_LOG_ERROR(
            "Output file %s exists and force override is disabled",
            final_output.c_str());
        return 1;
    }

    if (force_override) {
        if (fs::exists(output_file)) fs::remove(output_file);
        if (fs::exists(output_file + ".gz")) fs::remove(output_file + ".gz");
    }

    // Discover input files
    std::vector<std::string> input_files;
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (entry.is_regular_file()) {
            std::string path = entry.path().string();
            const std::string pfw_gz_suffix = ".pfw.gz";
            const std::string pfw_suffix = ".pfw";

            if (path.size() >= pfw_gz_suffix.size() &&
                path.compare(path.size() - pfw_gz_suffix.size(),
                             pfw_gz_suffix.size(), pfw_gz_suffix) == 0) {
                input_files.push_back(path);
            } else if (!gzip_only && path.size() >= pfw_suffix.size() &&
                       path.compare(path.size() - pfw_suffix.size(),
                                    pfw_suffix.size(), pfw_suffix) == 0) {
                input_files.push_back(path);
            }
        }
    }

    if (input_files.empty()) {
        const char* file_types = gzip_only ? ".pfw.gz" : ".pfw or .pfw.gz";
        DFTRACER_UTILS_LOG_ERROR("No %s files found in directory: %s",
                                 file_types, input_dir.c_str());
        return 1;
    }

    std::printf("==========================================\n");
    std::printf("DFTracer Merge (Pipeline Processing)\n");
    std::printf("==========================================\n");
    std::printf("Arguments:\n");
    std::printf("  Input dir: %s\n", input_dir.c_str());
    std::printf("  Output file: %s\n", final_output.c_str());
    std::printf("  Files found: %zu\n", input_files.size());
    std::printf("  Override: %s\n", force_override ? "true" : "false");
    std::printf("  Compress: %s\n", compress_output ? "true" : "false");
    std::printf("  Verify: %s\n", verify ? "true" : "false");
    std::printf("  Executor threads: %zu\n", executor_threads);
    std::printf("  Scheduler threads: %zu\n", scheduler_threads);
    std::printf("==========================================\n\n");

    auto start_time = std::chrono::high_resolution_clock::now();

    // Create temp directory for intermediate files
    std::string temp_dir =
        fs::path(output_file).parent_path() / "dftracer_merge_tmp";
    fs::create_directories(temp_dir);

    // ========================================================================
    // Create Pipeline with Configuration
    // ========================================================================
    auto pipeline_config = PipelineConfigManager()
                               .with_name("DFTracer Merge")
                               .with_executor_threads(executor_threads)
                               .with_scheduler_threads(scheduler_threads);

    Pipeline pipeline(pipeline_config);

    // ========================================================================
    // Task 1: Process files using FileMergerUtility
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s", "Task 1: Processing files for merge...");

    // Task 1.1: Create merge inputs
    auto create_merge_inputs =
        [&temp_dir, &index_dir, checkpoint_size,
         force_override](const std::vector<std::string>& files)
        -> std::vector<FileMergerUtilityInput> {
        std::vector<FileMergerUtilityInput> inputs;
        inputs.reserve(files.size());

        for (const auto& file : files) {
            // Determine temp file path
            std::string temp_file =
                temp_dir + "/merge_temp_" +
                std::to_string(FileMergerUtility::get_next_counter()) + ".tmp";

            auto input =
                FileMergerUtilityInput::from_file(file)
                    .with_output(temp_file)
                    .with_index(dft::determine_index_path(file, index_dir))
                    .with_checkpoint_size(checkpoint_size)
                    .with_force_rebuild(force_override);

            inputs.push_back(input);
        }

        return inputs;
    };

    auto task1_create_inputs =
        make_task(create_merge_inputs, "CreateMergeInputs");

    // Task 1.2: Process files using BatchProcessorUtility with
    // FileMergerUtility
    auto file_merger_func =
        [](TaskContext&,
           const FileMergerUtilityInput& input) -> FileMergerUtilityOutput {
        FileMergerUtility merger;
        return merger.process(input);
    };

    auto file_processor_batch = std::make_shared<
        BatchProcessorUtility<FileMergerUtilityInput, FileMergerUtilityOutput>>(
        file_merger_func);

    auto task1_process_files = utilities::use(file_processor_batch).as_task();
    task1_process_files->with_name("ProcessFiles");

    // ========================================================================
    // Task 2: Combine results into final output using FileCombinerUtility
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s", "Task 2: Combining results...");

    // Task 2.1: Create combiner input
    auto create_combiner_input =
        [output_file, compress_output](
            const std::vector<FileMergerUtilityOutput>& merge_results)
        -> FileCombinerUtilityInput {
        FileCombinerUtilityInput input;
        input.output_file = output_file;
        input.compress = compress_output;
        input.file_results = merge_results;

        return input;
    };

    auto task2_create_combiner_input =
        make_task(create_combiner_input, "CreateCombinerInput");

    // Task 2.2: Combine files
    auto combine_files =
        [](const FileCombinerUtilityInput& input) -> FileCombinerUtilityOutput {
        FileCombinerUtility combiner;
        return combiner.process(input);
    };

    auto task2_combine = make_task(combine_files, "CombineFiles");

    // ========================================================================
    // Task 3: Verify Merged Output (optional)
    // ========================================================================
    std::shared_ptr<Task> final_task = task2_combine;
    std::shared_ptr<Task> task3_verify = nullptr;

    if (verify) {
        DFTRACER_UTILS_LOG_INFO("%s", "Task 3: Configuring verification...");

        // Task 3.1: Create event hasher
        auto hasher =
            std::make_shared<utilities::composites::dft::EventHasher>();

        // Task 3.2: Input hasher - use the merge results which have event
        // counts We actually need to collect from original input files for
        // proper verification
        auto input_hasher = [hasher, checkpoint_size, force_override,
                             &index_dir](
                                const std::vector<FileMergerUtilityOutput>&
                                    merge_results) {
            // Create metadata from merge results to use existing collector
            std::vector<
                utilities::composites::dft::MetadataCollectorUtilityOutput>
                metadata;

            for (const auto& result : merge_results) {
                if (result.success) {
                    utilities::composites::dft::MetadataCollectorUtilityOutput
                        meta;
                    meta.file_path = result.file_path;
                    meta.success = true;
                    meta.valid_events = result.valid_events;
                    meta.start_line = 1;
                    // Use the actual total line count from the file
                    meta.end_line = result.total_lines;

                    // Determine if it's compressed and set index path
                    const std::string gz_suffix = ".gz";
                    bool is_compressed =
                        (result.file_path.size() >= gz_suffix.size() &&
                         result.file_path.compare(
                             result.file_path.size() - gz_suffix.size(),
                             gz_suffix.size(), gz_suffix) == 0);

                    if (is_compressed) {
                        meta.idx_path =
                            utilities::composites::dft::determine_index_path(
                                result.file_path, index_dir);
                    }

                    metadata.push_back(meta);
                }
            }

            // Use the existing EventCollectorFromMetadataUtility
            auto collect_input = utilities::composites::dft::
                EventCollectorFromMetadataCollectorUtilityInput::from_metadata(
                    metadata);

            utilities::composites::dft::EventCollectorFromMetadataUtility
                collector;
            auto events = collector.process(collect_input);

            auto hash_input =
                utilities::composites::dft::EventHashInput::from_events(
                    std::move(events));
            return hasher->process(hash_input);
        };

        // Task 3.3: Output event collector - reuse collected events from merge
        auto output_event_collector =
            [](TaskContext&, const FileCombinerUtilityOutput& result) {
                // Simply return the events we already collected during the
                // merge process
                return result.collected_events;
            };

        // Task 3.4: Event hasher for output
        auto event_hasher = [hasher](const std::vector<EventId>& events) {
            auto hash_input =
                utilities::composites::dft::EventHashInput::from_events(events);
            return hasher->process(hash_input);
        };

        // Task 3.5: Create chunk verifier utility
        auto verifier =
            std::make_shared<utilities::composites::ChunkVerifierUtility<
                FileCombinerUtilityOutput, FileMergerUtilityOutput, EventId>>(
                input_hasher, output_event_collector, event_hasher);

        // Task 3.6: Task definition - Use utility adapter pattern
        task3_verify = utilities::use(verifier).as_task();
        task3_verify->with_name("VerifyMerge");

        // Task 3.7: Combiner to merge output and input results for verification
        task3_verify->with_combiner(
            [](const FileCombinerUtilityOutput& output,
               const std::vector<FileMergerUtilityOutput>& inputs) {
                return utilities::composites::ChunkVerificationUtilityInput<
                           FileCombinerUtilityOutput, FileMergerUtilityOutput>::
                    from_chunks({output})  // Single "chunk" (the merged file)
                        .with_metadata(inputs);
            });

        final_task = task3_verify;
    }

    // ========================================================================
    // Set up dependencies and execute pipeline
    // ========================================================================

    // Dependencies
    task1_process_files->depends_on(task1_create_inputs);
    task2_create_combiner_input->depends_on(task1_process_files);
    task2_combine->depends_on(task2_create_combiner_input);

    if (verify && task3_verify) {
        task3_verify->depends_on(task2_combine);
        task3_verify->depends_on(task1_process_files);
    }

    // Set up pipeline
    pipeline.set_source(task1_create_inputs);
    pipeline.set_destination(final_task);

    // Execute pipeline
    pipeline.execute(input_files);

    // Get results
    auto merge_results =
        task1_process_files->get<std::vector<FileMergerUtilityOutput>>();
    auto combine_result = task2_combine->get<FileCombinerUtilityOutput>();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    // Clean up temp directory
    fs::remove_all(temp_dir);

    // Clean up temp index directory if created
    if (!temp_index_dir.empty() && fs::exists(temp_index_dir)) {
        DFTRACER_UTILS_LOG_INFO("Cleaning up temporary index directory: %s",
                                temp_index_dir.c_str());
        fs::remove_all(temp_index_dir);
    }

    // ========================================================================
    // Print Results
    // ========================================================================

    std::size_t successful_files = 0;
    std::size_t total_events = 0;
    std::size_t total_lines = 0;

    for (const auto& result : merge_results) {
        if (result.success) {
            successful_files++;
            total_events += result.valid_events;
            total_lines += result.lines_processed;
        } else {
            DFTRACER_UTILS_LOG_DEBUG("Failed to process: %s",
                                     result.file_path.c_str());
        }
    }

    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Merge Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Processed: %zu/%zu files\n", successful_files,
                merge_results.size());
    std::printf("  Total: %zu valid events from %zu lines\n", total_events,
                total_lines);
    std::printf("  Output: %s\n", combine_result.output_path.c_str());
    std::printf("  Files combined: %zu\n", combine_result.files_combined);
    std::printf("  Total events in output: %zu\n", combine_result.total_events);

    // Optional verification phase (Task 3)
    if (verify && task3_verify) {
        auto verify_result =
            task3_verify
                ->get<utilities::composites::ChunkVerificationUtilityOutput>();

        if (verify_result.input_hash == verify_result.output_hash) {
            std::printf(
                "  \u2713 Verification: PASSED - all input events present in "
                "merged output\n");
        } else {
            std::printf(
                "  \u2717 Verification: FAILED - event mismatch detected\n");
        }
        std::printf("    Input hash:  0x%016llx\n", verify_result.input_hash);
        std::printf("    Output hash: 0x%016llx\n", verify_result.output_hash);
    }

    std::printf("  Status: %s\n",
                combine_result.success ? "SUCCESS" : "FAILED");
    std::printf("==========================================\n");

    return (successful_files == merge_results.size() && combine_result.success)
               ? 0
               : 1;
}
