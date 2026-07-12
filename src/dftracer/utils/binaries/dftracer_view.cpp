#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/resolve_and_build.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::views;
using namespace dftracer::utils::utilities::filesystem;
using dftracer::utils::utilities::indexer::IndexBatchBuilderUtility;
using dftracer::utils::utilities::indexer::IndexBuildBatchConfig;

class ViewArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{cli::DirMode::DEFAULT_EMPTY};
    cli::FilesArgs files_args;
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;
    cli::QueryArgs query_args;

    std::string preset;
    std::string recipe;
    std::string save_recipe;
    std::string time_range;
    double min_duration = 0.0;
    double max_duration = 0.0;
    std::string output;
    bool stream = false;
    bool no_metadata = false;
    bool no_auto_index = false;

    explicit ViewArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.with_force = false;
        indexing.index_dir_help =
            "Directory where .dftindex stores are created";
        schema(directory, files_args, pipeline, indexing, query_args);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("--preset")
            .help("Predefined view: io, compute, dlio")
            .default_value<std::string>("");

        parser()
            .add_argument("--recipe")
            .help("Custom view JSON file path")
            .default_value<std::string>("");

        parser()
            .add_argument("--save-recipe")
            .help("Save the constructed view to a JSON file")
            .default_value<std::string>("");

        parser()
            .add_argument("--time-range")
            .help(
                "Timestamp filter as min,max in microseconds (e.g., "
                "1000000,2000000)")
            .default_value<std::string>("");

        parser()
            .add_argument("--min-duration")
            .help("Minimum event duration in microseconds")
            .scan<'g', double>()
            .default_value(static_cast<double>(0.0));

        parser()
            .add_argument("--max-duration")
            .help("Maximum event duration in microseconds")
            .scan<'g', double>()
            .default_value(static_cast<double>(0.0));

        parser()
            .add_argument("-o", "--output")
            .help("Output file path (default: stdout)")
            .default_value<std::string>("");

        parser()
            .add_argument("--stream")
            .help("Stream matching events to stdout as NDJSON")
            .flag();

        parser()
            .add_argument("--no-metadata")
            .help("Exclude metadata events (ph=M) from output")
            .flag();

        parser()
            .add_argument("--no-auto-index")
            .help(
                "Disable automatic index building for files missing .dftindex")
            .flag();
    }

    void post_parse() override {
        preset = parser().get<std::string>("--preset");
        recipe = parser().get<std::string>("--recipe");
        save_recipe = parser().get<std::string>("--save-recipe");
        time_range = parser().get<std::string>("--time-range");
        min_duration = parser().get<double>("--min-duration");
        max_duration = parser().get<double>("--max-duration");
        output = parser().get<std::string>("--output");
        stream = parser().get<bool>("--stream");
        no_metadata = parser().get<bool>("--no-metadata");
        no_auto_index = parser().get<bool>("--no-auto-index");
    }
};

struct ViewContext {
    std::string index_dir;
    std::size_t checkpoint_size;
    ViewDefinition view;
    bool stream_mode;
    FILE* out_file;
    std::mutex* output_mutex;
    std::vector<std::string>* all_events;
    std::atomic<std::uint64_t>* total_events_matched;
    std::atomic<std::uint64_t>* total_events_scanned;
    std::atomic<std::uint64_t>* total_chunks_scanned;
    std::atomic<std::uint64_t>* total_chunks_skipped;
    std::atomic<std::size_t>* indexed_count;
    std::atomic<std::size_t>* failed_count;
};

static coro::CoroTask<void> batch_index_files(
    const std::vector<std::string>& files_needing_index,
    const ViewContext& vctx, CoroScope& ctx) {
    auto batch_config = std::make_shared<IndexBuildBatchConfig>();
    batch_config->file_paths = files_needing_index;
    batch_config->index_dir = vctx.index_dir;
    batch_config->checkpoint_size = vctx.checkpoint_size;
    batch_config->parallelism =
        std::max<std::size_t>(1, files_needing_index.size());
    batch_config->use_batch_write = true;
    batch_config->rebuild_root_summaries = true;

    auto batch_result = co_await IndexBatchBuilderUtility::process(
        &ctx, std::move(batch_config));

    for (const auto& result : batch_result.results) {
        if (result.success) {
            (*vctx.indexed_count)++;
        } else {
            (*vctx.failed_count)++;
            if (!result.error_message.empty()) {
                DFTRACER_UTILS_LOG_ERROR("Auto-indexing failed for %s: %s",
                                         result.file_path.c_str(),
                                         result.error_message.c_str());
            }
        }
    }
}

