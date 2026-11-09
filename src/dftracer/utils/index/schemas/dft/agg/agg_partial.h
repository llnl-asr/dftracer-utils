// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/distinct_sketch.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/quantile_store.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::schemas::dft::agg {

/// What the events of a partial are. A name used as more than one kind has one
/// partial of each. Profile is a counter of a category other than sys.
enum class PartialKind : std::uint8_t {
    Event = 0,
    Profile = 1,
    System = 2,
    Aggregated = 3
};

inline constexpr std::size_t DISTINCT_EXACT_CAPACITY = 256;

/// The distinct count of one id kind (pid, tid, file). Up to
/// DISTINCT_EXACT_CAPACITY ids the count is exact. Past it the set becomes a
/// DistinctSketch and the count is an estimate (is_exact() says which).
class DistinctIds {
   public:
    using Sketch = dataframe::DistinctSketch;

    DistinctIds();
    DistinctIds(DistinctIds&& other) noexcept;
    DistinctIds& operator=(DistinctIds&& other) noexcept;
    DistinctIds(const DistinctIds&) = delete;
    DistinctIds& operator=(const DistinctIds&) = delete;
    ~DistinctIds();

    void insert(std::uint64_t id);

    /// Merges `other` in place; `other` must not be `*this`.
    void merge(const DistinctIds& other);

    bool is_exact() const noexcept { return !sketch_mode_; }
    bool empty() const noexcept { return !sketch_mode_ && list_.empty(); }

    /// The number of distinct ids, exact while is_exact().
    std::uint64_t count() const;

    /// The sorted ids while is_exact(); empty once it is a sketch.
    dstd::Span<const std::uint64_t> ids() const noexcept {
        return sketch_mode_ ? dstd::Span<const std::uint64_t>{}
                            : dstd::Span<const std::uint64_t>(list_);
    }

    void serialize_into(dstd::Vector<std::uint8_t>& buf) const;
    bool read_from(dstd::LeReader& r);
    /// Reads past one serialized set and keeps nothing.
    static bool skip(dstd::LeReader& r);

   private:
    using List = dstd::Vector<std::uint64_t>;
    using SketchPtr = dstd::UniquePtr<Sketch>;

    void to_sketch();
    void destroy() noexcept;

    union {
        List list_;
        SketchPtr sketch_;
    };
    bool sketch_mode_ = false;
};

inline std::uint64_t pid_tid_id(std::uint64_t pid, std::uint64_t tid) noexcept {
    return (pid << 32) | (tid & 0xFFFFFFFFu);
}

inline std::uint64_t pid_file_id(std::uint64_t pid,
                                 std::uint64_t file) noexcept {
    return file ^ (pid * dstd::hash::GOLDEN_RATIO);
}

/// The smallest ts and the largest ts + duration of the events of one pid.
struct PidSpan {
    std::uint64_t pid = 0;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
};

/// The sums of the events of one thread that start in one time bin. Bin `bin`
/// spans [bin * interval, (bin + 1) * interval) of the bucketed time (see
/// compute_time_bucket).
struct TimeBin {
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::uint64_t bin = 0;
    std::uint64_t count = 0;
    std::uint64_t duration_n = 0;
    std::uint64_t duration_sum = 0;
    std::uint64_t size_n = 0;
    std::uint64_t size_sum = 0;
};

/// What the index keeps of the events of one (chunk, category, name), as a
/// value that merges in place. The key is the caller's and is not stored. A
/// merge is exact except central moments (to rounding). A metric is absent
/// while its @ref dftracer::utils::dataframe::FieldStat "FieldStat" has n == 0.
/// Quantile stores fill only when percentiles are asked.
struct AggPartial {
    std::uint64_t count = 0;
    std::uint64_t ts = UINT64_MAX;  ///< smallest ts; UINT64_MAX when empty
    std::uint64_t te = 0;           ///< largest ts + duration
    /// Largest ts of an event, counters included.
    std::uint64_t ts_last_start = 0;

    dataframe::FieldStat duration;
    dataframe::FieldStat size;
    dataframe::FieldStat offset;
    QuantileStore duration_q;
    QuantileStore size_q;
    QuantileStore offset_q;
    /// Values added one event at a time to duration, size and offset, which the
    /// quantile stores hold when filled. A pre-aggregated counter adds to none.
    std::uint64_t duration_values = 0;
    std::uint64_t size_values = 0;
    std::uint64_t offset_values = 0;

    DistinctIds pids;
    DistinctIds tids;
    DistinctIds files;
    /// Distinct (pid, tid) and (pid, file) pairs, by pid_tid_id and
    /// pid_file_id. A thread id repeats across processes, so the sets above
    /// cannot count them.
    DistinctIds pid_tids;
    DistinctIds pid_files;

    dstd::Vector<PidSpan> pid_spans;  ///< sorted by pid
    /// Sums per thread and time bin, sorted by (pid, tid, bin) once
    /// normalize_bins ran. Empty when the build had no interval. A merge drops
    /// them.
    dstd::Vector<TimeBin> bins;
    /// Microseconds. 0 when the partial has no bins.
    std::uint64_t bin_interval = 0;
    double accuracy;

    explicit AggPartial(
        double relative_accuracy = QuantileStore::DEFAULT_ACCURACY);

    void clear();

    /// file_id 0 means no file.
    void observe(std::uint64_t pid, std::uint64_t tid, std::uint64_t file_id);

    void add_span(std::uint64_t pid, std::uint64_t ts_in, std::uint64_t end);

    /// Merges `other` in place; `other` must not be `*this`. Both must use
    /// one quantile accuracy. Drops the bins of both.
    void merge(const AggPartial& other);
    void normalize_bins();

    bool has_estimate() const noexcept {
        return !pids.is_exact() || !tids.is_exact() || !files.is_exact() ||
               !pid_tids.is_exact() || !pid_files.is_exact();
    }

    /// Appends the partial to `buf`. The bins must be normalized.
    void serialize_into(dstd::Vector<std::uint8_t>& buf) const;

    /// What deserialize keeps. A part left out is skipped and reads as absent.
    struct Fields {
        bool quantiles = true;  ///< the duration, size and offset stores
        bool bins = true;       ///< the time bins
        bool offset = true;     ///< the offset stat and store
        bool pids = true;
        bool tids = true;
        bool files = true;
        bool pid_tids = true;
        bool pid_files = true;
        bool pid_spans = true;
    };

    /// Parses exactly `bytes`; a truncated, malformed or over-long input is
    /// ErrorCode::Parse.
    static dstd::Result<AggPartial> deserialize(
        dstd::Span<const std::uint8_t> bytes,
        double relative_accuracy = QuantileStore::DEFAULT_ACCURACY);
    static dstd::Result<AggPartial> deserialize(
        dstd::Span<const std::uint8_t> bytes, double relative_accuracy,
        Fields keep);
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_H
