#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <unistd.h>

#include <argparse/argparse.hpp>
#include <chrono>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::indexer::internal;

static coro::CoroTask<int> run_event_count(argparse::ArgumentParser& program);

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
            static_cast<std::size_t>(dftracer_utils_hardware_concurrency()));

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

    return run_event_count(program).get();
}

static coro::CoroTask<int> run_event_count(argparse::ArgumentParser& program) {
    // Parse arguments
    std::string log_dir = program.get<std::string>("--directory");
    bool force_rebuild = program.get<bool>("--force");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    std::string index_dir = program.get<std::string>("--index-dir");

    // If no index dir specified, indices are stored next to trace files
    // (default IndexBuilderUtility behavior). This allows reuse of
    // indices built by dftracer_index.

    log_dir = fs::absolute(log_dir).string();

    // Discover input files
    utilities::filesystem::PatternDirectoryScannerUtility scanner;
    utilities::filesystem::PatternDirectoryScannerUtilityInput scan_input{
        log_dir, {".pfw", ".pfw.gz"}};
    auto matched_entries = scanner.process(scan_input).get();

    std::vector<std::string> input_files;
    input_files.reserve(matched_entries.size());
    for (const auto& entry : matched_entries) {
        input_files.push_back(entry.path.string());
    }

    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in: %s",
                                 log_dir.c_str());
        co_return 1;
    }

    auto pipeline_config = PipelineConfig()
                               .with_name("DFTracer Event Count")
                               .with_compute_threads(executor_threads)
                               .with_watchdog(false);

    Pipeline pipeline(pipeline_config);

    auto start_time = std::chrono::high_resolution_clock::now();

    std::atomic<std::size_t> total_events{0};
    std::atomic<std::size_t> files_processed{0};
    std::atomic<bool> is_approximate{false};

    auto count_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                auto* files_ptr = &input_files;
                auto* total_events_ptr = &total_events;
                auto* files_processed_ptr = &files_processed;
                auto* is_approximate_ptr = &is_approximate;
                auto file_chan =
                    coro::make_channel<std::size_t>(executor_threads * 2);

                // Producer
                scope.spawn([ch = file_chan->producer(),
                             num_files = input_files.size()](
                                CoroScope&) mutable -> coro::CoroTask<void> {
                    auto guard = ch.guard();
                    for (std::size_t i = 0; i < num_files; ++i) {
                        if (!co_await ch.send(i)) co_return;
                    }
                    co_return;
                });

                // Workers: build index if needed, then read count from DB
                for (std::size_t w = 0; w < executor_threads; ++w) {
                    scope.spawn([file_chan, files_ptr, checkpoint_size,
                                 force_rebuild, &index_dir, total_events_ptr,
                                 files_processed_ptr, is_approximate_ptr](
                                    CoroScope&) -> coro::CoroTask<void> {
                        while (auto fi_opt = co_await file_chan->receive()) {
                            const auto& fp = (*files_ptr)[*fi_opt];

                            // Build index if needed
                            utilities::indexer::IndexBuilderUtility builder;
                            auto config =
                                utilities::indexer::IndexBuildConfig::for_file(
                                    fp)
                                    .with_checkpoint_size(checkpoint_size)
                                    .with_force_rebuild(force_rebuild)
                                    .with_index_dir(index_dir);
                            co_await builder.process(config);

                            // Read event count from index
                            std::string index_path =
                                fp + constants::indexer::EXTENSION;
                            if (!index_dir.empty()) {
                                auto fname = fs::path(fp).filename();
                                index_path =
                                    (fs::path(index_dir) / fname).string() +
                                    constants::indexer::EXTENSION;
                            }

                            if (fs::exists(index_path)) {
                                try {
                                    utilities::indexer::IndexDatabase db(
                                        index_path);
                                    int fid = db.find_file(fp);
                                    if (fid >= 0) {
                                        if (!db.has_bloom_data(fid)) {
                                            is_approximate_ptr->store(
                                                true,
                                                std::memory_order_relaxed);
                                        }
                                        total_events_ptr->fetch_add(
                                            db.get_total_events(fid),
                                            std::memory_order_relaxed);
                                        files_processed_ptr->fetch_add(
                                            1, std::memory_order_relaxed);
                                        continue;
                                    }
                                } catch (...) {
                                }
                            }

                            // Fallback for small/unindexed files:
                            // stream decompress and count lines (approximate)
                            is_approximate_ptr->store(
                                true, std::memory_order_relaxed);
                            {
                                using utilities::fileio::lines::sources::
                                    async_streaming_gz_lines;
                                std::size_t count = 0;
                                auto gen = async_streaming_gz_lines(fp);
                                while (co_await gen.next()) {
                                    ++count;
                                }
                                total_events_ptr->fetch_add(
                                    count, std::memory_order_relaxed);
                                files_processed_ptr->fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                        }
                        co_return;
                    });
                }
                co_return;
            });
            co_return;
        },
        "EventCount");

    pipeline.set_source(count_task);
    pipeline.set_destination(count_task);
    pipeline.execute();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    if (is_approximate.load()) {
        std::printf("~%zu\n", total_events.load());
    } else {
        std::printf("%zu\n", total_events.load());
    }

    DFTRACER_UTILS_LOG_DEBUG("Completed in %.2f ms", duration.count());
    DFTRACER_UTILS_LOG_DEBUG("Files processed: %zu", files_processed.load());

    co_return 0;
}
