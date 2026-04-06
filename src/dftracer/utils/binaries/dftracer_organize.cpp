#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/event_router.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::reorganize;
using namespace dftracer::utils::utilities::indexer;

namespace {

coro::CoroTask<int> run_organize(const std::string& output_dir,
                                 const std::string& index_dir,
                                 const std::vector<std::string>& files,
                                 const std::vector<PredicateGroup>& groups,
                                 std::size_t checkpoint_size,
                                 bool force_rebuild, bool no_compress,
                                 std::size_t executor_threads,
                                 std::size_t chunk_size_mb) {
    std::printf("==========================================\n");
    std::printf("DFTracer Trace Reorganizer\n");
    std::printf("==========================================\n");
    std::printf("  Input files: %zu\n", files.size());
    std::printf("  Output directory: %s\n", output_dir.c_str());
    std::printf("  Chunk size: %zu MB\n", chunk_size_mb);
    std::printf("  Compress: %s\n", no_compress ? "false" : "true");
    std::printf("  Executor threads: %zu\n", executor_threads);
    std::printf("  Groups: %zu\n", groups.size());
    for (const auto& g : groups) {
        std::printf("    %s: %s\n", g.name.c_str(),
                    g.query.empty() ? "(remainder)" : g.query.c_str());
    }
    std::printf("==========================================\n\n");

    auto start_time = std::chrono::high_resolution_clock::now();

    // Step 1: Build indices
    std::printf("Step 1: Building indices...\n");
    {
        auto pipeline_config = PipelineConfig()
                                   .with_name("Organize: Build IDX")
                                   .with_compute_threads(executor_threads)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);

        std::atomic<std::size_t> built_count{0};
        std::atomic<std::size_t> skipped_count{0};

        auto build_task = make_task(
            [&](CoroScope& ctx) -> coro::CoroTask<void> {
                co_await ctx.scope([&](CoroScope& scope)
                                       -> coro::CoroTask<void> {
                    auto* built_ptr = &built_count;
                    auto* skipped_ptr = &skipped_count;
                    for (std::size_t i = 0; i < files.size(); ++i) {
                        const auto file_path = files[i];
                        scope.spawn([file_path, index_dir, checkpoint_size,
                                     force_rebuild, built_ptr, skipped_ptr](
                                        CoroScope&) -> coro::CoroTask<void> {
                            auto config =
                                IndexBuildConfig::for_file(file_path)
                                    .with_index_dir(index_dir)
                                    .with_checkpoint_size(checkpoint_size)
                                    .with_force_rebuild(force_rebuild)
                                    .with_manifest(true)
                                    .with_index_threshold(0);

                            IndexBuilderUtility builder;
                            auto result = co_await builder.process(config);

                            if (result.was_skipped) {
                                (*skipped_ptr)++;
                            } else if (result.success) {
                                (*built_ptr)++;
                            } else {
                                DFTRACER_UTILS_LOG_ERROR(
                                    "IDX build failed for %s: %s",
                                    file_path.c_str(),
                                    result.error_message.c_str());
                            }
                            co_return;
                        });
                    }
                    co_return;
                });
                co_return;
            },
            "BuildIDX");

        pipeline.set_source(build_task);
        pipeline.set_destination(build_task);
        pipeline.execute();

        std::printf("  Built: %zu, Skipped: %zu\n", built_count.load(),
                    skipped_count.load());
    }

    // Step 2: Build extraction plan
    std::printf("Step 2: Building extraction plan...\n");
    ReorganizationPlannerUtility planner;
    ReorganizationPlannerInput planner_input;
    planner_input.source_files = files;
    planner_input.groups = groups;
    planner_input.index_dir = index_dir;
    planner_input.checkpoint_size = checkpoint_size;

    ExtractionPlan plan;
    try {
        plan = co_await planner.process(planner_input);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Planning failed: %s", e.what());
        co_return 1;
    }

    std::printf("  Groups: %zu\n", plan.groups.size());
    std::printf("  Source files: %zu\n", plan.source_files.size());
    std::printf("  Extraction tasks: %zu\n", plan.tasks.size());
    std::printf("  Total events: %zu\n", plan.total_events);

    if (plan.tasks.empty()) {
        std::printf("No events to extract.\n");
        co_return 0;
    }

    // Step 3: Route events (parallel)
    std::printf("Step 3: Routing events...\n");
    EventRouterResult router_result;
    {
        auto pipeline_config = PipelineConfig()
                                   .with_name("Organize: Route Events")
                                   .with_compute_threads(executor_threads)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);

        EventRouterConfig router_config;
        router_config.plan = std::move(plan);
        router_config.output_dir = output_dir;
        router_config.index_dir = index_dir;
        router_config.chunk_size_bytes = chunk_size_mb * 1024 * 1024;
        router_config.checkpoint_size = checkpoint_size;
        router_config.executor_threads = executor_threads;
        router_config.compress = !no_compress;

        auto* router_config_ptr = &router_config;
        auto* router_result_ptr = &router_result;

        auto route_task = make_task(
            [router_config_ptr,
             router_result_ptr](CoroScope& scope) -> coro::CoroTask<void> {
                *router_result_ptr =
                    co_await route_events(scope, *router_config_ptr);
            },
            "RouteEvents");

