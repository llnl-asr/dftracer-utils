#ifndef DFTRACER_UTILS_TRACE_VIEWS_SHARDED_VIEW_H
#define DFTRACER_UTILS_TRACE_VIEWS_SHARDED_VIEW_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/views/view.h>

#include <functional>
#include <string>
#include <vector>

namespace dftracer::utils::trace::views {

/// Query a set of immutable, read-only index shards as one logical index.
///
/// Each shard is a self-contained `.dftindex` covering its own set of trace
/// files. A ShardedView enumerates every shard's files, applies the caller's
/// View configuration to a single-index View per shard (so each shard keeps its
/// aggregation fast path, which the executor only takes when all files share
/// one index), runs the per-shard `aggregate_partial()`, and reduces the
/// partials with `merge_partials_to_table()` / `merge_counter_partials()`. This
/// is the in-process form of the distributed MPI reduce in `dftracer_view`.
///
/// Fully read-only. Every shard is opened read-only for file enumeration and
/// for the per-shard `aggregate_partial()` (the tier read opens the aggregation
/// DB read-only; the scan fallback reads the index without writing). It never
/// builds, rebuilds, or persists a rollup - unlike `collect()`/`materialize()`,
/// which do - so it takes no RocksDB write lock and is safe against shards on a
/// read-only mount or NFS.
class ShardedView {
   public:
    /// Applies the caller's view options to a base View and returns it, as
    /// the `configure` lambda in `dftracer_view` does. The same configuration
    /// is applied to every shard and to the merger, so all partials are
    /// compatible.
    using Configure = std::function<View(View)>;

    /// Discover shards by reading `<root>/shards.json`; each shard's `path`
    /// resolves relative to `root`. Empty shards are dropped. Throws
    /// DFTUtilsException when the manifest is absent, unreadable, or built with
    /// a schema version this build cannot read.
    static ShardedView from_manifest(const std::string& root);

    /// Use an explicit set of shard index directories, bypassing the manifest.
    static ShardedView from_shard_dirs(std::vector<std::string> shard_dirs);

    const std::vector<std::string>& shard_dirs() const { return shard_dirs_; }

    /// Aggregate (group_by/agg) across all shards into one table. `configure`
    /// must set an aggregation; a raw-event configuration has no partial to
    /// merge.
    coro::CoroTask<dftracer::utils::dataframe::DataFrame> aggregate(
        Configure configure) const;

    /// Aggregate a counter query across all shards, emitting the merged rows as
    /// ph="C" counter events into `sink`.
    coro::CoroTask<ExportStats> aggregate_counters(Configure configure,
                                                   ExportSink& sink) const;

   private:
    explicit ShardedView(std::vector<std::string> shard_dirs)
        : shard_dirs_(std::move(shard_dirs)) {}

    std::vector<ViewFile> shard_view_files(const std::string& shard_dir) const;

    std::vector<std::string> shard_dirs_;
};

/// Write a `shards.json` manifest at `root` cataloging `shard_dirs` as one
/// shard set. Each shard directory is opened read-only to record its file-id
/// range and file count. A shard path is stored relative to `root` when it
/// lives under it, else absolute. Overwrites any existing manifest atomically.
/// A later `ShardedView::from_manifest(root)` reads the set back.
void write_shard_set(const std::string& root,
                     const std::vector<std::string>& shard_dirs);

/// Consolidate a shard set into one unified index (with its aggregation tier)
/// at `out_root`, rebuilt from the shards' trace files, then write a
/// single-shard manifest there. Returns the number of files consolidated. A
/// later query over `out_root` opens one index instead of N.
///
/// Requires the trace files to be present and to have distinct logical
/// filenames across shards (the registry keys on filename). Scan-fallback
/// queries need the traces under one directory; a tier-answered query does not
/// read them. co_await it.
coro::CoroTask<std::size_t> consolidate_shard_set(CoroScope* scope,
                                                  const std::string& root,
                                                  const std::string& out_root);

/// Merge a shard set's aggregation tiers into one consolidated tier index at
/// `out_root` WITHOUT re-reading traces. Each shard's tier is re-keyed into a
/// unified intern dictionary and combined by the merge operator; the result
/// answers tier-covered queries from a single index. Unlike
/// consolidate_shard_set it does not rebuild per-file data, so scan-fallback
/// queries are not served by it. Returns the number of files cataloged.
std::size_t merge_shard_set(const std::string& root,
                            const std::string& out_root);

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_SHARDED_VIEW_H
