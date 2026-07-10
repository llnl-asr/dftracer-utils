#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_builder_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <cinttypes>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::server {

using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::filesystem;
namespace indexer = dftracer::utils::utilities::indexer;

TraceIndex::TraceIndex(const std::string& directory,
                       const std::string& index_dir, std::size_t max_concurrent)
    : directory_(directory),
      index_dir_(index_dir),
      max_concurrent_(max_concurrent == 0 ? 8 : max_concurrent) {}

coro::CoroTask<void> TraceIndex::initialize() {
    PatternDirectoryScannerUtility scanner;
    PatternDirectoryScannerUtilityInput scan_input{
        directory_, {".pfw", ".pfw.gz"}, false};
    auto entries = co_await scanner.process(scan_input);

    files_.clear();
    path_to_index_.clear();
    files_.reserve(entries.size());

    global_min_ts_ = std::numeric_limits<std::uint64_t>::max();
    global_max_ts_ = 0;

    std::vector<std::size_t> needs_build;
    std::vector<std::size_t> large_files;

    for (const auto& entry : entries) {
        FileInfo info;
        info.path = entry.path.string();
        info.index_path = internal::determine_index_path(info.path, index_dir_);

        std::error_code ec;
        auto fsize = fs::file_size(info.path, ec);
        info.compressed_size = (!ec && fsize > 0) ? fsize : 0;

        std::size_t idx = files_.size();
        path_to_index_[info.path] = idx;

        info.has_bloom_data = fs::exists(info.index_path);
        info.has_checkpoint_index = fs::exists(info.index_path);
        if (!info.has_bloom_data) {
            needs_build.push_back(idx);
        } else {
            large_files.push_back(idx);
        }

        files_.push_back(std::move(info));
    }

    if (!needs_build.empty() || !large_files.empty()) {
        auto pipeline_config =
            PipelineConfig()
                .with_name("TraceIndex Init")
                .with_compute_threads(max_concurrent_)
                .with_watchdog(false)
                .with_global_timeout(std::chrono::seconds(0))
                .with_task_timeout(std::chrono::seconds(0))
                .with_io_backend(io::IoBackendType::THREADPOOL)
                .with_io_batch_size(1);

        Pipeline pipeline(pipeline_config);

        auto* files_ptr = &files_;
        auto* needs_build_ptr = &needs_build;
        auto* large_files_ptr = &large_files;
        auto* global_min_ts_ptr = &global_min_ts_;
        auto* global_max_ts_ptr = &global_max_ts_;
        std::string index_dir = index_dir_;
        std::size_t max_concurrent = max_concurrent_;

        auto init_task = make_task(
            [files_ptr, needs_build_ptr, large_files_ptr, global_min_ts_ptr,
             global_max_ts_ptr, index_dir,
             max_concurrent](CoroScope& ctx) -> coro::CoroTask<void> {
                if (!needs_build_ptr->empty()) {
                    DFTRACER_UTILS_LOG_INFO(
                        "TraceIndex: building index for %zu file(s) ...",
                        needs_build_ptr->size());

                    // Assign file identities up front, single-threaded, so the
                    // concurrent builders below don't race the shared next-id
                    // counter and collide two files onto one id. Hold each
                    // read-write handle open across the build so builders reuse
                    // it instead of hitting a read-only -> read-write upgrade.
                    std::vector<std::unique_ptr<indexer::IndexDatabase>>
                        held_dbs;
                    {
                        std::unordered_map<std::string,
                                           std::vector<std::size_t>>
                            by_index;
                        for (auto idx : *needs_build_ptr)
                            by_index[(*files_ptr)[idx].index_path].push_back(
                                idx);
                        for (auto& [ipath, idxs] : by_index) {
                            try {
                                auto db =
                                    std::make_unique<indexer::IndexDatabase>(
                                        ipath);
                                auto w = db->begin_write();
                                for (auto idx : idxs) {
                                    const auto& p = (*files_ptr)[idx].path;
                                    w->get_or_create_file_info(
                                        indexer::internal::get_logical_path(p),
                                        indexer::internal::calculate_file_hash(
                                            p));
                                }
                                w->commit();
                                held_dbs.push_back(std::move(db));
                            } catch (const std::exception& e) {
                                DFTRACER_UTILS_LOG_WARN(
                                    "TraceIndex: file-id pre-assign failed for "
                                    "%s: %s",
                                    ipath.c_str(), e.what());
                            }
                        }
                    }

                    auto file_chan =
                        coro::make_channel<std::size_t>(max_concurrent * 2);

                    const auto* index_dir_ptr = &index_dir;
                    co_await ctx.scope([&file_chan, files_ptr, needs_build_ptr,
                                        index_dir_ptr,
                                        max_concurrent](CoroScope& scope)
                                           -> coro::CoroTask<void> {
                        scope.spawn(
                            [ch = file_chan->producer(), needs_build_ptr](
                                CoroScope&) mutable -> coro::CoroTask<void> {
                                auto guard = ch.guard();
                                for (auto idx : *needs_build_ptr) {
                                    if (!co_await ch.send(idx)) co_return;
                                }
                                co_return;
                            });

                        for (std::size_t w = 0; w < max_concurrent; ++w) {
                            scope.spawn([ch = file_chan->consumer(), files_ptr,
                                         index_dir_ptr](CoroScope&)
                                            -> coro::CoroTask<void> {
                                while (auto fi_opt = co_await ch.receive()) {
                                    std::size_t fi = *fi_opt;
                                    auto* info = &(*files_ptr)[fi];

                                    indexer::IndexBuilderUtility builder;
                                    auto config =
                                        indexer::IndexBuildConfig::for_file(
                                            info->path)
                                            .with_index_dir(*index_dir_ptr);
                                    auto result =
                                        co_await builder.process(config);

                                    if (result.success) {
                                        info->index_path =
                                            internal::determine_index_path(
                                                info->path, *index_dir_ptr);
                                        info->has_bloom_data = true;
                                        info->has_checkpoint_index =
                                            fs::exists(info->index_path);
                                    } else {
                                        DFTRACER_UTILS_LOG_WARN(
                                            "TraceIndex: failed to "
                                            "index %s: %s",
                                            info->path.c_str(),
                                            result.error_message.c_str());
                                    }
                                }
                                co_return;
                            });
                        }
                        co_return;
                    });

                    for (auto idx : *needs_build_ptr) {
                        if ((*files_ptr)[idx].has_bloom_data) {
                            large_files_ptr->push_back(idx);
                        }
                    }
                }

                if (!large_files_ptr->empty()) {
                    auto meta_chan =
                        coro::make_channel<std::size_t>(max_concurrent * 2);

                    co_await ctx.scope([&meta_chan, files_ptr, large_files_ptr,
                                        max_concurrent](CoroScope& scope)
                                           -> coro::CoroTask<void> {
                        scope.spawn(
                            [ch = meta_chan->producer(), large_files_ptr](
                                CoroScope&) mutable -> coro::CoroTask<void> {
                                auto guard = ch.guard();
                                for (auto idx : *large_files_ptr) {
                                    if (!co_await ch.send(idx)) co_return;
                                }
                                co_return;
                            });

                        for (std::size_t w = 0; w < max_concurrent; ++w) {
                            scope.spawn([ch = meta_chan->consumer(),
                                         files_ptr](CoroScope&)
                                            -> coro::CoroTask<void> {
                                while (auto fi_opt = co_await ch.receive()) {
                                    std::size_t fi = *fi_opt;
                                    auto* info = &(*files_ptr)[fi];

                                    if (info->has_bloom_data) {
                                        try {
                                            indexer::IndexDatabase idx_db(
                                                info->index_path);
                                            auto logical = indexer::internal::
                                                get_logical_path(info->path);
                                            int fid = idx_db.get_file_info_id(
                                                logical);
                                            if (fid >= 0) {
                                                auto bounds =
                                                    idx_db.query_time_bounds(
                                                        fid);
                                                if (bounds.valid) {
                                                    info->min_timestamp_us =
                                                        bounds.min_timestamp_us;
                                                    info->max_timestamp_us =
                                                        bounds.max_timestamp_us;
                                                }
                                            }
                                        } catch (const std::exception& e) {
                                            DFTRACER_UTILS_LOG_WARN(
                                                "TraceIndex: failed to "
                                                "read time bounds from "
                                                "%s: %s",
                                                info->index_path.c_str(),
                                                e.what());
                                        }
                                    }

                                    auto meta_input =
                                        MetadataCollectorUtilityInput::
                                            from_file(info->path)
                                                .with_index(info->index_path);
                                    auto metadata =
                                        co_await MetadataCollectorUtility{}
                                            .process(meta_input);
                                    if (metadata.success) {
                                        info->uncompressed_size =
                                            metadata.uncompressed_size;
                                        info->num_checkpoints =
                                            metadata.num_checkpoints;
                                        info->checkpoint_size =
                                            metadata.checkpoint_size;
                                        info->compressed_size =
                                            metadata.compressed_size;
                                        info->num_lines = metadata.num_lines;
                                        info->size_mb = metadata.size_mb;
                                    }
                                }
                                co_return;
                            });
                        }
                        co_return;
                    });

                    for (auto fi : *large_files_ptr) {
                        const auto& info = (*files_ptr)[fi];
                        if (info.min_timestamp_us > 0 &&
                            info.min_timestamp_us < *global_min_ts_ptr)
                            *global_min_ts_ptr = info.min_timestamp_us;
                        if (info.max_timestamp_us > *global_max_ts_ptr)
                            *global_max_ts_ptr = info.max_timestamp_us;
                    }
                }

                co_return;
            },
            "TraceIndexInit");

        pipeline.set_source(init_task);
        pipeline.set_destination(init_task);
        pipeline.execute();
    }

    DFTRACER_UTILS_LOG_INFO("TraceIndex: found %zu trace files in %s",
                            files_.size(), directory_.c_str());
    if (global_max_ts_ > 0) {
        DFTRACER_UTILS_LOG_INFO("TraceIndex: global time range [%" PRIu64
                                ", %" PRIu64 "] us",
                                global_min_ts_, global_max_ts_);
    }
}

const TraceIndex::FileInfo* TraceIndex::find_file(
    const std::string& path) const {
    auto it = path_to_index_.find(path);
    if (it == path_to_index_.end()) return nullptr;
    return &files_[it->second];
}

const TraceIndex::FileInfo* TraceIndex::file_at(std::size_t index) const {
    if (index >= files_.size()) return nullptr;
    return &files_[index];
}

std::vector<const TraceIndex::FileInfo*> collect_candidate_files(
    TraceIndex& index, const QueryParams& params) {
    std::vector<const TraceIndex::FileInfo*> files;
    auto file_param = params.get("file");
    if (!file_param.empty()) {
        auto* f = index.find_file(std::string(file_param));
        if (f) files.push_back(f);
    } else {
        for (const auto& f : index.files()) {
            files.push_back(&f);
        }
    }
    return files;
}

}  // namespace dftracer::utils::server
