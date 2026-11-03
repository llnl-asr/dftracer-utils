// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/archive_format.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/schemas/dft/merged_statistics.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/pipeline/pipeline.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#include <memory>
#include <mutex>

using namespace dftracer::utils;
using namespace dftracer::utils::trace;
using namespace dftracer::utils::index::build;
using dftracer::utils::index::build::BatchBuilder;
using dftracer::utils::index::build::IndexBuildBatchConfig;
using dftracer::utils::index::gzip::CheckpointIndexerFactory;
using dftracer::utils::index::store::IndexExtension;

class InfoArgParse : public binaries::ArgParse {
   public:
    binaries::InputArgs inputs{
        binaries::DirMode::DefaultEmpty,
        "Compressed files to inspect (GZIP, TAR.GZ) or a folder of them"};
    binaries::PipelineArgs pipeline;
    binaries::IndexingArgs indexing;

    dstd::StringView report = "summary";

    explicit InfoArgParse(binaries::ArgParser& p) : ArgParse(p) {
        indexing.index_dir_help =
            "Directory to store index files (default: system temp directory)";
        schema(inputs, pipeline, indexing);
    }

   protected:
    void register_args() override {
        parser().option(report, {"--report"},
                        "Report type: summary (aggregate all files, default) "
                        "or detailed (per-file output)");
    }
};

static dstd::String format_size(std::uint64_t bytes) {
    return common::human_bytes(static_cast<double>(bytes), "", 2);
}

struct RootInfoSummary {
    std::size_t file_count = 0;
    std::uint64_t total_events = 0;
    std::uint64_t total_lines = 0;
    std::uint64_t total_uncompressed = 0;
};

static coro::CoroTask<dstd::SharedPtr<RootInfoSummary>> load_root_info_summary(
    // keep std: unswept API
    std::string index_path) {
    // keep std: unswept API
    auto result = std::make_shared<RootInfoSummary>();

    const auto snapshot =
        common::unwrap(index::plan::open_snapshot(index_path));
    const auto listed = common::unwrap(snapshot.files());
    result->file_count = listed.size();
    for (const auto& file : listed) {
        if (auto summary = common::unwrap(index::scalar_stats(snapshot, file)))
            result->total_events += summary->stats.total_events;
        if (auto meta = common::unwrap(index::file_metadata(snapshot, file))) {
            result->total_lines += meta->num_lines;
            result->total_uncompressed += meta->max_bytes;
        }
    }
    co_return result;
}

// Files of `index_path` in path order, with the ones whose pruning tier is
// current added to `group`.
// keep std: unswept API
static void load_cached_files(const std::string& index_path,
                              // keep std: unswept API
                              std::vector<std::string>& files,
                              // keep std: unswept API
                              std::vector<ResolvedFile>& group) {
    const auto snapshot =
        common::unwrap(index::plan::open_snapshot(index_path));
    const auto listed = common::unwrap(snapshot.files());
    files.reserve(listed.size());
    group.reserve(listed.size());
    std::size_t fi = 0;
    for (const auto& file : listed) {
        files.emplace_back(file.path());
        if (index::extension_current(snapshot, file, IndexExtension::Stats))
            group.push_back(ResolvedFile{fi, files.back(), {}});
        ++fi;
    }
}

// keep std: unswept API
static std::vector<MetadataCollectorUtilityOutput>
// keep std: unswept API
process_index_group_info_sync(std::string index_path,
                              // keep std: unswept API
                              std::vector<ResolvedFile> entries) {
    const auto snapshot =
        common::unwrap(index::plan::open_snapshot(index_path));

    // keep std: unswept API
    std::vector<MetadataCollectorUtilityOutput> results;
    results.reserve(entries.size());

    for (const auto& entry : entries) {
        MetadataCollectorUtilityOutput meta;
        meta.file_path = entry.file_path;
        meta.index_path = index_path;
        meta.has_index = true;
        meta.index_valid = true;

        const auto file = snapshot.file(
            index::store::internal::get_logical_path(entry.file_path));
        const auto metadata =
            file ? common::unwrap(index::file_metadata(snapshot, *file))
                 : dstd::nullopt;
        if (!metadata) {
            meta.success = false;
            meta.error_message = "No file metadata found in shared index";
            results.push_back(std::move(meta));
            continue;
        }

        meta.format = CheckpointIndexerFactory::detect_format(entry.file_path);
        meta.compressed_size = 0;
        meta.checkpoint_size = metadata->checkpoint_size;
        meta.num_lines = metadata->num_lines;
        meta.uncompressed_size = metadata->max_bytes;
        meta.size_mb =
            static_cast<double>(meta.uncompressed_size) / (1024.0 * 1024.0);
        meta.start_line = 1;
        meta.end_line = meta.num_lines;

        if (meta.checkpoint_size > 0 && meta.uncompressed_size > 0) {
            meta.num_checkpoints =
                (meta.uncompressed_size + meta.checkpoint_size - 1) /
                meta.checkpoint_size;
        }

        const auto merged = common::unwrap(
            index::schemas::dft::merged_statistics(snapshot, *file));
        if (merged) {
            meta.valid_events = merged->stats.total_events;
            if (merged->num_chunks > 0) {
                meta.num_checkpoints = merged->num_chunks;
            }
        } else {
            meta.valid_events = meta.num_lines;
        }

        meta.size_per_line =
            (meta.valid_events > 0)
                ? meta.size_mb / static_cast<double>(meta.valid_events)
                : 0.0;
        meta.success = true;
        results.push_back(std::move(meta));
    }

    return results;
}

