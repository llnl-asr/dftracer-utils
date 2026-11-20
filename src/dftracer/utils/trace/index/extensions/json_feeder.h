// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_JSON_FEEDER_H
#define DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_JSON_FEEDER_H

#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/schemas/dft/bloom_core.h>
#include <dftracer/utils/trace/views/fold.h>

#include <cstdint>

namespace dftracer::utils::trace::index::extensions {

/// The feeder for dftracer JSON lines. It owns the format: it reads the events
/// of a scan's @ref trace::views::detail::FoldBatch, which the scan parsed
/// once, and hands the values of each path to the summaries as typed values,
/// without building a frame. Paths are the dimensions of a chunk (the fixed
/// ones, the named extra ones and the auto-indexed args keys), and a chunk is a
/// gzip checkpoint.
class JsonFeeder {
   public:
    using BloomCore = trace::index::schemas::dft::BloomCore;
    using ChunkState = BloomCore::ChunkState;

    /// One auto-indexed args key within a chunk: its stats, and its string
    /// values (interned ids) while they number at most auto_max_distinct.
    struct AutoField {
        BloomCore::ChunkDimensionStats stats;
        dstd::HashSet<std::uint32_t> values;
        bool overflow = false;
    };
    using AutoChunk = dstd::HashMap<std::uint32_t, AutoField>;

    /// `config` must outlive the feeder. `captures_nested`: the nested extra
    /// dimensions come from a capture under "args.".
    JsonFeeder(dftracer::utils::dstd::StringIntern& intern,
               const BloomCore::ChunkIndexerConfig& config,
               bool captures_nested);

    /// Observes every event of `batch` into the chunk of its checkpoint. A
    /// parse never suspends, so two calls on one thread never overlap.
    void feed(ChunkState& chunk, AutoChunk& auto_chunk,
              const trace::views::detail::FoldBatch& batch) const;

   private:
    void feed_auto(AutoChunk& chunk,
                   const trace::views::detail::FoldEvent& e) const;

    dftracer::utils::dstd::StringIntern* intern_;
    const BloomCore::ChunkIndexerConfig* config_;
    // Interned args key (or captured path) of each extra dimension, in
    // config order.
    dstd::Vector<std::uint32_t> extra_keys_;
    // Args keys the path budget leaves alone: the extra dimensions and the
    // command hash keys the fixed shash dimension already covers.
    dstd::HashSet<std::uint32_t> auto_skip_;
    std::uint32_t dur_key_ = 0;
};

}  // namespace dftracer::utils::trace::index::extensions

#endif  // DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_JSON_FEEDER_H
