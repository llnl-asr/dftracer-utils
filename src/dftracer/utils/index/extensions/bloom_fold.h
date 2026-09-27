#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_BLOOM_FOLD_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_BLOOM_FOLD_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/schemas/dft/bloom_core.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/trace/views/fold.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::index::extensions {

/// Builds the per-checkpoint pruner index (bloom filters, chunk statistics,
/// dimension stats, columns) from the events a scan already delivers and writes
/// it to each file's index, as a byproduct of the pass the query runs anyway.
///
/// Chunks are held in a sparse map keyed by checkpoint, so fuse's round-robin
/// unit distribution (each worker sees a non-contiguous set) never balloons a
/// dense array; a whole-file read fills every checkpoint before finalize.
class BloomFold : public trace::views::detail::Fold {
   public:
    /// Indexes config.extra_dimensions besides the fixed dimensions: each is
    /// an args key or a dotted path below args.
    explicit BloomFold(
        dftracer::utils::StringIntern& intern,
        index::schemas::dft::BloomCore::ChunkIndexerConfig config = {});
    /// Removes the spill runs unless finish_spilled handed them off.
    ~BloomFold() override;
    BloomFold(const BloomFold&) = delete;
    BloomFold& operator=(const BloomFold&) = delete;

    /// A filtered read would build a pruner index that later reads cannot tell
    /// from a complete one.
    /// Every record, metadata included, so the chunk metadata is whole.
    bool accepts(const trace::views::detail::ScanShape& shape) const override {
        return !shape.filtered && shape.include_metadata;
    }
    bool needs_args() const override { return true; }
    /// The nested extra dimensions, under their args.-prefixed path.
    std::vector<std::string> extra_captures() const override;
    /// Enumerate every scalar leaf (nested included) so the harvested column
    /// set is schemaless: a nested-object arg surfaces as dotted leaf columns.
    bool wants_schema() const override { return true; }

    std::unique_ptr<trace::views::detail::Fold> slice() const override {
        return std::make_unique<BloomFold>(*intern_, config_);
    }

    void step(const trace::views::detail::FoldBatch& batch) override;
    void seal_unit(const trace::views::detail::ScanUnit& /*unit*/) override {}
    void drop_unit(const trace::views::detail::ScanUnit& unit) override;
    void merge(trace::views::detail::Fold& slice) override;
    coro::CoroTask<bool> finalize(
        const trace::views::detail::CoverageSet& covered) override;

    /// Write the single harvested file's `dft.index` data and manifest entry
    /// into a caller-owned write, for the streaming index build which owns
    /// the transaction. Assumes one file; no coverage gate. Consumes the
    /// chunks.
    void write(index::store::IndexWrite& w, int file_id);

    /// Total data events harvested across this fold's files. Call before
    /// write, which consumes the chunks.
    std::uint64_t total_events() const {
        std::uint64_t total = 0;
        for (const auto& [file, fs] : files_) {
            total += fs.spill.events;
            for (const auto& [cp, chunk] : fs.chunks)
                total += chunk.statistics.total_events;
        }
        return total;
    }

    /// Bounds the streaming build of one file whose chunks arrive in order:
    /// once the finished chunks held pass `share` bytes, they are written to
    /// sorted runs under `dir` (created as needed) for `file_id`.
    void enable_spill(std::uint64_t share, std::string dir, int file_id);
    /// Runs written so far; 0 means the file is still wholly in memory.
    std::size_t spill_runs() const;
    /// For a file with runs: writes the rest of its tier and its manifest
    /// entries to one more run and returns every run in ingest order. They
    /// commit only as one ingest in that order, since the last run's range
    /// deletes drop the paths the path budget did not keep. The caller owns
    /// the files from here on.
    std::vector<index::store::IndexDatabaseSstWriterContext::Artifacts>
    finish_spilled(int file_id);

    /// Harvested per-checkpoint chunks for a file (nullptr if unseen), for
    /// tests. Moved-from once finalize or write has run.
    const std::map<std::uint64_t, index::schemas::dft::BloomCore::ChunkState>*
    file_chunks(const std::string& file) const {
        auto it = files_.find(file);
        return it == files_.end() ? nullptr : &it->second.chunks;
    }

