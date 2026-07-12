#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_INDEX_RESOLVER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_INDEX_RESOLVER_UTILITY_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/tags/needs_context.h>
#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_file_entry_capability.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

struct ResolvedFile {
    std::size_t file_index = 0;
    std::string file_path;
    std::int32_t file_id = -1;
    indexer::IndexFileEntryCapability capabilities =
        indexer::IndexFileEntryCapability::NONE;
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
    bool require_manifest = false;
    bool require_aggregation = false;

    // Full config for computing hash with stored time_interval
    std::optional<aggregators::AggregationConfig> aggregation_config;
};

struct ResolverResult {
    std::vector<std::string> all_files;
    std::vector<std::size_t> all_file_sizes;
    std::vector<std::uint64_t> all_file_mtimes;
    std::string index_path;

    std::vector<FileWorkItem> needs_checkpoint;
    std::vector<FileWorkItem> needs_bloom;
    std::vector<FileWorkItem> needs_manifest;
    std::vector<FileWorkItem> needs_aggregation;

    std::vector<ResolvedFile> cached;

    // Aggregation augmentation info (when cached aggregation exists with
    // different time_interval)
    bool needs_augmentation = false;
    std::uint64_t stored_time_interval_us = 0;  // Time interval in cached data

    // A registered file's source changed since indexing (mtime/size mismatch).
    bool stale_detected = false;

    std::size_t total_needs_work() const {
        return needs_checkpoint.size() + needs_bloom.size() +
               needs_manifest.size() + needs_aggregation.size();
    }

    std::size_t total_cached() const { return cached.size(); }
};

class IndexResolverUtility
    : public utilities::Utility<ResolverInput, ResolverResult,
                                utilities::tags::NeedsContext> {
   private:
    filesystem::PatternDirectoryScannerUtility scanner_;

   public:
    coro::CoroTask<ResolverResult> process(const ResolverInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_INDEX_RESOLVER_UTILITY_H
