// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_SERVER_TRACE_INDEX_H
#define DFTRACER_UTILS_DOMAINS_SERVER_TRACE_INDEX_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/coro/async_mutex.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/domains/server/viz_result_cache.h>
#include <dftracer/utils/domains/server/viz_summary.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/index/gzip/member.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <dftracer/utils/trace/index/schemas/dft/statistics.h>
#include <dftracer/utils/trace/time_metric.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace dftracer::utils::domains::server {

/// Byte budget for the per-index viz result cache.
inline constexpr std::size_t VIZ_RESULT_CACHE_BYTES = 256UL * 1024 * 1024;

/// Scans a directory for trace files and caches paths to their
/// root-local `.dftindex` database. Used by API handlers to resolve file
/// paths and check index availability.
class TraceIndex {
   public:
    struct FileInfo {
        dstd::String path;
        dstd::String index_path;
        bool has_bloom_data = false;
        bool has_checkpoint_index = false;
        std::uint64_t min_timestamp_us = 0;
        std::uint64_t max_timestamp_us = 0;
        std::uint64_t compressed_size = 0;
        std::uint64_t uncompressed_size = 0;
        std::size_t checkpoint_count = 0;
        std::uint64_t checkpoint_size = 0;
        std::uint64_t line_count = 0;
        double size_mb = 0;
    };

    /// Per-file chunk metadata read once from the immutable index: byte spans
    /// and per-chunk statistics, keyed by checkpoint index. Shared read-only.
    struct FileChunkMeta {
        dstd::Vector<dftracer::utils::trace::index::gzip::ChunkSpan> spans;
        dstd::Vector<
            dftracer::utils::trace::index::schemas::dft::ChunkStatisticsResult>
            stats;
    };

    TraceIndex(const dstd::String& directory, const dstd::String& index_dir,
               std::size_t max_concurrent = 8,
               std::size_t checkpoint_size =
                   common::constants::indexer::DEFAULT_CHECKPOINT_SIZE);
    /// Waits for the store writes this index started and flushes the store
    /// once, so a later process reads every stored response.
    ~TraceIndex();

    /// Scan directory and populate the file list. The error is the directory
    /// scan or index lookup that failed; a file whose index cannot be read is
    /// logged and served without it.
    coro::CoroTask<dstd::Result<void>> initialize();

    std::size_t file_count() const { return files_.size(); }
    const dstd::Vector<FileInfo>& files() const { return files_; }

    /// Find a file by its path. Returns nullptr if not found.
    const FileInfo* find_file(const dstd::String& path) const;

    /// Find a file by index. Returns nullptr if out of range.
    const FileInfo* file_at(std::size_t index) const;

    /// Chunk spans + statistics for a file, read from the index on first use
    /// and cached for the process lifetime (the index is immutable). Returns
    /// nullptr when the file has no index or the read fails. Thread-safe.
    dstd::SharedPtr<const FileChunkMeta> chunk_meta(const FileInfo& file);

    const dstd::String& directory() const { return directory_; }
    const dstd::String& index_dir() const { return index_dir_; }
    std::size_t max_concurrent() const { return max_concurrent_; }

    /// The record schema of the served files (a viewer reads one), from the
    /// first file's index record or, without one, detected from the file;
    /// dftracer for an empty directory. Valid after initialize().
    const dftracer::utils::trace::index::RecordSchema& record_schema() const {
        return *record_schema_;
    }

    /// Result cache for heavy viz endpoints: an in-memory LRU in front of the
    /// folder's copies in the index store, keyed by the folder's fingerprint.
    VizResultCache& viz_cache() { return viz_cache_; }

    /// Trace-wide native time unit (one unit per trace), from the leading CM
    /// time_metric of the first file (absent = US). Index/event ts and dur are
    /// stored in this unit; the viz layer converts at its us boundary.
    using TimeMetric = dftracer::utils::trace::TimeMetric;
    TimeMetric time_metric() const { return time_metric_; }
    std::uint64_t native_to_us(std::uint64_t v) const {
        return dftracer::utils::trace::scale_between(time_metric_,
                                                     TimeMetric::Us, v);
    }
    std::uint64_t us_to_native(std::uint64_t v) const {
        return dftracer::utils::trace::scale_between(TimeMetric::Us,
                                                     time_metric_, v);
    }

    /// Native (index-unit) global bounds. The `_us` names are historical: for a
    /// US trace they are microseconds; for NS/MS/SEC traces use native_to_us().
    std::uint64_t global_min_timestamp_us() const { return global_min_ts_; }
    std::uint64_t global_max_timestamp_us() const { return global_max_ts_; }

    /// Lazily-built activity summary. Null until the build finishes.
    const VizSummary* viz_summary() const {
        return viz_summary_state_.load(std::memory_order_acquire) == 2
                   ? viz_summary_.get()
                   : nullptr;
    }
    /// Serializes the build so concurrent requests wait for it instead of each
    /// launching a whole-trace live scan of its own.
    coro::AsyncMutex& viz_summary_mutex() { return viz_summary_mutex_; }
    void set_viz_summary(dstd::UniquePtr<VizSummary> summary) {
        viz_summary_ = std::move(summary);
        viz_summary_state_.store(2, std::memory_order_release);
    }

    /// Column `value` of the row of row set `rowset` whose column `key` is
    /// `k`, read from the row sets the index build stored; each column pair
    /// loads once and stays. Empty when no row matches or no index holds
    /// the row set.
    dstd::String resolve(dstd::StringView rowset, dstd::StringView key,
                         dstd::StringView value, const dstd::String& k);

