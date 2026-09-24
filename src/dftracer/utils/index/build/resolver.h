#ifndef DFTRACER_UTILS_INDEX_BUILD_RESOLVER_H
#define DFTRACER_UTILS_INDEX_BUILD_RESOLVER_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::index::build {

struct ResolvedFile {
    std::size_t file_index = 0;
    std::string file_path;
    std::int32_t file_id = -1;
    /// The schema the file was indexed with.
    std::string schema;
};

struct FileWorkItem {
    std::size_t file_index = 0;
    std::string file_path;
    std::int32_t file_id = -1;
    /// For needs_bloom: the tier extensions that are missing or stale.
    index::store::ExtensionMask extensions;
    /// For needs_bloom: the schema the file was indexed with, which a tier
    /// rebuild decodes it with.
    std::string schema;
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

    /// The tier the caller would build. A file needs tier work when one of
    /// its extensions is missing, not current or built with other settings,
    /// or when bloom and zonemap together lack one of its extra_dimensions
    /// (normalized by extra_dimension_name). Unset checks only that the tier
    /// is current. Checked only with require_bloom.
    std::optional<ChunkIndexerConfig> bloom_config;

    /// The schema a build would use; empty accepts the recorded one. A file
    /// with no current schema record, or another schema, is rebuilt whole.
    std::string schema;

    /// Full config for computing hash with stored time_interval
    std::optional<index::schemas::dft::agg::AggregationConfig>
        aggregation_config;
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

    /// The stored aggregation tier differs from the requested config only in
    /// its interval, which `stored_time_interval_us` holds.
    bool needs_augmentation = false;
    std::uint64_t stored_time_interval_us = 0;

    /// A registered file's source changed since indexing (mtime/size mismatch)
    /// or an index is in another format.
    bool stale_detected = false;
    /// Index roots in another format, which are rebuilt whole.
    std::vector<std::string> outdated_roots;
    /// Index roots whose aggregation tier must be cleared before a build:
    /// rows built with other params, or of a file rebuilt whole. Their
    /// dftracer files are all in needs_aggregation when aggregating.
    std::vector<std::string> stale_aggregation_roots;

    /// Files a build attempted and could not index, with the reason.
    struct Failure {
        std::string file_path;
        std::string message;
    };
    std::vector<Failure> failures;

    std::size_t total_cached() const { return cached.size(); }
};

class Resolver {
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

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_RESOLVER_H
