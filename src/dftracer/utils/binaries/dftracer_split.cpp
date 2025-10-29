#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/executors/executor_factory.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/task_tag.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/reader/line_processor.h>
#include <dftracer/utils/reader/reader.h>
#include <dftracer/utils/reader/reader_factory.h>
#include <dftracer/utils/utilities/composites/composites.h>
#include <dftracer/utils/utilities/io/types/types.h>
#include <xxhash.h>
#include <yyjson.h>
#include <zlib.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

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
        "Split DFTracer traces into equal-sized chunks using pipeline "
        "processing");

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

    program.add_argument("--threads")
        .help(
            "Number of threads for parallel processing (default: number of CPU "
            "cores)")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

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

    std::string app_name = program.get<std::string>("--app-name");
    std::string log_dir = program.get<std::string>("--directory");
    std::string output_dir = program.get<std::string>("--output");
    int chunk_size_mb = program.get<int>("--chunk-size");
    bool force = program.get<bool>("--force");
    bool compress = program.get<bool>("--compress");
    bool verify = program.get<bool>("--verify");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t num_threads = program.get<std::size_t>("--threads");
    std::string index_dir = program.get<std::string>("--index-dir");

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
    std::printf("Arguments:\n");
    std::printf("  App name: %s\n", app_name.c_str());
    std::printf("  Override: %s\n", force ? "true" : "false");
    std::printf("  Compress: %s\n", compress ? "true" : "false");
    std::printf("  Data dir: %s\n", log_dir.c_str());
    std::printf("  Output dir: %s\n", output_dir.c_str());
    std::printf("  Chunk size: %d MB\n", chunk_size_mb);
    std::printf("  Threads: %zu\n", num_threads);
    std::printf("==========================================\n");

    if (!fs::exists(output_dir)) {
        fs::create_directories(output_dir);
    }

    auto executor = dftracer::utils::ExecutorFactory::create(num_threads);
    auto start_time = std::chrono::high_resolution_clock::now();

    // Phase 0: Build indexes sequentially (avoid SQLite write conflicts)
    DFTRACER_UTILS_LOG_INFO("%s", "Phase 0: Building indexes...");

    auto index_builder_processor = [checkpoint_size, force, &index_dir](
                                       const std::string& file_path,
                                       TaskContext& /*ctx*/)
        -> utilities::composites::dft::IndexBuildUtilityOutput {
        std::string idx_path = utilities::composites::dft::determine_index_path(
            file_path, index_dir);
        auto input =
            utilities::composites::dft::IndexBuildUtilityInput::from_file(
                file_path)
                .with_checkpoint_size(checkpoint_size)
                .with_force_rebuild(force)
                .with_index(idx_path);
        utilities::composites::dft::IndexBuilderUtility builder;
        return builder.process(input);
    };

    auto index_workflow =
        std::make_shared<utilities::composites::DirectoryFileProcessorUtility<
            utilities::composites::dft::IndexBuildUtilityOutput>>(
            index_builder_processor);

    auto index_dir_input =
        utilities::composites::DirectoryProcessInput::from_directory(log_dir)
            .with_extensions({".pfw", ".pfw.gz"});

    Pipeline index_pipeline("Index Building");
    auto index_task = utilities::use(index_workflow).emit_on(index_pipeline);

    // Use 1 thread for index building to avoid SQLite conflicts
    PipelineOutput index_output =
        executor->execute(index_pipeline, index_dir_input);
    auto index_result =
        index_output.get<utilities::composites::BatchFileProcessOutput<
            utilities::composites::dft::IndexBuildUtilityOutput>>(
            index_task.id());

    std::size_t indexes_built = 0;
    for (const auto& result : index_result.results) {
        if (result.success && result.was_built) {
            indexes_built++;
        }
    }

    DFTRACER_UTILS_LOG_INFO("Built %zu new indexes", indexes_built);

    // Phase 1: Collect metadata in parallel using DirectoryFileProcessor
    DFTRACER_UTILS_LOG_INFO("%s", "Phase 1: Collecting file metadata...");

    auto metadata_processor = [checkpoint_size, force, &index_dir](
                                  const std::string& file_path,
                                  TaskContext& /*ctx*/)
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

    auto workflow =
        std::make_shared<utilities::composites::DirectoryFileProcessorUtility<
            utilities::composites::dft::MetadataCollectorUtilityOutput>>(
            metadata_processor);

    auto dir_input =
        utilities::composites::DirectoryProcessInput::from_directory(log_dir)
            .with_extensions({".pfw", ".pfw.gz"});

    Pipeline metadata_pipeline("Metadata Collection");
    auto task = utilities::use(workflow).emit_on(metadata_pipeline);

    PipelineOutput output = executor->execute(metadata_pipeline, dir_input);
    auto batch_result =
        output.get<utilities::composites::BatchFileProcessOutput<
            utilities::composites::dft::MetadataCollectorUtilityOutput>>(
            task.id());

    if (batch_result.results.empty()) {
        DFTRACER_UTILS_LOG_ERROR(
            "No .pfw or .pfw.gz files found in directory: %s", log_dir.c_str());
        return 1;
    }

    DFTRACER_UTILS_LOG_INFO("Found %zu files to process",
                            batch_result.results.size());
    std::vector<utilities::composites::dft::MetadataCollectorUtilityOutput>
        all_metadata = std::move(batch_result.results);

    std::size_t successful_files = 0;
    double total_size_mb = 0;
    for (const auto& meta : all_metadata) {
        if (meta.success) {
            successful_files++;
            total_size_mb += meta.size_mb;
        }
    }

    DFTRACER_UTILS_LOG_INFO(
        "Collected metadata from %zu/%zu files, total size: %.2f MB",
        successful_files, all_metadata.size(), total_size_mb);

    if (successful_files == 0) {
        DFTRACER_UTILS_LOG_ERROR("%s", "No files were successfully processed");
        return 1;
    }

    // Phase 2: Create chunk mappings using DFTracerChunkManifestMapper workflow
    DFTRACER_UTILS_LOG_INFO("%s", "Phase 2: Creating chunk mappings...");

    utilities::composites::dft::ChunkManifestMapperUtility mapper;
    auto mapper_input =
        utilities::composites::dft::ChunkManifestMapperUtilityInput::
            from_metadata(all_metadata)
                .with_target_size(static_cast<double>(chunk_size_mb));

    std::vector<utilities::composites::dft::DFTracerChunkManifest> manifests =
        mapper.process(mapper_input);

    DFTRACER_UTILS_LOG_INFO("Created %zu chunks with line tracking:",
                            manifests.size());