    /// Summary cache in the index root's store, under
    /// `server.viz_summary`, in this folder's scope. The stored value carries
    /// a fingerprint of the current file set and their mtimes and sizes, so
    /// loading it skips the full rescan on restart.
    bool load_persisted_viz_summary();
    /// Commits the summary without flushing: the index flushes the store once,
    /// at shutdown.
    coro::CoroTask<dstd::Result<void>> persist_viz_summary();

    /// The stored viz results of `request_keys`, in order, from one store
    /// read. Nothing for a key the store holds no copy of, or a copy of
    /// another fingerprint.
    dstd::Vector<dstd::Optional<dstd::String>> load_viz_results(
        const dstd::Vector<dstd::String>& request_keys);
    /// Hands a viz result to a store write this index owns and returns: no
    /// request waits for the store. The write is keyed by the folder's
    /// fingerprint, and a failure is logged and dropped.
    void store_viz_result(dstd::String request_key, dstd::String body);

    /// Fingerprint of the indexed folder: its record schema, its time bounds
    /// and each file's path, size, mtime, checkpoint count and timestamps. A
    /// stored value keyed by it is never served after the folder is re-indexed
    /// or a file changes. Computed once, at the end of @ref initialize.
    const dstd::String& folder_fingerprint() const {
        return folder_fingerprint_;
    }

    /// Test hook: records that this index computed the summary rather than
    /// loading it from the store, so a test can tell a cache hit from a
    /// rebuild.
    void note_viz_summary_compute() {
        viz_summary_compute_count_.fetch_add(1, std::memory_order_relaxed);
    }
    /// Test hook: how many times @ref note_viz_summary_compute ran.
    std::size_t viz_summary_compute_count() const {
        return viz_summary_compute_count_.load(std::memory_order_relaxed);
    }
    /// Test hook: records that a viz result was in neither the memory cache nor
    /// the store, so this index computed it.
    void note_viz_result_compute() {
        viz_result_compute_count_.fetch_add(1, std::memory_order_relaxed);
    }
    /// Test hook: how many times @ref note_viz_result_compute ran.
    std::size_t viz_result_compute_count() const {
        return viz_result_compute_count_.load(std::memory_order_relaxed);
    }
    /// Test hook: how many times a viz result was looked for in the store, so a
    /// test can tell a memory hit from a store read.
    std::size_t viz_result_store_read_count() const {
        return viz_result_store_read_count_.load(std::memory_order_relaxed);
    }
    /// Test hook: how many times the index flushed the viz store at shutdown.
    std::size_t viz_store_flush_count() const {
        return viz_store_flush_count_.load(std::memory_order_relaxed);
    }

    /// Waits for every stored-response write this index started, then flushes
    /// the store once. The destructor calls it; call it before closing the
    /// index stores from outside, such as with release_all_roots.
    void drain_viz_writes();

   private:
    /// The folder's stored viz responses: one store handle for the life of the
    /// index, the writes no request waits for, and the bytes the folder keeps
    /// stored. Shared with the writes, which outlive the request that started
    /// them; the destructor waits for them.
    struct VizStore;

    /// The folder's store, opened on first use. Null when it cannot be opened.
    dstd::SharedPtr<VizStore> viz_store();
    /// One store write of one response, on the store's own runtime.
    static coro::CoroTask<void> write_viz_result(dstd::SharedPtr<VizStore> viz,
                                                 dstd::String stored_key,
                                                 dstd::String body,
                                                 dstd::String directory);
    /// The fingerprint of the folder as it stands, from one stat per file.
    dstd::String compute_folder_fingerprint() const;

    dstd::String directory_;
    dstd::String index_dir_;
    dstd::Vector<FileInfo> files_;
    dstd::HashMap<dstd::String, std::size_t> path_to_index_;
    std::uint64_t global_min_ts_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t global_max_ts_ = 0;
    TimeMetric time_metric_ = TimeMetric::Us;
    const dftracer::utils::trace::index::RecordSchema* record_schema_ = nullptr;
    std::size_t max_concurrent_;
    std::size_t checkpoint_size_;
    VizResultCache viz_cache_{VIZ_RESULT_CACHE_BYTES, this};

    std::mutex chunk_meta_mutex_;
    dstd::HashMap<dstd::String, dstd::SharedPtr<const FileChunkMeta>>
        chunk_meta_;

    std::mutex rowset_mutex_;
    dstd::HashMap<dstd::String, dstd::HashMap<dstd::String, dstd::String>>
        rowset_columns_;

    coro::AsyncMutex viz_summary_mutex_;
    dstd::UniquePtr<VizSummary> viz_summary_;
    std::atomic<int> viz_summary_state_{
        0};  ///< 0 not built, 1 building, 2 ready
    std::atomic<std::size_t> viz_summary_compute_count_{0};
    std::atomic<std::size_t> viz_result_compute_count_{0};
    std::atomic<std::size_t> viz_result_store_read_count_{0};
    std::atomic<std::size_t> viz_store_flush_count_{0};

    dstd::String folder_fingerprint_;
    std::mutex viz_store_mutex_;
    dstd::SharedPtr<VizStore> viz_store_;
};

class QueryParams;

/// Collect the candidate files for a streaming query: the explicit `?file=`
/// (when present and found in the index) or all indexed files. Callers apply
/// their own timestamp-overlap filter on top of this.
dstd::Vector<const TraceIndex::FileInfo*> collect_candidate_files(
    TraceIndex& index, const QueryParams& params);

}  // namespace dftracer::utils::domains::server

#endif  // DFTRACER_UTILS_DOMAINS_SERVER_TRACE_INDEX_H
