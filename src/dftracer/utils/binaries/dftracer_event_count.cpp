#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/resolve_and_build.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using dftracer::utils::utilities::fileio::lines::sources::
    async_streaming_gz_lines;
using dftracer::utils::utilities::indexer::IndexBatchBuilderUtility;
using dftracer::utils::utilities::indexer::IndexBuildBatchConfig;

class EventCountArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory;
    cli::PipelineArgs pipeline;
    cli::IndexingArgs indexing;

    explicit EventCountArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.index_dir_help =
            "Directory to store index files (default: system temp directory)";
        schema(directory, pipeline, indexing);
    }
};

static int run_event_count(const EventCountArgParse* cli);

struct EventCountBatchResult {
    std::size_t total_events = 0;
    std::size_t files_processed = 0;
    bool is_approximate = false;
};

static EventCountBatchResult process_index_group_event_counts_sync(
    std::string index_path, std::vector<ResolvedFile> entries) {
    std::vector<int> file_ids;
    file_ids.reserve(entries.size());
    for (const auto& entry : entries) {
        file_ids.push_back(entry.file_id);
    }

    EventCountBatchResult batch_result;

    utilities::indexer::IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);

    auto metadata_rows = db.query_file_metadata_batch(file_ids);
    auto merged_stats = db.query_merged_statistics_batch(file_ids);

    for (const auto file_id : file_ids) {
        auto merged_it = merged_stats.find(file_id);
        if (merged_it != merged_stats.end() &&
            merged_it->second.num_chunks > 0) {
            batch_result.total_events +=
                static_cast<std::size_t>(merged_it->second.stats.total_events);
            batch_result.files_processed++;
            continue;
        }

        auto metadata_it = metadata_rows.find(file_id);
        if (metadata_it != metadata_rows.end()) {
            batch_result.total_events +=
                static_cast<std::size_t>(metadata_it->second.num_lines);
            batch_result.files_processed++;
            batch_result.is_approximate = true;
        }
    }

    return batch_result;
}

static coro::CoroTask<EventCountBatchResult> process_index_group_event_counts(
    std::shared_ptr<std::string> index_path,
    std::shared_ptr<std::vector<ResolvedFile>> entries) {
    co_return process_index_group_event_counts_sync(std::move(*index_path),
                                                    std::move(*entries));
}

int main(int argc, char** argv) {
    return cli::cli_main<EventCountArgParse>(
        argc, argv, "dftracer_event_count",
        "Count valid events in DFTracer .pfw or .pfw.gz files using composable "
        "utilities and pipeline processing",
        [](EventCountArgParse& cli) { return run_event_count(&cli); });
}

static int run_event_count(const EventCountArgParse* cli) {
    const auto log_dir = fs::absolute(cli->directory.value).string();
    const auto index_dir = cli->indexing.index_dir;
    const auto checkpoint_size = cli->indexing.checkpoint_size;
    const auto force_rebuild = cli->indexing.force;
    const auto executor_threads = cli->pipeline.executor_threads;

    IndexResolverUtility resolver;
    ResolverInput resolve_input;
    resolve_input.directory = log_dir;
    resolve_input.index_dir = index_dir;
    resolve_input.require_checkpoints = !force_rebuild;

    auto resolve_result = resolver.process(resolve_input).get();

    if (resolve_result.all_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No .pfw or .pfw.gz files found in: %s",
                                 log_dir.c_str());
        return 1;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    std::atomic<std::size_t> total_events{0};
    std::atomic<std::size_t> files_processed{0};
    std::atomic<bool> is_approximate{false};

    std::vector<FileWorkItem> direct_scan_items;
    std::vector<ResolvedFile> indexed_entries =
        std::move(resolve_result.cached);
    std::string index_path = resolve_result.index_path;

    std::vector<FileWorkItem> needs_checkpoint =
        std::move(resolve_result.needs_checkpoint);

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer Event Count", cli->pipeline);
    Pipeline pipeline(pipeline_config);

    auto build_task = make_task(
        [&needs_checkpoint, index_dir, checkpoint_size, executor_threads,
         force_rebuild](CoroScope& scope) -> coro::CoroTask<void> {
            if (needs_checkpoint.empty()) {
                co_return;
            }
            auto batch_config = std::make_shared<IndexBuildBatchConfig>();
            batch_config->file_paths.reserve(needs_checkpoint.size());
            for (const auto& item : needs_checkpoint) {
                batch_config->file_paths.push_back(item.file_path);
            }
            batch_config->index_dir = index_dir;
            batch_config->checkpoint_size = checkpoint_size;
            batch_config->parallelism = executor_threads;
            batch_config->force_rebuild = force_rebuild;
            batch_config->use_batch_write = true;
            batch_config->rebuild_root_summaries = true;
            co_await IndexBatchBuilderUtility::process(&scope,
                                                       std::move(batch_config));
        },
        "BatchIndex");

    auto count_task = make_task(
        [&needs_checkpoint, &indexed_entries, &direct_scan_items, &total_events,
         &files_processed, &is_approximate, index_dir, index_path,
         executor_threads](CoroScope& ctx) -> coro::CoroTask<void> {
            if (!needs_checkpoint.empty()) {
                IndexResolverUtility re_resolver;
                ResolverInput refresh_input;
                std::vector<std::string> newly_indexed;
                newly_indexed.reserve(needs_checkpoint.size());
                for (const auto& item : needs_checkpoint) {
                    newly_indexed.push_back(item.file_path);
                }
                refresh_input.files = std::move(newly_indexed);
                refresh_input.index_dir = index_dir;
                refresh_input.require_checkpoints = true;

                auto refresh_result =
                    co_await re_resolver.process(refresh_input);
                for (auto& entry : refresh_result.cached) {
                    indexed_entries.push_back(std::move(entry));
                }
                for (auto& item : refresh_result.needs_checkpoint) {
                    direct_scan_items.push_back(std::move(item));
                }
            }

            if (!indexed_entries.empty()) {
                auto idx_path_ptr = std::make_shared<std::string>(index_path);
                auto entries_ptr = std::make_shared<std::vector<ResolvedFile>>(
                    std::move(indexed_entries));
                try {
                    auto batch_result =
                        co_await process_index_group_event_counts(
                            std::move(idx_path_ptr), std::move(entries_ptr));
                    total_events.fetch_add(batch_result.total_events,
                                           std::memory_order_relaxed);
                    files_processed.fetch_add(batch_result.files_processed,
                                              std::memory_order_relaxed);
                    if (batch_result.is_approximate) {
                        is_approximate.store(true, std::memory_order_relaxed);
                    }
                } catch (...) {
                    is_approximate.store(true, std::memory_order_relaxed);
                }
            }

            if (!direct_scan_items.empty()) {
                is_approximate.store(true, std::memory_order_relaxed);
                co_await ctx.scope([&](CoroScope& scope)
                                       -> coro::CoroTask<void> {
                    cli::parallel_for_each(
                        scope, std::move(direct_scan_items), executor_threads,
                        [total_events_ptr = &total_events,
                         files_processed_ptr = &files_processed](
                            const FileWorkItem& item) -> coro::CoroTask<void> {
                            std::size_t count = 0;
                            auto gen = async_streaming_gz_lines(item.file_path);
                            while (co_await gen.next()) {
                                ++count;
                            }
                            total_events_ptr->fetch_add(
                                count, std::memory_order_relaxed);
                            files_processed_ptr->fetch_add(
                                1, std::memory_order_relaxed);
                        });
                    co_return;
                });
            }
            co_return;
        },
        "Count");

    count_task->depends_on(build_task);
    pipeline.set_source(build_task);
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

    return 0;
}
