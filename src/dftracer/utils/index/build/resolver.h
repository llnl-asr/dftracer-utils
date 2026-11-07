// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_BUILD_RESOLVER_H
#define DFTRACER_UTILS_INDEX_BUILD_RESOLVER_H

#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/runtime/fs/fs.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::build {

struct ResolvedFile {
    std::size_t file_index = 0;
    dstd::String file_path;
    /// The schema the file was indexed with.
    dstd::String schema;
};

struct FileWorkItem {
    std::size_t file_index = 0;
    dstd::String file_path;
    /// For needs_bloom: the tier extensions that are missing or stale.
    index::store::ExtensionMask extensions;
    /// For needs_bloom: the schema the file was indexed with, which a tier
    /// rebuild decodes it with.
    dstd::String schema;
};

struct ResolverInput {
    dstd::String directory;
    dstd::String index_dir;
    dstd::Vector<dstd::String> files;
    /// Files already scanned, with the size and mtime that scan read. When not
    /// empty they replace `directory` and `files`: nothing is walked or
    /// stat'ed again.
    dstd::Vector<runtime::fs::FoundFile> entries;

    bool require_checkpoints = true;
    bool require_bloom = false;

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
    dstd::Optional<ChunkIndexerConfig> bloom_config;

    /// The schema a build would use; empty accepts the recorded one. A file
    /// with no current schema record, or another schema, is rebuilt whole.
    dstd::String schema;

    /// A file whose rows were built with another config needs aggregation work.
    index::schemas::dft::agg::AggregationConfig aggregation_config =
        index::schemas::dft::agg::standard_aggregation_config();
};

struct ResolverResult {
    dstd::Vector<dstd::String> all_files;
    dstd::Vector<std::size_t> all_file_sizes;
    dstd::Vector<std::uint64_t> all_file_mtimes;
    dstd::String index_path;

    dstd::Vector<FileWorkItem> needs_checkpoint;
    dstd::Vector<FileWorkItem> needs_bloom;
    dstd::Vector<FileWorkItem> needs_aggregation;

    dstd::Vector<ResolvedFile> cached;

    /// The stored aggregation tier differs from the requested config only in
    /// its interval, which `stored_time_interval_us` holds.
    bool needs_augmentation = false;
    std::uint64_t stored_time_interval_us = 0;

    /// A registered file's source changed since indexing (mtime/size mismatch).
    bool stale_detected = false;
    /// Index roots whose aggregation tier must be cleared before a build:
    /// rows built with other params, or of a file rebuilt whole. Their
    /// dftracer files are all in needs_aggregation when aggregating.
    dstd::Vector<dstd::String> stale_aggregation_roots;

    /// Files a build attempted and could not index, with the reason.
    struct Failure {
        dstd::String file_path;
        dstd::String message;
    };
    dstd::Vector<Failure> failures;

    std::size_t total_cached() const { return cached.size(); }
};

/// The files of `result` as scanned, to resolve them again without a scan.
dstd::Vector<runtime::fs::FoundFile> scanned_entries(
    const ResolverResult& result);
dstd::Vector<runtime::fs::FoundFile> scanned_entries(
    const ResolverResult& result, const dstd::Vector<FileWorkItem>& items);

class Resolver {
   public:
    coro::CoroTask<ResolverResult> operator()(coro::Scope& ctx,
                                              const ResolverInput& input) const;

    /// Scope-less overload: opens its own scope on the current executor.
    coro::CoroTask<ResolverResult> operator()(
        const ResolverInput& input) const {
        return coro::with_scope(
            [this, input](coro::Scope& s) { return (*this)(s, input); });
    }
};

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_RESOLVER_H
