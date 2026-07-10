#ifndef DFTRACER_UTILS_SERVER_TRACE_INDEX_H
#define DFTRACER_UTILS_SERVER_TRACE_INDEX_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/server/viz_summary.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter_cache.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::server {

/// Scans a directory for trace files and caches paths to their
/// root-local `.dftindex` database. Used by API handlers to resolve file
/// paths and check index availability.
class TraceIndex {
   public:
    struct FileInfo {
        std::string path;
        std::string index_path;
        bool has_bloom_data = false;
        bool has_checkpoint_index = false;
        std::uint64_t min_timestamp_us = 0;
        std::uint64_t max_timestamp_us = 0;
        std::uint64_t compressed_size = 0;
        std::uint64_t uncompressed_size = 0;
        std::size_t num_checkpoints = 0;
        std::uint64_t checkpoint_size = 0;
        std::uint64_t num_lines = 0;
        double size_mb = 0;
    };

    TraceIndex(const std::string& directory, const std::string& index_dir,
               std::size_t max_concurrent = 8);

    /// Scan directory and populate the file list.
    coro::CoroTask<void> initialize();

    std::size_t file_count() const { return files_.size(); }
    const std::vector<FileInfo>& files() const { return files_; }

    /// Find a file by its path. Returns nullptr if not found.
    const FileInfo* find_file(const std::string& path) const;

    /// Find a file by index. Returns nullptr if out of range.
    const FileInfo* file_at(std::size_t index) const;

    const std::string& directory() const { return directory_; }
    const std::string& index_dir() const { return index_dir_; }
    std::size_t max_concurrent() const { return max_concurrent_; }

    using BloomCache =
        dftracer::utils::utilities::composites::dft::indexing::BloomFilterCache;
    BloomCache& bloom_cache() { return bloom_cache_; }

    std::uint64_t global_min_timestamp_us() const { return global_min_ts_; }
    std::uint64_t global_max_timestamp_us() const { return global_max_ts_; }

    // Lazily-built activity summary. Returns nullptr until the build finishes;
    // callers fall back to a live scan meanwhile.
    const VizSummary* viz_summary() const {
        return viz_summary_state_.load(std::memory_order_acquire) == 2
                   ? viz_summary_.get()
                   : nullptr;
    }
    // Claim the right to build the summary; only the first caller gets true.
    bool try_begin_summary_build() {
        int expected = 0;
        return viz_summary_state_.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel);
    }
    void set_viz_summary(std::unique_ptr<VizSummary> summary) {
        viz_summary_ = std::move(summary);
        viz_summary_state_.store(2, std::memory_order_release);
    }

   private:
    std::string directory_;
    std::string index_dir_;
    std::vector<FileInfo> files_;
    std::unordered_map<std::string, std::size_t> path_to_index_;
    std::uint64_t global_min_ts_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t global_max_ts_ = 0;
    std::size_t max_concurrent_;
    BloomCache bloom_cache_;

    std::unique_ptr<VizSummary> viz_summary_;
    std::atomic<int> viz_summary_state_{0};  // 0 not built, 1 building, 2 ready
};

class QueryParams;

/// Collect the candidate files for a streaming query: the explicit `?file=`
/// (when present and found in the index) or all indexed files. Callers apply
/// their own timestamp-overlap filter on top of this.
std::vector<const TraceIndex::FileInfo*> collect_candidate_files(
    TraceIndex& index, const QueryParams& params);

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_TRACE_INDEX_H