// keep std: unswept API
static coro::CoroTask<std::vector<MetadataCollectorUtilityOutput>>
// keep std: unswept API
process_index_group_info(dstd::SharedPtr<std::string> index_path,
                         // keep std: unswept API
                         dstd::SharedPtr<std::vector<ResolvedFile>> entries) {
    co_return process_index_group_info_sync(std::move(*index_path),
                                            std::move(*entries));
}

// The file's manifest: each extension built for it, and whether it is usable.
static void print_extensions(const MetadataCollectorUtilityOutput& info,
                             const index::Snapshot& snapshot) {
    using dftracer::utils::index::store::ALL_EXTENSIONS;
    using dftracer::utils::index::store::extension_name;
    try {
        const auto file = snapshot.file(
            dftracer::utils::index::store::internal::get_logical_path(
                info.file_path));
        if (!file) return;
        if (auto schema = common::unwrap(index::schema(snapshot, *file)))
            std::printf("  Schema: %s\n", schema->c_str());
        std::printf("  Extensions:");
        for (auto ext : ALL_EXTENSIONS) {
            const auto name = extension_name(ext);
            if (index::extension_current(snapshot, *file, ext)) {
                std::printf(" %.*s", static_cast<int>(name.size()),
                            name.data());
                continue;
            }
            // An extension built at another format version reads as absent.
            const auto state = snapshot.extension(*file, ext, 0);
            if (state == index::Freshness::Missing) continue;
            const char* note =
                state == index::Freshness::Failed ? " (failed)" : " (outdated)";
            std::printf(" %.*s%s", static_cast<int>(name.size()), name.data(),
                        note);
        }
        // No plugin is loaded here, so a plugin extension's currency is
        // unknown.
        for (const auto& plugin :
             common::unwrap(index::plugins(snapshot, *file)))
            std::printf(" %s (plugin%s)", plugin.name.c_str(),
                        plugin.ready ? "" : ", failed");
        std::printf("\n");
    } catch (const std::exception& e) {
        std::printf("  Extensions: unreadable (%s)\n", e.what());
    }
}

static void print_file_info(const MetadataCollectorUtilityOutput& info,
                            const index::Snapshot& snapshot, bool detailed) {
    std::printf("========================================\n");
    std::printf("File: %s\n", info.file_path.c_str());
    std::printf("========================================\n");

    if (!info.success) {
        std::printf("  Status: ERROR - %s\n", info.error_message.c_str());
        std::printf("\n");
        return;
    }

    std::printf("Basic Information:\n");
    std::printf("  Format: %s\n", common::get_format_name(info.format));
    std::printf("  Status: %s\n", "OK");

    std::printf("\nFile Size:\n");
    if (info.compressed_size > 0) {
        std::printf("  Compressed:   %12s (%llu bytes)\n",
                    format_size(info.compressed_size).c_str(),
                    (unsigned long long)info.compressed_size);
    }
    std::printf("  Uncompressed: %12s (%llu bytes)\n",
                format_size(info.uncompressed_size).c_str(),
                (unsigned long long)info.uncompressed_size);

    if (info.compressed_size > 0 && info.uncompressed_size > 0 &&
        info.compressed_size != info.uncompressed_size) {
        double ratio =
            100.0 * (1.0 - static_cast<double>(info.compressed_size) /
                               static_cast<double>(info.uncompressed_size));
        std::printf("  Savings:      %.2f%% reduction\n", ratio);
    }

    std::printf("\nContent:\n");
    std::printf("  Total Lines: %llu\n", (unsigned long long)info.num_lines);
    std::printf("  Valid Events: %zu\n", info.valid_events);

    if (info.has_index && info.index_valid) {
        std::printf("\nIndex Information:\n");
        std::printf("  Index Store: %s\n", info.index_path.c_str());
        std::printf("  Checkpoint Size: %s\n",
                    format_size(info.checkpoint_size).c_str());
        std::printf("  Checkpoints: %zu\n", info.num_checkpoints);
        print_extensions(info, snapshot);
    }

    if (detailed) {
        std::printf("\nDetailed Statistics:\n");
        std::printf("  Start Line: %zu\n", info.start_line);
        std::printf("  End Line: %zu\n", info.end_line);
        std::printf("  Size (MB): %.6f\n", info.size_mb);
        std::printf("  MB per Event: %.8f\n", info.size_per_line);

        if (info.num_checkpoints > 0 && info.num_lines > 0) {
            auto lines_per_ckpt = info.num_lines / info.num_checkpoints;
            std::printf("\nRandom Access Performance:\n");
            std::printf("  Worst-case lines to scan: %llu (1 checkpoint)\n",
                        (unsigned long long)lines_per_ckpt);
            std::printf("  Avg lines to scan: %llu (0.5 checkpoint)\n",
                        (unsigned long long)(lines_per_ckpt / 2));
        }
    }

    std::printf("\n");
}

