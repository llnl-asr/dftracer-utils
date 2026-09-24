#ifndef DFTRACER_UTILS_TRACE_INDEXING_INDEX_RESOLVER_UTILITY_H
#define DFTRACER_UTILS_TRACE_INDEXING_INDEX_RESOLVER_UTILITY_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_file_entry_capability.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::trace::indexing {

struct ResolvedFile {
    std::size_t file_index = 0;
    std::string file_path;
    std::int32_t file_id = -1;
    utilities::indexer::IndexFileEntryCapability capabilities =
        utilities::indexer::IndexFileEntryCapability::NONE;
};

struct FileWorkItem {
    std::size_t file_index = 0;
    std::string file_path;
    std::int32_t file_id = -1;
};

struct ResolverInput {
    std::string directory;
    std::string index_dir;
    std::vector<std::string> files;

    bool require_checkpoints = true;
    bool require_bloom = false;
    bool require_aggregation = false;

    /// Checkpoint size the caller intends to build with. When non-zero, a file
    /// whose stored checkpoint size differs is rebuilt, so a changed
    /// `--checkpoint-size` (CLI) or `checkpoint_size=` (Python) takes effect
    /// without `--force`. Zero means do not compare.
    std::size_t checkpoint_size = 0;

    /// Args fields the bloom tier must cover; a file indexed without one of
    /// them needs its bloom tier rebuilt. Checked only with require_bloom.
    std::vector<std::string> bloom_fields;

    /// Full config for computing hash with stored time_interval
    std::optional<aggregators::AggregationConfig> aggregation_config;
};

struct ResolverResult {
    std::vector<std::string> all_files;
    std::vector<std::size_t> all_file_sizes;
    std::vector<std::uint64_t> all_file_mtimes;
    std::string index_path;

    std::vector<FileWorkItem> needs_checkpoint;
    std::vector<FileWorkItem> needs_bloom;
    std::vector<FileWorkItem> needs_aggregation;

    std::vector<ResolvedFile> cached;

    /// Aggregation augmentation info (when cached aggregation exists with
    /// different time_interval)
    bool needs_augmentation = false;
    std::uint64_t stored_time_interval_us =
        0;  ///< Time interval in cached data

    /// A registered file's source changed since indexing (mtime/size mismatch).
    bool stale_detected = false;

    std::size_t total_cached() const { return cached.size(); }
};

class IndexResolverUtility {
   public:
    coro::CoroTask<ResolverResult> operator()(CoroScope& ctx,
                                              const ResolverInput& input) const;

    /// Scope-less overload: opens its own CoroScope on the current executor.
    coro::CoroTask<ResolverResult> operator()(
        const ResolverInput& input) const {
        return with_scope(*this, input);
    }

   private:
    utilities::filesystem::PatternDirectoryScannerUtility scanner_;
};

}  // namespace dftracer::utils::trace::indexing

#endif  // DFTRACER_UTILS_TRACE_INDEXING_INDEX_RESOLVER_UTILITY_H
