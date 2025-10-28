#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config_manager.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/utilities/utility_adapter.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/utilities/utilities.h>

#include <argparse/argparse.hpp>
#include <chrono>
#include <thread>

using namespace dftracer::utils;

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    auto default_checkpoint_size_str =
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE) + " B (" +
        std::to_string(Indexer::DEFAULT_CHECKPOINT_SIZE / (1024 * 1024)) +
        " MB)";

    argparse::ArgumentParser program("dftracer_event_count",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Count valid events in DFTracer .pfw or .pfw.gz files using composable "
        "utilities and pipeline processing");

    program.add_argument("-d", "--directory")
        .help("Directory containing .pfw or .pfw.gz files")
        .default_value<std::string>(".");

    program.add_argument("-f", "--force").help("Force index recreation").flag();

    program.add_argument("-c", "--checkpoint-size")
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
        .help("Number of scheduler threads (default: 1)")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(1));

    program.add_argument("--index-dir")
        .help("Directory to store index files (default: system temp directory)")
        .default_value<std::string>("");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::cerr << program;
        return 1;
    }

    // Parse arguments
    std::string log_dir = program.get<std::string>("--directory");
    bool force_rebuild = program.get<bool>("--force");
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

    // Create pipeline with configuration
    auto pipeline_config = PipelineConfigManager()
                               .with_name("DFTracer Event Count")
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
    auto index_builder_processor = [checkpoint_size, force_rebuild, &index_dir](
                                       TaskContext& /*ctx*/,
                                       const std::string& file_path)
        -> utilities::composites::dft::IndexBuildUtilityOutput {
        std::string idx_path = utilities::composites::dft::determine_index_path(
            file_path, index_dir);
        auto input =
            utilities::composites::dft::IndexBuildUtilityInput::from_file(
                file_path)
                .with_checkpoint_size(checkpoint_size)
                .with_force_rebuild(force_rebuild)
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

    // Task 2.2: Utility definition - DirectoryFileProcessorUtility with
    // MetadataCollector
    auto metadata_processor = [checkpoint_size, force_rebuild, &index_dir](
                                  TaskContext& /*ctx*/,
                                  const std::string& file_path)
        -> utilities::composites::dft::MetadataCollectorUtilityOutput {
        std::string idx_path = utilities::composites::dft::determine_index_path(
            file_path, index_dir);

        auto input = utilities::composites::dft::MetadataCollectorUtilityInput::
                         from_file(file_path)
                             .with_checkpoint_size(checkpoint_size)
                             .with_force_rebuild(force_rebuild)
                             .with_index(idx_path);

        return utilities::composites::dft::MetadataCollectorUtility{}.process(
            input);
    };

    auto metadata_workflow =
        std::make_shared<utilities::composites::DirectoryFileProcessorUtility<
            utilities::composites::dft::MetadataCollectorUtilityOutput>>(
            metadata_processor);

    // Task 2.3: Task definition - Convert utility to task
    auto task2_collect_metadata = utilities::use(metadata_workflow).as_task();
    task2_collect_metadata->with_name("CollectMetadata");

    // Task 2 needs the same directory input as Task 1
    task2_collect_metadata->with_combiner([&log_dir](const IndexBuildOutput&) {
        // Return fresh directory input for metadata collection
        return utilities::composites::DirectoryProcessInput::from_directory(
                   log_dir)
            .with_extensions({".pfw", ".pfw.gz"});
    });

    // ========================================================================
    // Task 3: Aggregate Event Counts
    // ========================================================================
    DFTRACER_UTILS_LOG_INFO("%s", "Task 3: Aggregating event counts...");

    // Task 3.1: Input - Metadata from Task 2
    using AggregateInput = MetadataCollectOutput;

    // Task 3.2: Output - Total event count
    using AggregateOutput = std::size_t;

    // Task 3.3: Utility definition - Sum up valid_events from all metadata
    auto aggregate_counts_func =
        [](const AggregateInput& batch_result) -> AggregateOutput {
        DFTRACER_UTILS_LOG_INFO("Aggregating event counts from %zu files...",
                                batch_result.results.size());

        std::size_t total_events = 0;
        for (const auto& meta : batch_result.results) {
            if (meta.success) {
                total_events += meta.valid_events;
                DFTRACER_UTILS_LOG_DEBUG("File %s: %zu events",
                                         meta.file_path.c_str(),
                                         meta.valid_events);
            } else {
                DFTRACER_UTILS_LOG_WARN("Skipping unsuccessful file: %s",
                                        meta.file_path.c_str());
            }
        }

        DFTRACER_UTILS_LOG_INFO("Total events across all files: %zu",
                                total_events);
        return total_events;
    };

    // Task 3.4: Task definition
    auto task3_aggregate_counts =
        make_task(aggregate_counts_func, "AggregateEventCounts");

    // ========================================================================
    // Execute Pipeline
    // ========================================================================

    // Define dependencies
    task2_collect_metadata->depends_on(task1_build_indexes);
    task3_aggregate_counts->depends_on(task2_collect_metadata);

    // Set up pipeline
    pipeline.set_source(task1_build_indexes);
    pipeline.set_destination(task3_aggregate_counts);

    // Execute pipeline with initial input
    pipeline.execute(index_dir_input);

    // Get final results
    auto total_events = task3_aggregate_counts->get<AggregateOutput>();
    auto metadata_results =
        task2_collect_metadata->get<MetadataCollectOutput>();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    // ========================================================================
    // Print Results
    // ========================================================================

    // Output total event count (for scripting)
    std::printf("%zu\n", total_events);

    // Log detailed statistics
    std::size_t successful_files = 0;
    for (const auto& meta : metadata_results.results) {
        if (meta.success) {
            successful_files++;
        }
    }

    DFTRACER_UTILS_LOG_DEBUG("Processed %zu files in %.2f ms", successful_files,
                             duration.count());
    DFTRACER_UTILS_LOG_DEBUG("Total valid events found: %zu", total_events);

    // Cleanup temporary index directory if created
    if (!temp_index_dir.empty() && fs::exists(temp_index_dir)) {
        DFTRACER_UTILS_LOG_INFO("Cleaning up temporary index directory: %s",
                                temp_index_dir.c_str());
        fs::remove_all(temp_index_dir);
    }

    return 0;
}