   private:
    using ChunkState = index::schemas::dft::BloomCore::ChunkState;
    using Catalog = index::schemas::dft::BloomCore::Catalog;

    // One auto-indexed args key within a chunk: its stats, and its string
    // values (interned ids) while they number at most auto_max_distinct.
    struct AutoField {
        AutoField() { stats.value_type.clear(); }
        index::schemas::dft::BloomCore::ChunkDimensionStats stats;
        ankerl::unordered_dense::set<std::uint32_t> values;
        bool overflow = false;
    };
    using AutoChunk = ankerl::unordered_dense::map<std::uint32_t, AutoField>;

    using AutoKeys = std::vector<std::pair<std::string, std::uint32_t>>;

    struct SpillState {
        std::optional<std::uint64_t> max_seen;
        /// Chunks below this are finished.
        std::uint64_t final_upto = 0;
        /// Chunks below this are in runs.
        std::uint64_t next = 0;
        /// Estimated bytes of finished chunks still in memory.
        std::uint64_t resident = 0;
        std::uint64_t events = 0;
        std::optional<index::schemas::dft::BloomCore::FileAccumulator> acc;
        /// Each spilled chunk's own auto keys.
        std::vector<std::pair<std::uint64_t, std::vector<std::uint32_t>>> keys;
        ankerl::unordered_dense::set<std::uint32_t> key_union;
        std::vector<index::store::IndexDatabaseSstWriterContext::Artifacts>
            runs;
        bool handed_off = false;
    };

    struct FileState {
        std::string index_path;
        SpillState spill;
        std::map<std::uint64_t, ChunkState> chunks;
        std::map<std::uint64_t, AutoChunk> auto_chunks;
        /// The catalog: data-record leaves by interned exact path.
        ankerl::unordered_dense::map<std::uint32_t, index::store::PathStat>
            paths;
        index::schemas::dft::BloomCore::PidTidCache pidtid;
    };

    dftracer::utils::StringIntern* intern_;
    index::schemas::dft::BloomCore::ChunkIndexerConfig config_;
    // Interned args key (or captured path) of each extra dimension, in
    // config_.extra_dimensions order.
    std::vector<std::uint32_t> extra_keys_;
    // Args keys the path budget leaves alone: the extra dimensions and the
    // command hash keys the fixed shash dimension already covers.
    ankerl::unordered_dense::set<std::uint32_t> auto_skip_;
    std::uint32_t dur_key_ = 0;

    /// Writes the file's tier and manifest entries into `w`, one chunk at a
    /// time. Consumes `fs`.
    void write_file(index::store::IndexWrite& w, int file_id, FileState& fs);
    static void observe_path(index::store::PathStat& stat, std::uint8_t tag);
    // Nested extra dimensions come from a capture under "args."; records whose
    // every leaf is already a key need none.
    bool captures_nested() const { return !config_.auto_prefix.empty(); }
    /// Keeps the path_budget auto keys with the most non-null records, ties
    /// by name.
    void select_auto_keys(const FileState& fs,
                          std::map<std::string, std::uint32_t>& keys) const;
    Catalog catalog(const FileState& fs) const;
    void observe_auto(AutoChunk& chunk,
                      const trace::views::detail::FoldEvent& e);
    ChunkState assemble_chunk(FileState& fs, std::uint64_t cp,
                              const std::vector<std::string>& dims,
                              const AutoKeys& keys);
    AutoKeys auto_keys(const FileState& fs, bool select) const;
    std::vector<std::string> dims_of(const AutoKeys& keys) const;
    std::uint64_t resident_bytes(const FileState& fs, std::uint64_t cp) const;
    void chunk_started(FileState& fs, std::uint64_t cp);
    void spill(FileState& fs, std::uint64_t upto);

    StringViewMap<FileState> files_;
    std::uint64_t empty_bloom_bytes_ = 0;
    std::uint64_t spill_share_ = 0;
    std::string spill_dir_;
    int spill_file_id_ = -1;
};

}  // namespace dftracer::utils::index::extensions

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_BLOOM_FOLD_H