static coro::CoroTask<void> auto_index_and_resolve(
    // keep std: unswept API
    std::vector<FileWorkItem>& files_needing_index,
    // keep std: unswept API
    const std::string& index_dir, std::size_t checkpoint_size,
    std::size_t executor_threads,
    // keep std: unswept API
    std::unordered_map<std::string, std::vector<ResolvedFile>>&
        indexed_groups) {
    auto index_path = internal::determine_index_path(
        files_needing_index.front().file_path, index_dir);
    dftracer::utils::index::release_root(index_path);

    {
        // keep std: unswept API
        auto batch_config = std::make_shared<IndexBuildBatchConfig>();
        batch_config->file_paths.reserve(files_needing_index.size());
        for (const auto& item : files_needing_index) {
            batch_config->file_paths.push_back(item.file_path);
        }
        batch_config->index_dir = index_dir;
        batch_config->checkpoint_size = checkpoint_size;
        batch_config->parallelism = executor_threads;

        auto batch_result =
            co_await BatchBuilder::process(std::move(batch_config));

        for (const auto& result : batch_result.results) {
            if (!result.success && !result.error_message.empty()) {
                DFTRACER_UTILS_LOG_ERROR("Auto-indexing failed for %s: %s",
                                         result.file_path.c_str(),
                                         result.error_message.c_str());
            }
        }
    }

    // keep std: unswept API
    std::vector<std::string> newly_indexed;
    newly_indexed.reserve(files_needing_index.size());
    for (const auto& item : files_needing_index) {
        newly_indexed.push_back(item.file_path);
    }

    Resolver resolver;
    ResolverInput refresh_input;
    refresh_input.files = std::move(newly_indexed);
    refresh_input.index_dir = index_dir;
    refresh_input.require_checkpoints = true;

    auto refresh_result = co_await resolver(refresh_input);

    if (!refresh_result.cached.empty()) {
        indexed_groups[refresh_result.index_path] =
            std::move(refresh_result.cached);
    }
}

