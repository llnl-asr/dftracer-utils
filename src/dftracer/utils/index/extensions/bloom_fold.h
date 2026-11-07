// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_BLOOM_FOLD_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_BLOOM_FOLD_H

#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/bloom_core.h>
#include <dftracer/utils/trace/views/fold.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::index {
class Snapshot;
class FileRef;
class FileBuild;
}  // namespace dftracer::utils::index

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
        dftracer::utils::dstd::StringIntern& intern,
        index::schemas::dft::BloomCore::ChunkIndexerConfig config = {});
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
    dstd::Vector<dstd::String> extra_captures() const override;
    /// Enumerate every scalar leaf (nested included) so the harvested column
    /// set is schemaless: a nested-object arg surfaces as dotted leaf columns.
    bool wants_schema() const override { return true; }
    bool reads_never_indexed() const override { return false; }

    dstd::UniquePtr<trace::views::detail::Fold> slice() const override {
        return dstd::make_unique<BloomFold>(*intern_, config_);
    }

    void step(const trace::views::detail::FoldBatch& batch) override;

    /// step() in two parts, so batches of different checkpoints can be
    /// observed at once. open() runs on one thread in checkpoint order and may
    /// spill the chunks SPILL_LAG or more checkpoints before the batch's,
    /// which must be observed by then. The observe() calls of one checkpoint
    /// run in order, each after its open().
    static constexpr std::uint64_t SPILL_LAG = 16;
    class Lane;
    Lane open(const trace::views::detail::FoldBatch& batch);
    void observe(const Lane& lane,
                 const trace::views::detail::FoldBatch& batch);
    bool spills() const { return spill_share_ != 0; }
    /// Whether a run waits for write_run, which puts it into the build and
    /// stages it.
    bool spill_pending() const { return spill_pending_; }
    void seal_unit(const trace::views::detail::ScanUnit& /*unit*/) override {}
    void drop_unit(const trace::views::detail::ScanUnit& unit) override;
    void merge(trace::views::detail::Fold& slice) override;
    coro::CoroTask<bool> finalize(
        const trace::views::detail::CoverageSet& covered) override;

    /// Writes the single harvested file's tier into `build`, which gets each
    /// tier extension finished. Assumes one file; no coverage gate. Consumes
    /// the chunks.
    void write(index::FileBuild& build);

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

    /// Past `share` bytes the finished chunks of a file are written as a run
    /// into `build`: write_run puts the run there and stages it, so its memory
    /// goes before the file ends and one commit publishes the whole tier. The
    /// final write() must be given the same `build`.
    void enable_spill(std::uint64_t share, index::FileBuild& build);
    /// Writes the run spill() queued into the build and stages it, waiting for
    /// the task that writes the build now.
    coro::CoroTask<void> write_run();
    /// Caps the auto keys. Unset, the size is read from the path at write.
    void set_file_bytes(std::uint64_t bytes) { file_bytes_ = bytes; }
    /// This fold adds chunks from `next_chunk` on to `file` in `snapshot`,
    /// which both stay alive until write(). File-wide records continue from
    /// the stored ones, and the auto dimensions stay as the file has them,
    /// because old chunks hold no records for a new one.
    void continue_file(const index::Snapshot* snapshot,
                       const index::FileRef* file, std::uint64_t next_chunk);
    /// Bytes the open run holds before it is committed. It never falls under
    /// MIN_SPILL_RUN_BYTES and doubles every SPILL_RUNS_PER_DOUBLING runs, so a
    /// tiny budget writes few large runs.
    static std::uint64_t run_limit(std::uint64_t share,
                                   std::size_t runs) noexcept;
    /// Runs staged so far; 0 means the file is still wholly in memory.
    std::size_t spill_runs() const;

    /// Empty if the file is unseen.
    index::schemas::dft::BloomCore::Catalog file_catalog(
        const dstd::String& file);
    /// One per distinct (leaf, tag) pair a flush met, not one per event.
    std::uint64_t catalog_observations() const { return catalog_observations_; }

    /// Harvested per-checkpoint chunks for a file (nullptr if unseen), for
    /// tests. Moved-from once finalize or write has run.
    const dstd::Map<std::uint64_t, index::schemas::dft::BloomCore::ChunkState>*
    file_chunks(const dstd::String& file) const {
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
        index::schemas::dft::BloomCore::NumericBounds bounds;
        dstd::HashSet<std::uint32_t> values;
        bool overflow = false;
    };
    using AutoChunk = dstd::HashMap<std::uint32_t, AutoField>;

   public:
    class Lane {
        friend class BloomFold;
        ChunkState* chunk_ = nullptr;
        AutoChunk* auto_chunk_ = nullptr;
    };

   private:
    // A kept auto key; an absence-only one keeps only its absence entries.
    struct AutoKey {
        dstd::String name;
        std::uint32_t id = 0;
        bool absence_only = false;
    };
    using AutoKeys = dstd::Vector<AutoKey>;

    // One auto key's evidence over the file's finished chunks where it has a
    // value: what it costs and whether it can rule a chunk out.
    struct PathEvidence {
        // The bounds of the first chunk summarized.
        dstd::String min;
        dstd::String max;
        std::uint64_t present_chunks = 0;
        std::uint64_t zone_chunks = 0;
        std::uint64_t bloom_chunks = 0;
        // Zone bounds and chunk bloom bits.
        std::uint64_t payload_bytes = 0;
        // Values over the chunks with a bloom.
        std::uint64_t distinct = 0;
        // Every chunk has the same bounds, so no range or equality test
        // keeps one of them and skips another.
        bool same_bounds = true;
    };

    const index::Snapshot* continue_snapshot_ = nullptr;
    const index::FileRef* continue_file_ = nullptr;
    std::uint64_t continue_next_ = 0;

    struct SpillState {
        dstd::Optional<std::uint64_t> max_seen;
        /// Chunks below this are finished.
        std::uint64_t final_upto = 0;
        /// Chunks below this are in runs.
        std::uint64_t next = 0;
        /// Estimated bytes of finished chunks still in memory.
        std::uint64_t resident = 0;
        std::uint64_t events = 0;
        dstd::Optional<index::schemas::dft::BloomCore::FileAccumulator> acc;
        struct SpilledChunk {
            std::uint64_t checkpoint = 0;
            std::uint64_t events = 0;
            dstd::Vector<std::uint32_t> keys;
        };
        /// Each spilled chunk's data events and own auto keys.
        dstd::Vector<SpilledChunk> keys;
        /// A chunk finished since the last run, with the dimensions it was
        /// assembled for.
        struct PendingChunk {
            std::uint64_t checkpoint = 0;
            dstd::Vector<dstd::String> dims;
            index::schemas::dft::BloomCore::ChunkState chunk;
        };
        dstd::Vector<PendingChunk> pending;
        dstd::HashSet<std::uint32_t> key_union;
        std::size_t runs = 0;
        std::uint64_t run_bytes = 0;
    };

    using SchemaLeaf = dstd::Pair<std::uint32_t, std::uint8_t>;
    struct Shape {
        dstd::Vector<SchemaLeaf> leaves;
        std::uint64_t events = 0;
    };

    struct FileState {
        dstd::String index_path;
        dstd::Vector<Shape> shapes;
        dstd::HashMap<std::uint64_t, std::uint32_t> shape_ids;
        std::uint32_t last_shape = 0;
        SpillState spill;
        dstd::Map<std::uint64_t, ChunkState> chunks;
        dstd::Map<std::uint64_t, AutoChunk> auto_chunks;
        /// The catalog: data-record leaves by interned exact path.
        dstd::HashMap<std::uint32_t, index::store::PathStat> paths;
        /// Auto keys of the chunks summarized so far.
        dstd::HashMap<std::uint32_t, PathEvidence> evidence;
    };

    dftracer::utils::dstd::StringIntern* intern_;
    index::schemas::dft::BloomCore::ChunkIndexerConfig config_;
    // Interned args key (or captured path) of each extra dimension, in
    // config_.extra_dimensions order.
    dstd::Vector<std::uint32_t> extra_keys_;
    // Args keys the path budget leaves alone: the extra dimensions and the
    // command hash keys the fixed shash dimension already covers.
    dstd::HashSet<std::uint32_t> auto_skip_;
    std::uint32_t dur_key_ = 0;
    dstd::Optional<std::uint64_t> file_bytes_;

    /// Writes the tier of `file` into `build`, one chunk at a time. Consumes
    /// `fs`.
    void write_file(index::FileBuild& build, dstd::CStringView file,
                    FileState& fs);
    /// Writes the chunks spill() queued into `build` and frees them.
    void write_pending(index::FileBuild& build, FileState& fs);
    // Nested extra dimensions come from a capture under "args."; records whose
    // every leaf is already a key need none.
    bool captures_nested() const { return !config_.auto_prefix.empty(); }
    void summarize(PathEvidence& ev, const AutoField& f) const;
    /// The auto keys with the most non-null records, ties by name, while
    /// their estimated evidence over the `data_chunks` chunks holding data
    /// records fits the cap of `file` and their count fits path_budget; keys
    /// whose evidence cannot rule out a chunk keep only absence entries, or
    /// nothing.
    AutoKeys select_auto_keys(
        dstd::CStringView file, const FileState& fs, std::uint64_t data_chunks,
        const dstd::Map<dstd::String, std::uint32_t>& keys) const;
    Catalog catalog(const FileState& fs) const;
    void observe_shape(FileState& fs, dstd::Span<const SchemaLeaf> leaves);
    void flush_shapes(FileState& fs);
    void observe_auto(AutoChunk& chunk,
                      const trace::views::detail::FoldEvent& e);
    ChunkState assemble_chunk(FileState& fs, std::uint64_t cp,
                              const dstd::Vector<dstd::String>& dims,
                              const AutoKeys& keys);
    AutoKeys auto_keys(dstd::CStringView file, FileState& fs);
    dstd::Vector<dstd::String> dims_of(const AutoKeys& keys) const;
    std::uint64_t resident_bytes(const FileState& fs, std::uint64_t cp) const;
    void chunk_started(FileState& fs, std::uint64_t cp);
    void spill(FileState& fs, std::uint64_t upto);

    dstd::StringViewMap<FileState> files_;
    dstd::String last_file_;
    FileState* last_state_ = nullptr;
    std::uint64_t catalog_observations_ = 0;
    std::uint64_t spill_share_ = 0;
    index::FileBuild* build_ = nullptr;
    bool spill_pending_ = false;
};

}  // namespace dftracer::utils::index::extensions

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_BLOOM_FOLD_H
