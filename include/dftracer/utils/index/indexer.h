#ifndef DFTRACER_UTILS_INDEX_INDEXER_H
#define DFTRACER_UTILS_INDEX_INDEXER_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::index {

/// The bloom and dimension statistics tier.
struct BloomOptions {
    /// A file whose index lacks this tier (or one of `fields`) needs work.
    bool required = true;
    /// Args fields indexed by name, besides the automatic ones.
    std::vector<std::string> fields;
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
    std::string index_dir;
    std::size_t checkpoint_size = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    /// Worker count; 0 uses all cores.
    std::size_t parallelism = 0;
    bool checkpoints = true;
    /// nullopt skips the bloom tier.
    std::optional<BloomOptions> bloom = BloomOptions{};
    /// The pruning extensions the bloom tier builds: any of "zonemap",
    /// "bloom", "counts" and "postings". dft.stats and dft.metadata are built
    /// with the tier.
    std::vector<std::string> extensions = {"zonemap", "bloom", "counts",
                                           "postings"};
    std::optional<schemas::dft::agg::AggregationConfig> aggregation;
    /// RecordSchema every trace is decoded with ("dftracer" or "generic");
    /// empty detects each file's from its first lines.
    std::string schema;
    /// Bytes a build may hold at once, as View::memory_budget: 0 is about a
    /// third of available memory, NO_SPILL_BUDGET leaves it unbounded.
    std::uint64_t memory_budget = 0;
    /// Runtime for the blocking calls; nullptr uses default_runtime(). Not
    /// owned; it must outlive the Indexer.
    Runtime* runtime = nullptr;
};

struct IndexStatus {
    std::size_t total = 0;
    std::vector<std::string> ready;
    std::vector<std::string> needs_work;
    /// Ready files whose last gzip member was cut short when indexed; their
    /// recovered tail is not checksum-verified.
    std::vector<std::string> truncated;
    std::string index_path;
    std::uint64_t aggregation_interval_us = 0;
    /// The stored aggregation tier was built with another config than the
    /// requested one, or holds rows of a file that changed, so the next build
    /// clears and rebuilds it; the other extensions are kept.
    bool aggregation_needs_rebuild = false;
    /// Files indexed by this call; always 0 for status().
    std::size_t indexed = 0;
};

struct IndexedFile {
    std::string path;
    std::string index_path;
    std::int64_t file_id = -1;
    /// Size and modification time recorded when the file was indexed.
    std::uint64_t size_bytes = 0;
    std::uint64_t mtime = 0;
    /// The last gzip member was cut short when the file was indexed.
    bool truncated = false;
    /// The schema its records were decoded with.
    std::string schema;
};

/// One manifest entry of a file.
struct ExtensionEntry {
    /// Such as "core.members" or "zonemap".
    std::string name;
    std::uint32_t version = 0;
    std::uint64_t params_hash = 0;
    /// False when the build of the extension failed.
    bool ready = false;
    /// Ready, at the current version, and built with the settings in
    /// IndexerOptions.
    bool current = false;
};

struct FileManifest {
    std::string path;
    std::string index_path;
    /// The extensions built for the file, in manifest order.
    std::vector<ExtensionEntry> extensions;
};

/// What one pruning extension rules out of a file on its own.
struct ExtensionPrune {
    std::string name;
    bool file_ruled_out = false;
    /// Chunks ruled out, ascending.
    std::vector<std::uint64_t> removed;
};

struct FileExplain {
    std::string path;
    /// False when the file is not in the index; nothing is pruned then.
    bool indexed = false;
    bool may_match = true;
    std::uint64_t chunks = 0;
    /// Chunks the query reads, ascending: the chunks a View scan reads.
    std::vector<std::uint64_t> read;
    /// One entry per pruning extension current for the file.
    std::vector<ExtensionPrune> extensions;
};

/// The manifest or explanation as a JSON array of objects whose keys are the
/// field names above; params_hash is a 16-digit hex string, since a JSON
/// number may not hold 64 bits.
std::string to_json(const std::vector<FileManifest>& manifest);
std::string to_json(const std::vector<FileExplain>& explain);

/// Builds and inspects the index of a fixed set of traces.
///
/// Copies share one state. Use an Indexer from one thread at a time; separate
/// Indexers over the same index root are serialized by the index write lock.
/// Failures throw DFTUtilsException: INVALID_ARGUMENT or NOT_FOUND from open,
/// and the engine's I/O or compression error, naming the trace, from build.
class Indexer {
   public:
    /// `paths` holds trace files and directories; a directory contributes the
    /// trace files (.pfw, .jsonl and .ndjson, plain or gzip) directly
    /// inside it. Reads nothing else and writes nothing.
    static Indexer open(std::vector<std::string> paths,
                        IndexerOptions options = {});

    /// Which files are ready for the requested tiers. Writes nothing.
    coro::CoroTask<IndexStatus> status(CoroScope& scope) const;
    /// Builds the requested tiers for files that lack them or changed since
    /// they were indexed; fresh files are left alone.
    coro::CoroTask<IndexStatus> build(CoroScope& scope);
    /// Rebuilds the requested tiers for every file.
    coro::CoroTask<IndexStatus> rebuild(CoroScope& scope);
    /// The indexed files; files not yet indexed are omitted.
    coro::CoroTask<std::vector<IndexedFile>> files(CoroScope& scope) const;
    /// The manifest of every indexed file. Writes nothing.
    coro::CoroTask<std::vector<FileManifest>> manifest(CoroScope& scope) const;
    /// Per file, the chunks `query` reads and what each pruning extension
    /// rules out alone. Writes nothing. Throws INVALID_ARGUMENT when the
    /// query does not parse.
    coro::CoroTask<std::vector<FileExplain>> explain(CoroScope& scope,
                                                     std::string query) const;
    /// Rewrites one tier extension ("zonemap", "bloom", "counts", "postings",
    /// "dft.stats", "dft.metadata" or a registered plugin extension's name)
    /// of every file, and builds files not yet indexed. Other extensions are
    /// left as they are. Throws INVALID_ARGUMENT for another name.
    coro::CoroTask<IndexStatus> rebuild_extension(CoroScope& scope,
                                                  std::string extension);
    /// Removes one tier extension's data and manifest entries from every
    /// indexed file; the next build makes it again only when
    /// IndexerOptions::extensions names it, or for a plugin extension while
    /// it is registered. Throws INVALID_ARGUMENT as rebuild_extension does.
    coro::CoroTask<IndexStatus> drop_extension(CoroScope& scope,
                                               std::string extension);

    /// Blocking forms, run on IndexerOptions::runtime.
    IndexStatus status() const;
    IndexStatus build();
    IndexStatus rebuild();
    std::vector<IndexedFile> files() const;
    std::vector<FileManifest> manifest() const;
    std::vector<FileExplain> explain(std::string query) const;
    IndexStatus rebuild_extension(std::string extension);
    IndexStatus drop_extension(std::string extension);
    /// The rows the build stored for row set `name` of the files' source,
    /// every indexed file's in open order. Throws DFTUtilsException
    /// INVALID_ARGUMENT when a file's index holds no rows for it (a row set
    /// the build does not evaluate, or an index built before it); read such
    /// a row set with View::duql("from <name>").
    dataframe::DataFrame rowset(std::string name) const;

    /// The expanded trace list, in open order.
    const std::vector<std::string>& paths() const;
    const IndexerOptions& options() const;

   private:
    struct Impl;
    explicit Indexer(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_INDEXER_H
