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
// every event into per-lane time buckets once, letting zoomed-out density and
// counter queries answer in O(buckets) with no event cap. Deep-zoom and
// filtered queries scan live instead. Bucket resolution adapts to lane count so
// total cells stay under MAX_CELLS; lanes past the budget are dropped.
struct VizSummary {
    // ~60 MB ceiling at 20 bytes/cell, independent of trace size.
    static constexpr std::size_t MAX_CELLS = 3'000'000;
    static constexpr std::size_t MIN_BUCKETS_PER_LANE = 4'096;
    static constexpr std::size_t MAX_BUCKETS_PER_LANE = 65'536;

    struct Cell {
        std::uint32_t count = 0;
        std::uint64_t total = 0;  // summed busy time (us)
        std::uint32_t max_dur = 0;
        std::uint32_t name_id = std::numeric_limits<std::uint32_t>::max();
    };

    struct Lane {
        std::int64_t pid = 0;
        std::int64_t tid = 0;
        std::vector<Cell> cells;  // size == nbuckets
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

    std::uint64_t t_begin = 0;        // absolute us (global min)
    std::uint64_t t_end = 0;          // absolute us (global max)
    std::size_t nbuckets = 0;
    double bucket_us = 0;
    std::uint64_t max_dur = 0;        // longest event seen anywhere

    std::vector<Lane> lanes;
    std::vector<double> read_bytes;   // counters, size == nbuckets
    std::vector<double> write_bytes;  // size == nbuckets
    std::vector<double> ops;          // size == nbuckets
    std::vector<std::string> names;   // name_id -> name

    // Precomputed Analyze aggregates, one per GroupBy dimension.
    std::vector<GroupRow> by_name;
    std::vector<GroupRow> by_cat;
    std::vector<GroupRow> by_pid;
    std::vector<GroupRow> by_fhash;

    // Operation name -> category (first seen), so the UI can label each
    // operation with its real layer instead of guessing from the name.
    std::vector<std::pair<std::string, std::string>> name_cats;

    std::size_t total_files = 0;  // distinct files declared by FH metadata
    std::size_t io_files = 0;     // subset actually read from or written to

    std::int64_t bucket_of(double abs_ts) const {
        if (bucket_us <= 0) return -1;
        double rel = abs_ts - static_cast<double>(t_begin);
        if (rel < 0) return -1;
        std::int64_t b = static_cast<std::int64_t>(rel / bucket_us);
        if (b < 0) return -1;
        if (b >= static_cast<std::int64_t>(nbuckets))
            b = static_cast<std::int64_t>(nbuckets) - 1;
        return b;
    }
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_SUMMARY_H