static coro::CoroTask<void> read_single_chunk(
    const std::string& file_path, const std::string& index_path,
    const ViewChunkCandidate& candidate, const ViewContext& vctx, CoroScope&) {
    ViewReaderInput reader_input;
    reader_input.with_file_path(file_path)
        .with_index_path(index_path)
        .with_checkpoint_size(vctx.checkpoint_size)
        .with_byte_range(candidate.start_byte, candidate.end_byte)
        .with_checkpoint_idx(candidate.checkpoint_idx)
        .with_view(vctx.view);
    reader_input.query = vctx.view.query;

    ViewReaderUtility reader;
    auto gen = reader.process(reader_input);
    while (auto batch = co_await gen.next()) {
        (*vctx.total_events_matched) += batch->events_matched;
        (*vctx.total_events_scanned) += batch->events_scanned;

        if (vctx.stream_mode) {
            std::lock_guard<std::mutex> lock(*vctx.output_mutex);
            for (const auto& event : batch->events) {
                if (vctx.out_file) {
                    std::fprintf(vctx.out_file, "%.*s\n",
                                 static_cast<int>(event.size()), event.data());
                } else {
                    std::printf("%.*s\n", static_cast<int>(event.size()),
                                event.data());
                }
            }
        } else {
            std::lock_guard<std::mutex> lock(*vctx.output_mutex);
            for (const auto& event : batch->events) {
                vctx.all_events->emplace_back(event);
            }
        }
    }
    (*vctx.total_chunks_scanned)++;
}

static coro::CoroTask<void> process_single_file(const std::string& file_path,
                                                const ViewContext& vctx,
                                                CoroScope& fctx) {
    std::string index_path =
        internal::determine_index_path(file_path, vctx.index_dir);

    auto meta_input = MetadataCollectorUtilityInput::from_file(file_path)
                          .with_checkpoint_size(vctx.checkpoint_size)
                          .with_force_rebuild(false)
                          .with_index(index_path);
    auto metadata = co_await MetadataCollectorUtility{}.process(meta_input);

    if (!metadata.success) {
        DFTRACER_UTILS_LOG_ERROR("Failed to collect metadata for %s: %s",
                                 file_path.c_str(),
                                 metadata.error_message.c_str());
        co_return;
    }

    ViewBuilderInput builder_input;
    builder_input.with_view(vctx.view)
        .with_file_path(file_path)
        .with_index_path(fs::exists(index_path) ? index_path : "")
        .with_uncompressed_size(metadata.uncompressed_size)
        .with_num_checkpoints(metadata.num_checkpoints);

    ViewBuilderUtility builder;
    auto build_output = co_await builder.process(builder_input);

    if (!build_output) {
        DFTRACER_UTILS_LOG_ERROR("ViewBuilder failed for %s",
                                 file_path.c_str());
        co_return;
    }

    (*vctx.total_chunks_skipped) += build_output->skipped_checkpoints;

    if (!build_output->file_may_match) {
        co_return;
    }

    auto& candidates = build_output->candidates;
    co_await fctx.scope([&file_path, &index_path, &vctx, &candidates](
                            CoroScope& chunk_scope) -> coro::CoroTask<void> {
        for (const auto& candidate : candidates) {
            chunk_scope.spawn([&file_path, &index_path, &candidate,
                               &vctx](CoroScope& cctx) -> coro::CoroTask<void> {
                co_await read_single_chunk(file_path, index_path, candidate,
                                           vctx, cctx);
            });
        }
        co_return;
    });
}