#if DFTRACER_UTILS_LOGGER_DEBUG_ENABLED == 1
    for (std::size_t i = 0; i < manifests.size(); ++i) {
        DFTRACER_UTILS_LOG_DEBUG(
            "  Chunk %zu: %.2f MB, %zu lines across %zu files", i + 1,
            manifests[i].total_size_mb, manifests[i].total_lines(),
            manifests[i].specs.size());
        for (const auto& spec : manifests[i].specs) {
            if (spec.has_line_info()) {
                DFTRACER_UTILS_LOG_DEBUG(
                    "    %s: lines %zu-%zu (bytes %zu-%zu)",
                    fs::path(spec.file_path).filename().c_str(),
                    spec.start_line, spec.end_line, spec.start_byte,
                    spec.end_byte);
            }
        }
    }
#endif

    // Create ChunkExtractorUtilityInput with DFTracerChunkManifest (includes
    // line tracking)
    std::vector<utilities::composites::dft::ChunkExtractorUtilityInput>
        chunk_inputs;
    for (int i = 0; i < static_cast<int>(manifests.size()); ++i) {
        auto input = utilities::composites::dft::ChunkExtractorUtilityInput::
                         from_manifest(i + 1, manifests[i])
                             .with_output_dir(output_dir)
                             .with_app_name(app_name)
                             .with_compression(compress);
        chunk_inputs.push_back(input);
    }

    if (chunk_inputs.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s", "No chunks created");
        return 1;
    }

    // Phase 3: Extract chunks in parallel using DFTracerChunkExtractor workflow
    DFTRACER_UTILS_LOG_INFO("%s", "Phase 3: Extracting chunks...");

    // Use BatchProcessor with comparator to automatically sort results by
    // chunk_index
    auto extractor_workflow =
        std::make_shared<utilities::composites::dft::ChunkExtractorUtility>();
    auto chunk_extractor =
        std::make_shared<utilities::composites::BatchProcessorUtility<
            utilities::composites::dft::ChunkExtractorUtilityInput,
            utilities::composites::dft::ChunkExtractorUtilityOutput>>(
            extractor_workflow);

    chunk_extractor->with_comparator(
        [](const utilities::composites::dft::ChunkExtractorUtilityOutput& a,
           const utilities::composites::dft::ChunkExtractorUtilityOutput& b) {
            return a.chunk_index < b.chunk_index;
        });

    Pipeline extract_pipeline("Chunk Extraction");
    auto extract_task =
        utilities::use(chunk_extractor).emit_on(extract_pipeline);

    PipelineOutput extract_output =
        executor->execute(extract_pipeline, chunk_inputs);
    std::vector<utilities::composites::dft::ChunkExtractorUtilityOutput>
        results = extract_output.get<std::vector<
            utilities::composites::dft::ChunkExtractorUtilityOutput>>(
            extract_task.id());

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::size_t successful_chunks = 0;
    std::size_t total_events = 0;

    for (const auto& result : results) {
        if (result.success) {
            successful_chunks++;
            total_events += result.events;
        } else {
            DFTRACER_UTILS_LOG_ERROR("Failed to create chunk %d",
                                     result.chunk_index);
        }
    }

    std::printf("\n");
    std::printf("Split completed in %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Input: %zu files, %.2f MB\n", successful_files,
                total_size_mb);
    std::printf("  Output: %zu/%zu chunks, %zu events\n", successful_chunks,
                results.size(), total_events);

    if (verify) {
        auto verify_start = std::chrono::high_resolution_clock::now();
        std::printf("  Validating output chunks match input...\n");

        auto hasher =
            std::make_shared<utilities::composites::dft::EventHasher>();

        auto input_hasher =
            [&hasher](
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

        // Event collector: use event IDs from extraction results
        auto event_collector =
            [](const utilities::composites::dft::ChunkExtractorUtilityOutput&
                   result,
               TaskContext&) {
                return result.event_ids;  // Just return the pre-collected IDs
            };

        // Event hasher: hash collected events
        auto event_hasher = [&hasher](const std::vector<EventId>& events) {
            auto hash_input =
                utilities::composites::dft::EventHashInput::from_events(events);
            return hasher->process(hash_input);
        };

        auto verifier =
            std::make_shared<utilities::composites::ChunkVerifierUtility<
                utilities::composites::dft::ChunkExtractorUtilityOutput,
                utilities::composites::dft::MetadataCollectorUtilityOutput,
                EventId>>(input_hasher, event_collector, event_hasher);

        auto verify_input =
            utilities::composites::ChunkVerificationUtilityInput<
                utilities::composites::dft::ChunkExtractorUtilityOutput,
                utilities::composites::dft::MetadataCollectorUtilityOutput>::
                from_chunks(results)
                    .with_metadata(all_metadata);

        Pipeline verify_pipeline("Chunk Verification");
        auto verify_task = utilities::use(verifier).emit_on(verify_pipeline);

        PipelineOutput verify_output =
            executor->execute(verify_pipeline, verify_input);
        auto verify_result =
            verify_output
                .get<utilities::composites::ChunkVerificationUtilityOutput>(
                    verify_task.id());

        auto verify_end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> verify_duration =
            verify_end - verify_start;

        if (verify_result.passed) {
            std::printf(
                "  \u2713 Verification: PASSED - all events present in output "
                "(%.2f seconds)\n",
                verify_duration.count() / 1000.0);
            std::printf("    Input hash:  %016llx, Output hash: %016llx\n",
                        (unsigned long long)verify_result.input_hash,
                        (unsigned long long)verify_result.output_hash);
        } else {
            std::printf(
                "  \u2717 Verification: FAILED - event mismatch detected (%.2f "
                "seconds)\n",
                verify_duration.count() / 1000.0);
            DFTRACER_UTILS_LOG_ERROR(
                "Hash mismatch: input=%016llx output=%016llx",
                (unsigned long long)verify_result.input_hash,
                (unsigned long long)verify_result.output_hash);
        }
    }

    std::printf("All chunks processed in %.2f ms\n", duration.count());

    // Cleanup temporary index directory if created
    if (!temp_index_dir.empty() && fs::exists(temp_index_dir)) {
        DFTRACER_UTILS_LOG_INFO("Cleaning up temporary index directory: %s",
                                temp_index_dir.c_str());
        fs::remove_all(temp_index_dir);
    }

    return successful_chunks == results.size() ? 0 : 1;
}