static coro::CoroTask<int> run_info(const InfoArgParse* cli) {
    // keep std: unswept API
    const std::string directory(cli->inputs.directory);
    const auto report = cli->report;
    const auto force_rebuild = cli->indexing.force;
    const auto checkpoint_size = cli->indexing.checkpoint_size;
    // keep std: unswept API
    const std::string index_dir(cli->indexing.index_dir);
    const auto executor_threads = cli->pipeline.executor_threads;
    const bool summary_mode = (report != "detailed");

    common::Timer stages_storage("dftracer_info");
    common::Timer* stages =
        cli->pipeline.time_profiling ? &stages_storage : nullptr;
    common::Timer overall(true);

    // keep std: unswept API
    std::vector<std::string> files;
    // keep std: unswept API
    std::vector<std::string> cli_files;
    for (const auto file : cli->inputs.files) cli_files.emplace_back(file);
    // keep std: unswept API
    std::vector<FileWorkItem> files_needing_index;
    // keep std: unswept API
    std::unordered_map<std::string, std::vector<ResolvedFile>> indexed_groups;

    {
        common::ScopedTimer _t(stages, "collect_and_classify");

        if (!directory.empty()) {
            if (!common::input_exists(directory)) {
                DFTRACER_UTILS_LOG_ERROR("Directory does not exist: %s",
                                         directory.c_str());
                co_return 1;
            }

            co_await ensure_index_fresh(directory, "", index_dir,
                                        force_rebuild);

            auto trusted_index_path =
                internal::determine_index_path(directory, index_dir);
            if (!force_rebuild &&
                common::unwrap(dstd::fs::exists(dstd::PathView(
                    dftracer::utils::index::store_dir(trusted_index_path))))) {
                if (summary_mode) {
                    common::ScopedTimer _rt(stages, "root_summary_read");
                    auto root_result =
                        co_await load_root_info_summary(trusted_index_path);

                    if (stages) stages->print_stages();

                    std::printf("==========================================\n");
                    std::printf("DFTracer File Info Summary\n");
                    std::printf("==========================================\n");
                    std::printf("  Total Files:        %zu\n",
                                root_result->file_count);
                    std::printf("  Successful:         %zu\n",
                                root_result->file_count);
                    std::printf("  Failed:             0\n");
                    std::printf("  Total Lines:        %llu\n",
                                (unsigned long long)root_result->total_lines);
                    std::printf("  Valid Events:       %llu\n",
                                (unsigned long long)root_result->total_events);
                    std::printf(
                        "  Total Uncompressed: %s (%llu bytes)\n",
                        format_size(root_result->total_uncompressed).c_str(),
                        (unsigned long long)root_result->total_uncompressed);
                    if (root_result->total_events > 0) {
                        std::printf(
                            "  Avg Bytes/Event:    %.2f bytes\n",
                            static_cast<double>(
                                root_result->total_uncompressed) /
                                static_cast<double>(root_result->total_events));
                    }
                    std::printf("  Processing Time:    %.2f ms\n",
                                static_cast<double>(overall.elapsed()) / 1e6);
                    std::printf("==========================================\n");
                    co_return 0;
                }

                {
                    common::ScopedTimer _lr(stages, "load_registry");
                    load_cached_files(trusted_index_path, files,
                                      indexed_groups[trusted_index_path]);
                }
            } else {
                common::ScopedTimer _ds(stages, "scan_and_resolve");
                Resolver resolver;
                // keep std: unswept API
                auto input = std::make_unique<ResolverInput>();
                input->directory = directory;
                input->index_dir = index_dir;
                auto result = co_await resolver(*input);
                files = std::move(result.all_files);
                if (!result.cached.empty()) {
                    indexed_groups[result.index_path] =
                        std::move(result.cached);
                }
                files_needing_index = std::move(result.needs_checkpoint);
            }
        } else {
            common::ScopedTimer _rs(stages, "resolve_index_state");
            co_await ensure_indexes_fresh("", cli_files, index_dir,
                                          force_rebuild);
            Resolver resolver;
            // keep std: unswept API
            auto input = std::make_unique<ResolverInput>();
            input->files = cli_files;
            input->index_dir = index_dir;
            auto result = co_await resolver(*input);
            files = std::move(result.all_files);
            if (!result.cached.empty()) {
                indexed_groups[result.index_path] = std::move(result.cached);
            }
            files_needing_index = std::move(result.needs_checkpoint);
        }
    }

    if (files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s", "No files found. Use --help for usage.");
        co_return 1;
    }

    // keep std: unswept API
    std::vector<MetadataCollectorUtilityOutput> all_results;
    std::mutex results_mutex;

    if (!indexed_groups.empty()) {
        common::ScopedTimer _t(stages, "index_batch_read");
        co_await coro::with_scope(
            [&](coro::Scope& scope) -> coro::CoroTask<void> {
                auto* all_results_ptr = &all_results;
                auto* mutex_ptr = &results_mutex;
                for (auto& [ip, group] : indexed_groups) {
                    // keep std: unswept API
                    auto idx_path_ptr = std::make_shared<std::string>(ip);
                    auto entries_ptr =
                        // keep std: unswept API
                        std::make_shared<std::vector<ResolvedFile>>(
                            std::move(group));
                    scope.spawn([idx_path_ptr, entries_ptr, all_results_ptr,
                                 mutex_ptr]() mutable -> coro::CoroTask<void> {
                        auto infos = co_await process_index_group_info(
                            std::move(idx_path_ptr), std::move(entries_ptr));
                        std::lock_guard<std::mutex> lock(*mutex_ptr);
                        for (auto& info : infos) {
                            all_results_ptr->push_back(std::move(info));
                        }
                    });
                }
                co_return;
            });
    }

    if (!files_needing_index.empty()) {
        common::ScopedTimer _t(stages, "auto_index_and_build");
        co_await auto_index_and_resolve(files_needing_index, index_dir,
                                        checkpoint_size, executor_threads,
                                        indexed_groups);

        if (!indexed_groups.empty()) {
            common::ScopedTimer _t2(stages, "newly_indexed_batch_read");
            co_await coro::with_scope([&](coro::Scope& scope)
                                          -> coro::CoroTask<void> {
                auto* all_results_ptr = &all_results;
                auto* mutex_ptr = &results_mutex;
                for (auto& [ip, group] : indexed_groups) {
                    // keep std: unswept API
                    auto idx_path_ptr = std::make_shared<std::string>(ip);
                    auto entries_ptr =
                        // keep std: unswept API
                        std::make_shared<std::vector<ResolvedFile>>(
                            std::move(group));
                    scope.spawn([idx_path_ptr, entries_ptr, all_results_ptr,
                                 mutex_ptr]() mutable -> coro::CoroTask<void> {
                        auto infos = co_await process_index_group_info(
                            std::move(idx_path_ptr), std::move(entries_ptr));
                        std::lock_guard<std::mutex> lock(*mutex_ptr);
                        for (auto& info : infos) {
                            all_results_ptr->push_back(std::move(info));
                        }
                    });
                }
                co_return;
            });
        }
    }

    if (stages) stages->print_stages();
    if (summary_mode) {
        std::uint64_t total_uncompressed = 0;
        std::uint64_t total_lines = 0;
        std::uint64_t total_events = 0;
        std::size_t ok = 0;
        std::size_t bad = 0;

        for (const auto& r : all_results) {
            if (r.success) {
                total_uncompressed += r.uncompressed_size;
                total_lines += r.num_lines;
                total_events += r.valid_events;
                ok++;
            } else {
                bad++;
            }
        }

        std::printf("==========================================\n");
        std::printf("DFTracer File Info Summary\n");
        std::printf("==========================================\n");
        std::printf("  Total Files:        %zu\n", files.size());
        std::printf("  Successful:         %zu\n", ok);
        std::printf("  Failed:             %zu\n", bad);
        std::printf("  Total Lines:        %llu\n",
                    (unsigned long long)total_lines);
        std::printf("  Valid Events:       %llu\n",
                    (unsigned long long)total_events);
        std::printf("  Total Uncompressed: %s (%llu bytes)\n",
                    format_size(total_uncompressed).c_str(),
                    (unsigned long long)total_uncompressed);

        if (total_events > 0) {
            std::printf("  Avg Bytes/Event:    %.2f bytes\n",
                        static_cast<double>(total_uncompressed) /
                            static_cast<double>(total_events));
        }

        std::printf("  Processing Time:    %.2f ms\n",
                    static_cast<double>(overall.elapsed()) / 1e6);
        std::printf("==========================================\n");

        co_return (bad == 0) ? 0 : 1;
    }

    // keep std: unswept API
    std::unordered_map<std::string, index::Snapshot> snapshots;
    for (const auto& r : all_results) {
        auto it = snapshots.find(r.index_path);
        if (it == snapshots.end())
            it = snapshots
                     .emplace(r.index_path,
                              common::unwrap(
                                  index::plan::open_snapshot(r.index_path)))
                     .first;
        print_file_info(r, it->second, !summary_mode);
    }

    if (files.size() > 1) {
        std::uint64_t total_uncompressed = 0;
        std::uint64_t total_lines = 0;
        std::size_t ok = 0;

        for (const auto& r : all_results) {
            if (r.success) {
                ok++;
                total_uncompressed += r.uncompressed_size;
                total_lines += r.num_lines;
            }
        }

        std::printf("==========================================\n");
        std::printf("Summary\n");
        std::printf("==========================================\n");
        std::printf("Total Files: %zu\n", files.size());
        std::printf("Successful: %zu\n", ok);
        std::printf("Failed: %zu\n", files.size() - ok);
        std::printf("Total Lines: %llu\n", (unsigned long long)total_lines);
        std::printf("Total Uncompressed: %s\n",
                    format_size(total_uncompressed).c_str());
        std::printf("Processing Time: %.2f ms\n",
                    static_cast<double>(overall.elapsed()) / 1e6);
    }

    co_return 0;
}

int main(int argc, char** argv) {
    return binaries::cli_main<InfoArgParse>(
        argc, argv, "dftracer_info",
        "Display metadata and index information for DFTracer compressed files "
        "using composable utilities and pipeline processing",
        [](InfoArgParse& cli) {
            return binaries::run_single_task(
                "DFTracer Info", cli.pipeline,
                [&cli](coro::Scope&) -> coro::CoroTask<int> {
                    co_return co_await run_info(&cli);
                });
        });
}
