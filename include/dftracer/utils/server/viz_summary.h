#ifndef DFTRACER_UTILS_SERVER_VIZ_SUMMARY_H
#define DFTRACER_UTILS_SERVER_VIZ_SUMMARY_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::server {

// Activity summary ("mipmap") for the viz overview: a full parallel scan folds
// every event into per-lane time buckets once, letting density and counter
// queries answer in O(buckets) with no event cap. The buckets form a pyramid of
// levels, each 4x finer than the one above, so a zoomed-in window is served
// from the coarsest level whose buckets still fit inside one output pixel.
// Only zooms below the finest level, and filtered queries, scan live.
struct VizSummary {
    // Level-0 (counter grid) budget: ~60 MB at 20 bytes/cell. Also caps lanes.
    static constexpr std::size_t MAX_CELLS = 3'000'000;
    static constexpr std::size_t MIN_BUCKETS_PER_LANE = 4'096;
    static constexpr std::size_t MAX_BUCKETS_PER_LANE = 65'536;

    // Levels below level 0, each 4x finer. The build coarsens the pyramid
    // rather than exceed MAX_FINE_CELLS.
    static constexpr std::size_t EXTRA_LEVELS = 3;
    static constexpr std::size_t MAX_FINE_CELLS = 16'000'000;

    struct Cell {
        std::uint32_t count = 0;
        std::uint64_t total = 0;  // summed busy time (us)
        std::uint32_t max_dur = 0;
        std::uint32_t name_id = std::numeric_limits<std::uint32_t>::max();
    };

    // Sparse: `buckets` holds the index of each occupied bucket, ascending, and
    // is parallel to `cells`.
    struct Lane {
        std::int64_t pid = 0;
        std::int64_t tid = 0;
        std::vector<std::uint32_t> buckets;
        std::vector<Cell> cells;
    };

    struct Level {
        std::size_t nbuckets = 0;
        double bucket_us = 0;
        std::vector<Lane> lanes;
        // I/O counter tracks at this resolution, size == nbuckets. Dense and
        // lane-independent, so the whole pyramid of them costs a few MB.
        std::vector<double> read_bytes;
        std::vector<double> write_bytes;
        std::vector<double> ops;
    };

    // Whole-trace aggregate for one grouping key (Analyze panel), precomputed
    // in the same scan so the panel needs no live scan. Sorted by total desc.
    struct GroupRow {
        std::string key;
        std::uint64_t count = 0;
        double total = 0;
        double min = 0;
        double max = 0;
    };

    std::uint64_t t_begin = 0;  // absolute us (global min)
    std::uint64_t t_end = 0;    // absolute us (global max)
    std::size_t nbuckets = 0;   // level 0, also the counter grid
    double bucket_us = 0;       // level 0
    std::uint64_t max_dur = 0;  // longest event seen anywhere

    // Coarsest (== level 0, the counter grid) first, finest last.
    std::vector<Level> levels;

    std::vector<std::string> names;  // name_id -> name

    // Precomputed Analyze aggregates, one per GroupBy dimension.
    std::vector<GroupRow> by_name;
    std::vector<GroupRow> by_cat;
    std::vector<GroupRow> by_pid;
    std::vector<GroupRow> by_fhash;

    // Operation name -> category (first seen), so the UI can label each
    // operation with its real layer instead of guessing from the name.
    std::vector<std::pair<std::string, std::string>> name_cats;

    // Every groupable column present in the trace (top-level scalar fields +
    // args keys), harvested once per distinct event name. Sorted.
    std::vector<std::string> columns;

    std::size_t total_files = 0;  // distinct files declared by FH metadata
    std::size_t io_files = 0;     // subset actually read from or written to

    // Per-process totals for the fork hierarchy. Exact (not bucketed), so the
    // proctree answers from here identically to a live scan.
    struct ProcRow {
        std::int64_t pid = 0;
        std::int64_t ppid = -1;      // args.ppid, -1 when absent
        std::uint64_t first_ts = 0;  // native
        std::uint64_t bytes = 0;
        std::uint64_t io_ops = 0;
        double io_busy = 0;          // native
        std::string hhash;
        std::string rank;
    };
    std::vector<ProcRow> procs;

    // fork/clone calls: child is the pid from args.ret, or -1 when the trace
    // does not record it and the parent has to be inferred by time.
    struct ForkEdge {
        std::uint64_t ts = 0;
        std::int64_t pid = 0;
        std::int64_t child = -1;
    };
    std::vector<ForkEdge> forks;

    // HH metadata: hhash -> hostname.
    std::vector<std::pair<std::string, std::string>> hosts;

    struct AppSpan {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        std::int64_t pid = 0;
        std::int64_t tid = 0;
        // Set for long_events so folding one back into a cell needs no parse.
        std::uint32_t name_id = std::numeric_limits<std::uint32_t>::max();
        std::string json;  // absolute ts; normalized per request
    };
    std::vector<AppSpan> app_spans;

    // Events at least `long_threshold_us` (the finest bucket width) wide, kept
    // whole and excluded from every level's cells. A request serves the ones
    // at least one pixel wide as real spans and folds the rest back into
    // density blocks, which is what a live scan of the window would do.
    std::vector<AppSpan> long_events;  // ascending by begin
    double long_threshold_us = 0;
    static constexpr std::size_t MAX_LONG_EVENTS = 1'000'000;
    static constexpr std::size_t MAX_RESPONSE_SPANS = 100'000;

    // Absolute-us spans between runs (no lane active, no process alive). Any
    // present means a multi-run trace the viewer can timelapse-compress.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> idle_gaps;

    std::int64_t bucket_of(double abs_ts) const {
        return bucket_of(abs_ts, bucket_us, nbuckets);
    }

    std::int64_t bucket_of(double abs_ts, double width,
                           std::size_t count) const {
        if (width <= 0) return -1;
        double rel = abs_ts - static_cast<double>(t_begin);
        if (rel < 0) return -1;
        std::int64_t b = static_cast<std::int64_t>(rel / width);
        if (b < 0) return -1;
        if (b >= static_cast<std::int64_t>(count))
            b = static_cast<std::int64_t>(count) - 1;
        return b;
    }

    // A cell straddling a column boundary is split by overlap, which assumes
    // its events are spread evenly; asking for several cells per column keeps
    // what that assumption costs down to a few percent.
    static constexpr double CELLS_PER_PIXEL = 8;

    // Coarsest level fine enough to answer at `threshold` (one output pixel),
    // or null when even the finest is too coarse.
    const Level* level_for(double threshold) const {
        const Level* best = nullptr;
        const Level* fallback = nullptr;
        for (const auto& l : levels) {
            if (l.bucket_us <= 0 || l.bucket_us > threshold) continue;
            if (fallback == nullptr || l.bucket_us > fallback->bucket_us)
                fallback = &l;
            if (l.bucket_us <= threshold / CELLS_PER_PIXEL &&
                (best == nullptr || l.bucket_us > best->bucket_us))
                best = &l;
        }
        return best != nullptr ? best : fallback;
    }
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_SUMMARY_H