        pipeline.set_source(route_task);
        pipeline.set_destination(route_task);
        pipeline.execute();
    }

    std::printf("  Events written: %zu\n", router_result.total_events_written);
    std::printf("  Chunks created: %zu\n", router_result.chunks_created);
    std::printf("  Source files processed: %zu\n",
                router_result.source_files_processed);

    // Step 4: Build `.dftindex` stores for output chunk files.
    if (!router_result.output_files.empty()) {
        std::printf("Step 4: Building .dftindex stores...\n");
        auto pipeline_config = PipelineConfig()
                                   .with_name("Organize: Build Index Stores")
                                   .with_compute_threads(executor_threads)
                                   .with_watchdog(false);

        Pipeline pipeline(pipeline_config);

        auto* output_files_ptr = &router_result.output_files;

        auto index_store_task = make_task(
            [output_files_ptr, output_dir,
             checkpoint_size](CoroScope& ctx) -> coro::CoroTask<void> {
                co_await ctx.scope(
                    [&](CoroScope& scope) -> coro::CoroTask<void> {
                        for (const auto& out_file : *output_files_ptr) {
                            scope.spawn([out_file, checkpoint_size](CoroScope&)
                                            -> coro::CoroTask<void> {
                                auto config =
                                    IndexBuildConfig::for_file(out_file)
                                        .with_index_dir("")
                                        .with_checkpoint_size(checkpoint_size)
                                        .with_force_rebuild(true)
                                        .with_manifest(true)
                                        .with_index_threshold(0);

                                IndexBuilderUtility builder;
                                co_await builder.process(config);
                                co_return;
                            });
                        }
                        co_return;
                    });
                co_return;
            },
            "BuildSidecars");

        pipeline.set_source(index_store_task);
        pipeline.set_destination(index_store_task);
        pipeline.execute();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::printf("\n==========================================\n");
    std::printf("Reorganization Complete\n");
    std::printf("==========================================\n");
    std::printf("  Time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Input files: %zu\n", files.size());
    std::printf("  Events routed: %zu\n", router_result.total_events_written);
    std::printf("  Chunks created: %zu\n", router_result.chunks_created);
    std::printf("  Output files:\n");
    for (const auto& f : router_result.output_files) {
        if (fs::exists(f)) {
            std::printf(
                "    %s (%.2f MB)\n", f.c_str(),
                static_cast<double>(fs::file_size(f)) / (1024.0 * 1024.0));
        }
    }
    std::printf("==========================================\n");

    co_return router_result.success ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_organize",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Reorganize DFTracer trace files by routing events to "
        "predicate-based groups with chunked output.");

    program.add_argument("--files")
        .help("Input trace files (.pfw, .pfw.gz)")
        .nargs(argparse::nargs_pattern::any)
        .default_value<std::vector<std::string>>({});

    program.add_argument("-d", "--directory")
        .help("Directory containing trace files")
        .default_value<std::string>("");

    program.add_argument("-o", "--output").help("Output directory").required();

    program.add_argument("--groups")
        .help(
            "Predicate groups: \"io:cat==\\\"POSIX\\\"\" "
            "\"compute:cat==\\\"APP\\\"\"")
        .nargs(argparse::nargs_pattern::at_least_one)
        .required();

    program.add_argument("--chunk-size")
        .help("Target chunk size in MB (default: 256)")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(256));

    program.add_argument("--checkpoint-size")
        .help("Checkpoint size for indexing in bytes")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(
            indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--index-dir")
        .help("Directory for .dftindex stores")
        .default_value<std::string>("");

    program.add_argument("-f", "--force")
        .help("Force rebuild of indices")
        .flag();

    program.add_argument("--no-compress")
        .help("Write plain .pfw instead of .pfw.gz")
        .flag();

    program.add_argument("--executor-threads")
        .help("Worker threads")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(dftracer_utils_hardware_concurrency()));

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
        std::cerr << program;
        return 1;
    }

    std::string directory = program.get<std::string>("--directory");
    std::string output_dir = program.get<std::string>("--output");
    std::string index_dir = program.get<std::string>("--index-dir");
    auto group_specs = program.get<std::vector<std::string>>("--groups");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    std::size_t chunk_size_mb = program.get<std::size_t>("--chunk-size");
    bool force_rebuild = program.get<bool>("--force");
    bool no_compress = program.get<bool>("--no-compress");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");

    fs::create_directories(output_dir);

    auto groups = parse_group_specs(group_specs);
    if (groups.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s", "No groups specified.");
        return 1;
    }

    std::vector<std::string> files;
    if (!directory.empty()) {
        if (!fs::exists(directory)) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                     directory.c_str());
            return 1;
        }
        filesystem::PatternDirectoryScannerUtility scanner;
        filesystem::PatternDirectoryScannerUtilityInput scan_input{
            directory, {".pfw", ".pfw.gz"}, false};
        auto matched = scanner.process(scan_input).get();
        for (const auto& entry : matched) {
            files.push_back(entry.path.string());
        }
    } else {
        files = program.get<std::vector<std::string>>("--files");
    }

    if (files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "No input files. Use --files or --directory.");
        return 1;
    }

    return run_organize(output_dir, index_dir, files, groups, checkpoint_size,
                        force_rebuild, no_compress, executor_threads,
                        chunk_size_mb)
        .get();
}
