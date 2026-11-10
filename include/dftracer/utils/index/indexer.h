// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_INDEXER_H
#define DFTRACER_UTILS_INDEX_INDEXER_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/runtime/runtime.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::runtime::fs {
struct FoundFile;
}  // namespace dftracer::utils::runtime::fs

namespace dftracer::utils::index {

/// The bloom and dimension statistics tier.
struct BloomOptions {
    /// A file whose index lacks this tier (or one of `fields`) needs work.
    bool required = true;
    /// Args fields indexed by name, besides the automatic ones.
    dstd::Vector<dstd::String> fields;
    double false_positive_rate = 0.01;
    /// Also index each file's other args paths, most frequent first, while
    /// their evidence fits `stats_share` (in (0, 1]) of the file's size, at
    /// least 8 MiB, and, when `path_budget` is above 0, at most
    /// `path_budget` of them; a string one while a chunk holds at most
    /// `auto_max_distinct` of its values.
    std::size_t path_budget = 0;
    double stats_share = 0.05;
    std::size_t auto_max_distinct = 256;
    std::size_t expected_entries_per_chunk = 1024;
};

struct IndexerOptions {
    /// Directory holding the index; empty puts it beside each trace.
    dstd::String index_dir;
    std::size_t checkpoint_size =
        common::constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    /// Worker count; 0 uses all cores.
    std::size_t parallelism = 0;
    bool checkpoints = true;
    /// nullopt skips the bloom tier.
    dstd::Optional<BloomOptions> bloom = BloomOptions{};
    /// The pruning extensions the bloom tier builds: any of "zonemap",
    /// "bloom", "counts" and "postings". dft.stats and dft.metadata are built
    /// with the tier.
    dstd::Vector<dstd::String> extensions = {"zonemap", "bloom", "counts",
                                             "postings"};
    /// The aggregation tier every build makes. A caller may change the windows,
    /// keys and sketches but cannot turn it off.
    schemas::dft::agg::AggregationConfig aggregation =
        schemas::dft::agg::standard_aggregation_config();
    /// Schema every trace is decoded with ("dftracer" or "generic");
    /// empty detects each file's from its first lines.
    dstd::String schema;
    /// Bytes a build may hold at once, as View::memory_budget: 0 is about a
    /// third of available memory, memory::NO_SPILL_BUDGET leaves it unbounded.
    std::uint64_t memory_budget = 0;
    /// Runtime for the blocking calls; nullptr uses default_runtime(). Not
    /// owned; it must outlive the Indexer.
    Runtime* runtime = nullptr;
};

struct IndexStatus {
    std::size_t total = 0;
    dstd::Vector<dstd::String> ready;
    dstd::Vector<dstd::String> needs_work;
    /// Ready files whose last gzip member was cut short when indexed; their
    /// recovered tail is not checksum-verified.
    dstd::Vector<dstd::String> truncated;
    dstd::String index_path;
    std::uint64_t aggregation_interval_us = 0;
    /// The stored aggregation tier was built with another config than the
    /// requested one, or holds rows of a file that changed, so the next build
    /// clears and rebuilds it; the other extensions are kept.
    bool aggregation_needs_rebuild = false;
    /// Files indexed by this call; always 0 for status().
    std::size_t indexed = 0;
};

struct IndexedFile {
    dstd::String path;
    dstd::String index_path;
    /// Size and modification time recorded when the file was indexed.
    std::uint64_t size_bytes = 0;
    std::uint64_t mtime = 0;
    /// The last gzip member was cut short when the file was indexed.
    bool truncated = false;
    /// The schema its records were decoded with.
    dstd::String schema;
};

/// One manifest entry of a file.
struct ExtensionEntry {
    /// Such as "core.members" or "zonemap".
    dstd::String name;
    std::uint32_t version = 0;
    std::uint64_t params_hash = 0;
    /// False when the build of the extension failed.
    bool ready = false;
    /// Ready, at the current version, and built with the settings in
    /// @ref IndexerOptions.
    bool current = false;
};

struct FileManifest {
    dstd::String path;
    dstd::String index_path;
    /// The extensions built for the file, in manifest order.
    dstd::Vector<ExtensionEntry> extensions;
};

/// What one pruning extension rules out of a file on its own.
struct ExtensionPrune {
    dstd::String name;
    bool file_ruled_out = false;
    /// Chunks ruled out, ascending.
    dstd::Vector<std::uint64_t> removed;
};

struct FileExplain {
    dstd::String path;
    /// False when the file is not in the index; nothing is pruned then.
    bool indexed = false;
    bool may_match = true;
    std::uint64_t chunks = 0;
    /// Chunks the query reads, ascending: the chunks a View scan reads.
    dstd::Vector<std::uint64_t> read;
    /// One entry per pruning extension current for the file.
    dstd::Vector<ExtensionPrune> extensions;
};

/// The manifest or explanation as a JSON array of objects whose keys are the
/// field names above; params_hash is a 16-digit hex string, since a JSON
/// number may not hold 64 bits.
dstd::String to_json(const dstd::Vector<FileManifest>& manifest);
dstd::String to_json(const dstd::Vector<FileExplain>& explain);

class Indexer;

namespace detail {
/// Opens trace files a caller already scanned, with the size and mtime that
/// scan read. The first status(), build() or rebuild() of the @ref Indexer
/// uses them without stat'ing. Later calls stat the files. An INVALID_ARGUMENT
/// error when `files` is empty or an option is invalid.
dstd::Result<Indexer> open_scanned(dstd::Vector<runtime::fs::FoundFile> files,
                                   IndexerOptions options);
}  // namespace detail

/// Builds and inspects the index of a fixed set of traces.
///
/// Copies share one state. Use an Indexer from one thread at a time; separate
/// Indexers over the same index root are serialized by the index write lock.
/// Every call that can fail returns a @ref dftracer::utils::dstd::Result:
/// INVALID_ARGUMENT or NOT_FOUND from open, and the engine's I/O or
/// compression error, naming the trace, from build.
class Indexer {
   public:
    /// `paths` holds trace files and directories; a directory contributes the
    /// trace files (.pfw, .jsonl and .ndjson, plain or gzip) directly
    /// inside it. Reads nothing else and writes nothing.
    /// An INVALID_ARGUMENT error for no paths or an invalid option, and
    /// NOT_FOUND for a path that does not exist.
    static dstd::Result<Indexer> open(dstd::Vector<dstd::String> paths,
                                      IndexerOptions options = {});

