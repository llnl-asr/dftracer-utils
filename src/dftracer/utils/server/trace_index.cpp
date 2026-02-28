#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_builder.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#include <cinttypes>
#include <limits>

namespace dftracer::utils::server {

using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::filesystem;

TraceIndex::TraceIndex(const std::string& directory,
                       const std::string& index_dir)
    : directory_(directory), index_dir_(index_dir) {}

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

    for (const auto& entry : entries) {
        FileInfo info;
        info.path = entry.path.string();
        info.bidx_path = determine_bloom_index_path(info.path, index_dir_);
        info.idx_path = internal::determine_index_path(info.path, index_dir_);
        info.has_bloom_index = fs::exists(info.bidx_path);
        info.has_checkpoint_index = fs::exists(info.idx_path);

        // Build .idx + .bidx on the fly when missing.
        if (!info.has_bloom_index) {
            DFTRACER_UTILS_LOG_INFO("TraceIndex: building index for %s ...",
                                    info.path.c_str());

            BloomIndexBuildInput build_input;
            build_input.file_path = info.path;
            build_input.index_dir = index_dir_;
            build_input.force_rebuild = false;
            build_input.dimensions = default_bloom_dimensions();

            auto result =
                co_await BloomIndexBuilderUtility{}.process(build_input);

            if (result.success) {
                info.bidx_path = result.bidx_path;
                info.has_bloom_index = true;
                info.idx_path =
                    internal::determine_index_path(info.path, index_dir_);
                info.has_checkpoint_index = fs::exists(info.idx_path);
                DFTRACER_UTILS_LOG_INFO("TraceIndex: indexed %s",
                                        info.path.c_str());
            } else {
                DFTRACER_UTILS_LOG_WARN("TraceIndex: failed to index %s: %s",
                                        info.path.c_str(),
                                        result.error_message.c_str());
            }
        }

        // Query per-file time bounds from .bidx if available.
        if (info.has_bloom_index) {
            try {
                BloomIndexDatabase bidx(info.bidx_path);
                int fid = bidx.get_file_info_id(info.path);
                if (fid >= 0) {
                    auto tb = queries::query_time_bounds(bidx.db(), fid);
                    if (tb.valid) {
                        info.min_timestamp_us = tb.min_timestamp_us;
                        info.max_timestamp_us = tb.max_timestamp_us;
                        if (tb.min_timestamp_us < global_min_ts_) {
                            global_min_ts_ = tb.min_timestamp_us;
                        }
                        if (tb.max_timestamp_us > global_max_ts_) {
                            global_max_ts_ = tb.max_timestamp_us;
                        }
                    }
                }
            } catch (const std::exception& e) {
                DFTRACER_UTILS_LOG_WARN(
                    "TraceIndex: failed to read time bounds from %s: %s",
                    info.bidx_path.c_str(), e.what());
            }
        }

        path_to_index_[info.path] = files_.size();
        files_.push_back(std::move(info));
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

}  // namespace dftracer::utils::server
