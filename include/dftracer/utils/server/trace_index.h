#ifndef DFTRACER_UTILS_SERVER_TRACE_INDEX_H
#define DFTRACER_UTILS_SERVER_TRACE_INDEX_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/async_mutex.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/server/viz_result_cache.h>
#include <dftracer/utils/server/viz_summary.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter_cache.h>
#include <dftracer/utils/utilities/composites/dft/time_metric.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::server {

// Byte budget for the per-index viz result cache.
inline constexpr std::size_t VIZ_RESULT_CACHE_BYTES = 256UL * 1024 * 1024;

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
               std::size_t max_concurrent = 8,
               std::size_t checkpoint_size =
                   constants::indexer::DEFAULT_CHECKPOINT_SIZE);

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

    // Result cache for heavy viz endpoints. Immutable trace => never stale.
    VizResultCache& viz_cache() { return viz_cache_; }

    // Trace-wide native time unit (one unit per trace), from the leading CM
    // time_metric of the first file (absent = US). Index/event ts and dur are
    // stored in this unit; the viz layer converts at its us boundary.
    using TimeMetric = dftracer::utils::utilities::composites::dft::TimeMetric;
    TimeMetric time_metric() const { return time_metric_; }
    std::uint64_t native_to_us(std::uint64_t v) const {
        return dftracer::utils::utilities::composites::dft::scale_between(
            time_metric_, TimeMetric::US, v);
    }
    std::uint64_t us_to_native(std::uint64_t v) const {
        return dftracer::utils::utilities::composites::dft::scale_between(
            TimeMetric::US, time_metric_, v);
    }

    // Native (index-unit) global bounds. The `_us` names are historical: for a
    // US trace they are microseconds; for NS/MS/SEC traces use native_to_us().
    std::uint64_t global_min_timestamp_us() const { return global_min_ts_; }
    std::uint64_t global_max_timestamp_us() const { return global_max_ts_; }

    // Lazily-built activity summary. Null until the build finishes.
    const VizSummary* viz_summary() const {
        return viz_summary_state_.load(std::memory_order_acquire) == 2
                   ? viz_summary_.get()
                   : nullptr;
    }
    // Serializes the build so concurrent requests wait for it instead of each
    // launching a whole-trace live scan of its own.
    coro::AsyncMutex& viz_summary_mutex() { return viz_summary_mutex_; }
    void set_viz_summary(std::unique_ptr<VizSummary> summary) {
        viz_summary_ = std::move(summary);
        viz_summary_state_.store(2, std::memory_order_release);
    }

    // Resolve a content hash (file/host/string) to its name via a point lookup
    // in the per-root index databases, which are opened once and kept. Empty
    // when no root knows the hash.
    using HashType =
        dftracer::utils::utilities::indexer::IndexDatabase::HashType;
    std::string resolve_hash(HashType type, const std::string& hash);

    // On-disk summary cache (index_dir/.dftviz_summary), keyed by a fingerprint
    // of the current file set so a re-indexed trace invalidates it. Loading it
    // skips the full rescan on restart.
    bool load_persisted_viz_summary();
    void persist_viz_summary() const;

   private:
    std::string viz_summary_cache_path() const;
    std::string viz_summary_fingerprint() const;

    std::string directory_;
    std::string index_dir_;
    std::vector<FileInfo> files_;
    std::unordered_map<std::string, std::size_t> path_to_index_;
    std::uint64_t global_min_ts_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t global_max_ts_ = 0;
    TimeMetric time_metric_ = TimeMetric::US;
    std::size_t max_concurrent_;
    std::size_t checkpoint_size_;
    BloomCache bloom_cache_;
    VizResultCache viz_cache_{VIZ_RESULT_CACHE_BYTES};

    std::mutex hash_db_mutex_;
    std::unordered_map<std::string, std::string> hash_names_;
    std::unordered_map<
        std::string,
        std::shared_ptr<dftracer::utils::utilities::indexer::IndexDatabase>>
        hash_dbs_;

    coro::AsyncMutex viz_summary_mutex_;
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