    /// Which files are ready for the requested tiers. Writes nothing.
    coro::CoroTask<dstd::Result<IndexStatus>> status(coro::Scope& scope) const;
    /// Builds the requested tiers for files that lack them or changed since
    /// they were indexed; fresh files are left alone.
    coro::CoroTask<dstd::Result<IndexStatus>> build(coro::Scope& scope);
    /// Rebuilds the requested tiers for every file.
    coro::CoroTask<dstd::Result<IndexStatus>> rebuild(coro::Scope& scope);
    /// The indexed files; files not yet indexed are omitted.
    coro::CoroTask<dstd::Result<dstd::Vector<IndexedFile>>> files(
        coro::Scope& scope) const;
    /// The manifest of every indexed file. Writes nothing.
    coro::CoroTask<dstd::Result<dstd::Vector<FileManifest>>> manifest(
        coro::Scope& scope) const;
    /// Per file, the chunks `query` reads and what each pruning extension
    /// rules out alone. Writes nothing. An INVALID_ARGUMENT error when the
    /// query does not parse.
    coro::CoroTask<dstd::Result<dstd::Vector<FileExplain>>> explain(
        coro::Scope& scope, dstd::String query) const;
    /// Rewrites one tier extension ("zonemap", "bloom", "counts", "postings",
    /// "dft.stats", "dft.metadata" or a registered plugin extension's name)
    /// of every file, and builds files not yet indexed. Other extensions are
    /// left as they are. An INVALID_ARGUMENT error for another name.
    coro::CoroTask<dstd::Result<IndexStatus>> rebuild_extension(
        coro::Scope& scope, dstd::String extension);
    /// Removes one tier extension's data and manifest entries from every
    /// indexed file; the next build makes it again only when
    /// IndexerOptions::extensions names it, or for a plugin extension while
    /// it is registered. An INVALID_ARGUMENT error as @ref rebuild_extension
    /// gives.
    coro::CoroTask<dstd::Result<IndexStatus>> drop_extension(
        coro::Scope& scope, dstd::String extension);

    /// Blocking forms, run on IndexerOptions::runtime.
    dstd::Result<IndexStatus> status() const;
    dstd::Result<IndexStatus> build();
    dstd::Result<IndexStatus> rebuild();
    dstd::Result<dstd::Vector<IndexedFile>> files() const;
    dstd::Result<dstd::Vector<FileManifest>> manifest() const;
    dstd::Result<dstd::Vector<FileExplain>> explain(
        dstd::StringView query) const;
    dstd::Result<IndexStatus> rebuild_extension(dstd::StringView extension);
    dstd::Result<IndexStatus> drop_extension(dstd::StringView extension);
    /// The rows the build stored for row set `name` of the files' source,
    /// every indexed file's in open order. An INVALID_ARGUMENT error when a
    /// file's index holds no rows for it (a row set the build does not
    /// evaluate, or an index built before it); read such a row set with
    /// View::duql("from <name>").
    dstd::Result<dataframe::DataFrame> rowset(dstd::String name) const;

    /// The expanded trace list, in open order.
    const dstd::Vector<dstd::String>& paths() const;
    const IndexerOptions& options() const;

   private:
    friend dstd::Result<Indexer> detail::open_scanned(
        dstd::Vector<runtime::fs::FoundFile> files, IndexerOptions options);
    struct Impl;
    explicit Indexer(dstd::SharedPtr<Impl> impl);
    static dstd::Result<void> validate_options(Impl& impl);
    static dstd::Result<Indexer> finish_open(dstd::SharedPtr<Impl> impl);
    coro::CoroTask<dstd::Result<IndexStatus>> status_of(
        coro::Scope& scope, dstd::Vector<runtime::fs::FoundFile> entries) const;
    dstd::SharedPtr<Impl> impl_;
};

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_INDEXER_H
