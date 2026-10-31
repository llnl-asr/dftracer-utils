#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/quantile_store.h>
#include <dftracer/utils/utilities/common/statistics/distinct_sketch.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::index::schemas::dft::agg {

/// What the events of a partial are: events, profiles (counters of a category
/// other than sys) and the pre-aggregated sys counters. A name used as more
/// than one has one partial of each.
enum class PartialKind : std::uint8_t { Event = 0, Profile = 1, System = 2 };

/// A set of ids kept exactly up to this many ids.
inline constexpr std::size_t DISTINCT_EXACT_CAPACITY = 256;

/// The distinct count of one id kind (pid, tid, file). Up to
/// DISTINCT_EXACT_CAPACITY ids it is a sorted list and the count is exact; the
/// id after that turns it into a sparse DistinctSketch and the count is an
/// estimate from then on (is_exact() says which). Merging two lists stays a
/// list while the union fits. Ids enter the sketch through splitmix64, so
/// small ids spread over the registers.
///
/// Layout: a union of the list (24 B) and the sketch pointer (8 B) with a
/// tag, 32 B in all. An empty set holds no heap bytes; the list grows
/// geometrically (at most 9 allocations up to 256 ids) and the sketch is one
/// allocation plus its own storage.
class DistinctIds {
   public:
    using Sketch = utilities::common::statistics::DistinctSketch;

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

    /// Appends the set: a tag, then delta varint ids (list), delta varint
    /// sorted hashes (sparse sketch) or the registers (dense sketch).
    void serialize_into(dstd::Vector<std::uint8_t>& buf) const;
    bool read_from(LeReader& r);

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

/// The id of a (pid, tid) pair in a DistinctIds set.
inline std::uint64_t pid_tid_id(std::uint64_t pid, std::uint64_t tid) noexcept {
    return (pid << 32) | (tid & 0xFFFFFFFFu);
}

/// The id of a (pid, file) pair in a DistinctIds set.
inline std::uint64_t pid_file_id(std::uint64_t pid,
                                 std::uint64_t file) noexcept {
    return file ^ (pid * hash::GOLDEN_RATIO);
}

/// First and last time one pid was seen in the partial: the smallest ts and
/// the largest ts + duration of its events.
struct PidSpan {
    std::uint64_t pid = 0;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
};

/// The stats of one numeric arg, keyed by the arg name's per-root id.
struct ArgStat {
    dataframe::FieldStat stat;
    QuantileStore quantiles;
    /// Values added one event at a time. The quantile store holds exactly
    /// these when it is filled; a pre-aggregated counter adds to `stat` only.
    std::uint64_t values = 0;
    std::uint32_t id = 0;

    explicit ArgStat(std::uint32_t arg_id = 0,
                     double accuracy = QuantileStore::DEFAULT_ACCURACY)
        : quantiles(accuracy), id(arg_id) {}
};

/// Everything the index keeps of the events of one (chunk, category, name),
/// as a value that merges in place. The category and name are the key of the
/// caller and are not stored. Merging the partials of the chunks of a file
/// gives the metrics the per-event aggregator gives for that file: counts,
/// sums, minima, maxima and times are exact, the central moments agree to
/// rounding, and the distinct ids are exact up to DISTINCT_EXACT_CAPACITY.
///
/// A metric is absent while its FieldStat has n == 0 (size and offset are
/// absent for most names); the serialized form omits it. The quantile stores
/// are filled only when the caller asks for percentiles.
///
/// Not held: boundary associations and parent pid, which the trace writer
/// fills from the association tracker at output time.
struct AggPartial {
    std::uint64_t count = 0;
    std::uint64_t ts = UINT64_MAX;  ///< smallest ts; UINT64_MAX when empty
    std::uint64_t te = 0;           ///< largest ts + duration
    /// Largest ts of an event, counters included. Every event of the partial
    /// starts in [ts, ts_last_start].
    std::uint64_t ts_last_start = 0;

    dataframe::FieldStat duration;
    dataframe::FieldStat size;
    dataframe::FieldStat offset;
    QuantileStore duration_q;
    QuantileStore size_q;
    QuantileStore offset_q;
    /// Values added one event at a time to duration, size and offset, which
    /// the quantile stores hold when they are filled. A pre-aggregated
    /// counter adds to the stats and to none of these.
    std::uint64_t duration_values = 0;
    std::uint64_t size_values = 0;
    std::uint64_t offset_values = 0;

    DistinctIds pids;
    DistinctIds tids;
    DistinctIds files;
    /// Distinct (pid, tid) and (pid, file) pairs, by pid_tid_id and
    /// pid_file_id. They count threads and per-process file accesses, which
    /// the sets above cannot: a thread id repeats across processes.
    DistinctIds pid_tids;
    DistinctIds pid_files;

    dstd::Vector<PidSpan> pid_spans;  ///< sorted by pid
    dstd::Vector<ArgStat> args;       ///< sorted by id
    double accuracy;

    explicit AggPartial(
        double relative_accuracy = QuantileStore::DEFAULT_ACCURACY);

    /// Back to the state of a new partial, keeping the capacity of the pid
    /// spans and the args.
    void clear();

    /// Counts the event under pid/tid/file (file_id 0 means no file).
    void observe(std::uint64_t pid, std::uint64_t tid, std::uint64_t file_id);

    /// Widens ts, te and the span of `pid` to [ts, end].
    void add_span(std::uint64_t pid, std::uint64_t ts_in, std::uint64_t end);

    /// The stats of arg `id`, inserted in sorted place when absent.
    ArgStat& arg(std::uint32_t id);

    /// Merges `other` in place; `other` must not be `*this`. Both must use
    /// one quantile accuracy.
    void merge(const AggPartial& other);

    /// True when a distinct count is an estimate.
    bool has_estimate() const noexcept {
        return !pids.is_exact() || !tids.is_exact() || !files.is_exact() ||
               !pid_tids.is_exact() || !pid_files.is_exact();
    }

    /// Appends the partial to `buf`. Integers are varints (ids and pid spans
    /// delta coded), doubles are IEEE bit patterns, the FieldStat keeps its
    /// domain and exact sums with their carry.
    void serialize_into(dstd::Vector<std::uint8_t>& buf) const;

    /// What deserialize keeps of the stored partial. A part left out is
    /// skipped in the bytes, not built, and reads as absent.
    struct Fields {
        bool quantiles = true;  ///< the duration, size and offset stores
        bool args = true;       ///< the per-arg stats
    };

    /// Parses exactly `bytes`; a truncated, malformed or over-long input is
    /// ErrorCode::Parse.
    static dstd::Expected<AggPartial, ErrorCode> deserialize(
        dstd::Span<const std::uint8_t> bytes,
        double relative_accuracy = QuantileStore::DEFAULT_ACCURACY);
    static dstd::Expected<AggPartial, ErrorCode> deserialize(
        dstd::Span<const std::uint8_t> bytes, double relative_accuracy,
        Fields keep);
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_PARTIAL_H