static coro::CoroTask<int> run_view(const ViewArgParse* cli) {
    const auto& directory = cli->directory.value;
    const auto& index_dir = cli->indexing.index_dir;
    const auto& preset = cli->preset;
    const auto& recipe_path = cli->recipe;
    const auto& save_recipe = cli->save_recipe;
    const auto& output_path = cli->output;
    const auto& time_range_str = cli->time_range;
    const auto min_duration = cli->min_duration;
    const auto max_duration = cli->max_duration;
    const auto stream_mode = cli->stream;
    const auto no_metadata = cli->no_metadata;
    const auto no_auto_index = cli->no_auto_index;
    const auto checkpoint_size = cli->indexing.checkpoint_size;
    const auto& query_str = cli->query_args.query;

    ViewDefinition view;

    if (!preset.empty()) {
        if (preset == "io") {
            view = ViewDefinition::io_view();
        } else if (preset == "compute") {
            view = ViewDefinition::compute_view();
        } else if (preset == "dlio") {
            view = ViewDefinition::dlio_view();
        } else {
            DFTRACER_UTILS_LOG_ERROR(
                "Unknown preset: %s. Use: io, compute, "
                "dlio",
                preset.c_str());
            co_return 1;
        }
    } else if (!recipe_path.empty()) {
        if (!fs::exists(recipe_path)) {
            DFTRACER_UTILS_LOG_ERROR("Recipe file not found: %s",
                                     recipe_path.c_str());
            co_return 1;
        }
        std::ifstream recipe_file(recipe_path);
        std::string json_content((std::istreambuf_iterator<char>(recipe_file)),
                                 std::istreambuf_iterator<char>());
        view = ViewDefinition::from_json(json_content);
    } else {
        view.name = "custom";
        view.description = "Custom inline view";
    }

    using common::query::Query;
    std::optional<Query> query;
    if (!query_str.empty()) {
        auto result = Query::from_string(query_str);
        if (!result) {
            DFTRACER_UTILS_LOG_ERROR("Invalid --query: %s",
                                     result.error().format().c_str());
            co_return 1;
        }
        query = std::move(*result);
    }

    std::optional<std::pair<double, double>> time_range;
    if (!time_range_str.empty()) {
        auto comma = time_range_str.find(',');
        if (comma != std::string::npos) {
            double min_ts = std::stod(time_range_str.substr(0, comma));
            double max_ts = std::stod(time_range_str.substr(comma + 1));
            time_range = std::make_pair(min_ts, max_ts);
        } else {
            DFTRACER_UTILS_LOG_ERROR(
                "Invalid --time-range format. Use: min,max (e.g., "
                "1000000,2000000)");
            co_return 1;
        }
    }

    if (time_range || min_duration > 0 || max_duration > 0) {
        std::string extra;
        if (time_range) {
            extra += "ts >= " +
                     std::to_string(static_cast<uint64_t>(time_range->first));
            extra += " and ts <= " +
                     std::to_string(static_cast<uint64_t>(time_range->second));
        }
        if (min_duration > 0) {
            if (!extra.empty()) extra += " and ";
            extra +=
                "dur >= " + std::to_string(static_cast<uint64_t>(min_duration));
        }
        if (max_duration > 0) {
            if (!extra.empty()) extra += " and ";
            extra +=
                "dur <= " + std::to_string(static_cast<uint64_t>(max_duration));
        }
        if (query) {
            std::string combined = "(" + query->source() + ") and " + extra;
            query = common::query::parse_or_throw(combined);
        } else {
            query = common::query::parse_or_throw(extra);
        }
    }

    if (query) {
        view.with_query(std::move(*query));
    }

    if (no_metadata) {
        view.with_include_metadata(false);
    }

    if (!view.query) {
        DFTRACER_UTILS_LOG_ERROR(
            "%s", "No view specified. Use --preset, --recipe, or --query.");
        co_return 1;
    }

    if (!save_recipe.empty()) {
        std::ofstream out(save_recipe);
        out << view.to_json();
        out.close();
        std::printf("View recipe saved to: %s\n", save_recipe.c_str());
    }

    std::vector<std::string> files;
    if (!directory.empty()) {
        if (!fs::exists(directory)) {
            DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                     directory.c_str());
            co_return 1;
        }

        PatternDirectoryScannerUtility scanner;
        PatternDirectoryScannerUtilityInput scan_input{
            directory, {".pfw", ".pfw.gz"}, false};
        auto matched = co_await scanner.process(scan_input);

        for (const auto& entry : matched) {
            files.push_back(entry.path.string());
        }

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in: %s",
                                     directory.c_str());
            co_return 1;
        }
    } else {
        files = cli->files_args.value;

        if (files.empty()) {
            DFTRACER_UTILS_LOG_ERROR(
                "%s", "No files or directory specified. Use --help for usage.");
            co_return 1;
        }
    }

    std::vector<std::string> files_needing_index;
    for (const auto& file_path : files) {
        std::string index_path =
            internal::determine_index_path(file_path, index_dir);
        if (!fs::exists(index_path)) {
            files_needing_index.push_back(file_path);
        }
    }

    if (!files_needing_index.empty()) {
        if (no_auto_index) {
            DFTRACER_UTILS_LOG_ERROR(
                "Missing .dftindex store for %zu file(s) and --no-auto-index "
                "is "
                "set. Run dftracer_index first.",
                files_needing_index.size());
            for (const auto& f : files_needing_index) {
                std::fprintf(stderr, "  Missing index: %s\n", f.c_str());
            }
            co_return 1;
        }

        std::printf("Auto-building index for %zu file(s)...\n",
                    files_needing_index.size());
    }

    std::mutex output_mutex;
    std::vector<std::string> all_events;
    std::atomic<std::uint64_t> total_events_matched{0};
    std::atomic<std::uint64_t> total_events_scanned{0};
    std::atomic<std::uint64_t> total_chunks_scanned{0};
    std::atomic<std::uint64_t> total_chunks_skipped{0};
    std::atomic<std::size_t> indexed_count{0};
    std::atomic<std::size_t> failed_count{0};

    FILE* out_file = nullptr;
    if (!output_path.empty()) {
        out_file = std::fopen(output_path.c_str(), "w");
        if (!out_file) {
            DFTRACER_UTILS_LOG_ERROR("Failed to open output file: %s",
                                     output_path.c_str());
            co_return 1;
        }
    }

    ViewContext vctx{index_dir,
                     checkpoint_size,
                     view,
                     stream_mode,
                     out_file,
                     &output_mutex,
                     &all_events,
                     &total_events_matched,
                     &total_events_scanned,
                     &total_chunks_scanned,
                     &total_chunks_skipped,
                     &indexed_count,
                     &failed_count};

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer View", cli->pipeline);

    Pipeline pipeline(pipeline_config);

    auto* files_needing_index_ptr = &files_needing_index;
    auto* files_ptr = &files;

    auto combined_task = make_task(
        [files_needing_index_ptr, files_ptr, no_auto_index,
         &vctx](CoroScope& ctx) -> coro::CoroTask<void> {
            if (!no_auto_index) {
                co_await indexing::ensure_indexes_fresh(&ctx, "", *files_ptr,
                                                        vctx.index_dir);
            }

            if (!files_needing_index_ptr->empty()) {
                co_await batch_index_files(*files_needing_index_ptr, vctx, ctx);

                std::printf("Auto-indexing complete: %zu indexed, %zu failed\n",
                            vctx.indexed_count->load(),
                            vctx.failed_count->load());
            }

            co_await ctx.scope([files_ptr, &vctx](
                                   CoroScope& scope) -> coro::CoroTask<void> {
                for (std::size_t fi = 0; fi < files_ptr->size(); ++fi) {
                    const auto file_path = (*files_ptr)[fi];
                    scope.spawn([file_path, &vctx](
                                    CoroScope& fctx) -> coro::CoroTask<void> {
                        co_await process_single_file(file_path, vctx, fctx);
                    });
                }
                co_return;
            });

            co_return;
        },
        "DFTracerView");

    pipeline.set_source(combined_task);
    pipeline.set_destination(combined_task);
    try {
        pipeline.execute();
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Pipeline failed: %s", e.what());
        if (out_file) std::fclose(out_file);
        co_return 1;
    }

    if (!stream_mode) {
        FILE* target = out_file ? out_file : stdout;

        for (const auto& event : all_events) {
            std::fprintf(target, "%s\n", event.c_str());
        }
    }

    if (out_file) {
        std::fclose(out_file);
    }

    std::fprintf(stderr,
                 "View: %s | Files: %zu | Chunks: scanned=%llu skipped=%llu "
                 "| Events: matched=%llu scanned=%llu\n",
                 view.name.c_str(), files.size(),
                 (unsigned long long)total_chunks_scanned.load(),
                 (unsigned long long)total_chunks_skipped.load(),
                 (unsigned long long)total_events_matched.load(),
                 (unsigned long long)total_events_scanned.load());

    co_return 0;
}

int main(int argc, char** argv) {
    return cli::cli_main<ViewArgParse>(
        argc, argv, "dftracer_view",
        "Apply filtered views to DFTracer trace files. Uses bloom filter "
        "indices for efficient chunk-skipping. Supports predefined views "
        "(io, compute, dlio), custom recipes, and inline queries.",
        [](ViewArgParse& cli) -> int {
            try {
                return run_view(&cli).get();
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_ERROR("Fatal: %s", e.what());
                return 1;
            }
        });
}
