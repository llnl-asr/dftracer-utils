#ifndef DFTRACER_UTILS_SERVER_TRACE_INDEX_H
#define DFTRACER_UTILS_SERVER_TRACE_INDEX_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter_cache.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::server {

/// Scans a directory for trace files and caches paths to their
/// sidecar index file (.idx). Used by API handlers to resolve file
/// paths and check index availability.
class TraceIndex {
   public:
    // Files below this compressed size are streamed directly without
    // building a sidecar index file (.idx).  At 8 MB compressed
    // (~160 MB uncompressed with typical 20x JSON compression), a file
    // has only a handful of 32 MB checkpoints -- the indexing overhead
    // exceeds the benefit of bloom-filter skip.
    static constexpr std::size_t INDEX_SIZE_THRESHOLD =
        constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD;

    struct FileInfo {
        std::string path;
        std::string idx_path;
        bool has_bloom_data = false;
        bool has_checkpoint_index = false;
        bool is_small = false;
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

   private:
    std::string directory_;
    std::string index_dir_;
    std::vector<FileInfo> files_;
    std::unordered_map<std::string, std::size_t> path_to_index_;
    std::uint64_t global_min_ts_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t global_max_ts_ = 0;
    std::size_t max_concurrent_;
    BloomCache bloom_cache_;
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_TRACE_INDEX_H
