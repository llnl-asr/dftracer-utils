#ifndef DFTRACER_UTILS_DATAFRAME_AGG_H
#define DFTRACER_UTILS_DATAFRAME_AGG_H

#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/sketch.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The aggregation engine: a mergeable partial group state (`AggState`) with
// synchronous accumulate / merge / finalize. This is the shared kernel both the
// View (async coroutine driver over scanned batches) and the
// post-materialization path (sync parallel_reduce over column chunks) delegate
// to - one aggregation logic, two drivers. accumulate touches one state on one
// thread (no locks); parallelism is the driver's job (per-worker or per-chunk
// partials, then merge).
namespace dftracer::utils::dataframe {

struct DataFrame;  // dataframe/dataframe.h; only used by value as a return type

enum class AggOp {
    Count = 0,
    Sum = 1,
    Min = 2,
    Max = 3,
    Mean = 4,
    Var = 5,    ///< sample variance
    Std = 6,    ///< sample standard deviation
    Skew = 7,   ///< population skewness
    Kurt = 8,   ///< excess (population) kurtosis
    First = 9,  ///< first non-null value in row order (order-independent merge)
    Last = 10,  ///< last non-null value in row order
    Pct = 11,   ///< a DDSketch quantile (level in AggSpec::param), mergeable
    Hist = 12,  ///< the DDSketch histogram, a list<struct{lo,hi,count}> column
    ArgMax =
        13,  ///< the String repr of `value_col` at the row maximizing `by_col`
    SumSq = 14,  ///< sum of squares (Float64), from the shared FieldStat
    SetUnion =
        15,      ///< distinct String values of `value_col`, sorted and joined
    // Occupancy (time-window reductions over the ts=`value_col`, dur=`by_col`
    // pair; `param` carries the endpoint-snap tolerance occ_cell_us): a
    // per-group +1/-1 endpoint delta-map, mergeable by key-wise add.
    Busy = 16,         ///< exact interval-union length (us) where depth > 0
    Concurrency = 17,  ///< sum(dur) / busy
    Utilization = 18,  ///< busy / (max_end - min_ts)
    Active = 19,       ///< peak overlap depth
    /// Count of non-null (present) values in `value_col`, an Int64 column.
    /// Unlike Count (the group's row count) this reads the field's
    /// FieldStat::n, so it counts only the rows where the value column was
    /// present.
    CountValid = 20,
    /// The String repr of `value_col` at the row minimizing `by_col`. Specs
    /// sharing a `by_col` resolve to the same winning row.
    ArgMin = 21,
    /// Bitwise OR of `value_col` read as u64, a Uint64 column.
    BitOr = 22,
    /// Approximate distinct count of `value_col` (KMV sketch, `param` = k), an
    /// Int64 column.
    Distinct = 23,
    /// String reprs of `value_col` ordered by `by_col` ascending, a
    /// list<string> column.
    ListSorted = 24,
    /// String reprs of `value_col` at the `param` (k) largest `by_col` values,
    /// a list<string> column in descending `by` order.
    TopK = 25,
    /// String reprs of `value_col` at the `param` (k) smallest `by_col`
    /// values, a list<string> column in ascending `by` order.
    BottomK = 26,
    /// Approximate heavy hitters of `value_col` (SpaceSaving, `param` =
    /// counters), a list<struct{value, count}> column by descending count.
    ApproxTopK = 27,
    /// Deterministic bottom-k-by-hash sample of the distinct reprs of
    /// `value_col` (`param` = k), a sorted list<string> column.
    Sample = 28,
    /// Pearson correlation of (x = `by_col`, y = `value_col`), Float64.
    Corr = 29,
    /// Population covariance of (x = `by_col`, y = `value_col`), Float64.
    CovarPop = 30,
    /// Sample covariance of (x = `by_col`, y = `value_col`), Float64.
    CovarSamp = 31,
    /// Least-squares slope of y = `value_col` on x = `by_col`, Float64.
    RegrSlope = 32,
    /// Least-squares intercept of y = `value_col` on x = `by_col`, Float64.
    RegrIntercept = 33,
    /// Coefficient of determination of y = `value_col` on x = `by_col`,
    /// Float64.
    RegrR2 = 34,
    /// Product of the present values of `value_col` (Float64; 1 for a group
    /// with none, as pandas).
    Prod = 35
};

/// Ops taking a second input through `by_col`: the extremum ArgMax/ArgMin
/// maximize/minimize, the ordered list/top-k ops order by, the co-moment ops
/// read as x, and occupancy reads as dur.
inline bool agg_uses_by_col(AggOp op) {
    return op == AggOp::ArgMax || op == AggOp::ArgMin ||
           op == AggOp::ListSorted || op == AggOp::TopK ||
           op == AggOp::BottomK || op == AggOp::Corr || op == AggOp::CovarPop ||
           op == AggOp::CovarSamp || op == AggOp::RegrSlope ||
           op == AggOp::RegrIntercept || op == AggOp::RegrR2 ||
           op == AggOp::Busy || op == AggOp::Concurrency ||
           op == AggOp::Utilization || op == AggOp::Active;
}

/// Ops reading `value_col` as a String repr rather than as a number: their
/// value must bind a raw input column of any type, so a caller lowering an
/// expression aggregate has to bypass the numeric evaluator for them.
inline bool agg_uses_raw_value(AggOp op) {
    return op == AggOp::ArgMax || op == AggOp::ArgMin ||
           op == AggOp::SetUnion || op == AggOp::Distinct ||
           op == AggOp::ListSorted || op == AggOp::TopK ||
           op == AggOp::BottomK || op == AggOp::ApproxTopK ||
           op == AggOp::Sample;
}

/// The DataType agg_finalize actually writes for `op` over a value column of
/// `value_type`: Sum/Min/Max/First/Last widen through the same
/// integer/unsigned/float domain agg_finalize keys on (Int64/Uint64/Float64),
/// String columns pass First/Last through unchanged, and every other op's
/// type is fixed regardless of `value_type`. Never guesses: every AggOp is
/// covered, so schema code built on this can never disagree with collect().
DataType agg_output_type(AggOp op, TypeId value_type);

/// One aggregate: `op` over the value column at index `value_col` in the values
/// passed to accumulate (ignored for Count), named `out` in the result.
struct AggSpec {
    AggOp op;
    std::int32_t value_col = -1;
    std::string out;
    double param = 0.0;  ///< Pct: quantile q in [0, 1]; occupancy: occ_cell_us;
                         ///< Distinct/TopK/BottomK/ApproxTopK/Sample: k
    std::int32_t by_col = -1;  ///< the second input for the ops
                               ///< agg_uses_by_col() names; unused otherwise
};

/// One reduction applied to every auto-discovered dyn (per-argument) column.
/// The finalized column for argument `name` is `out_prefix + name`;
/// `op`/`param` select the reduction (Pct reads a per-name sketch, every other
/// op the per-name FieldStat). Configured once at agg_new - the argument names
/// themselves are discovered during accumulate, so a single pass can build the
/// per-argument aggregates without a name pre-scan.
struct AggDynSpec {
    AggOp op;
    double param = 0.0;      ///< Pct quantile q in [0, 1]
    std::string out_prefix;  ///< finalized column name is `out_prefix + name`
};

/// One dyn value column fed to accumulate, named by its discovered argument.
/// The column set may differ per batch; a group's dyn set is the union of the
/// names seen for it, and an absent name is simply not accumulated
/// (present-count semantics). `col` is borrowed for the accumulate call only.
struct AggDynInput {
    std::string name;
    const Series* col;
};

class AggState;  // opaque, mergeable partial group state (defined in agg.cpp)

struct AggStateDeleter {
    void operator()(AggState* p) const noexcept;
};
using AggStatePtr = std::unique_ptr<AggState, AggStateDeleter>;

/// A fresh partial for `specs`, grouping by N key columns. `dyn` (optional)
/// enables the name-keyed dyn side-table: each spec in `dyn` is one reduction
/// applied to every argument name fed through agg_accumulate's dyn inputs.
AggStatePtr agg_new(std::vector<AggSpec> specs,
                    std::vector<AggDynSpec> dyn = {});

/// Fold rows [begin, end) of a batch (the N `keys` columns + the value columns
/// the specs reference) into `state`. The composite key is hashed and compared
/// column-by-column (no string concatenation); each key column keeps its own
/// type. `end < 0` means the whole column. Serial; one thread per state - the
/// parallel/async drivers give each chunk its own state.
void agg_accumulate(AggState& state, const std::vector<const Series*>& keys,
                    const std::vector<const Series*>& values,
                    std::int64_t begin = 0, std::int64_t end = -1);
/// As above plus the dyn inputs: each `AggDynInput` feeds a per-argument value
/// column whose name is discovered here. Requires `state` to have been built
/// with a non-empty `dyn` spec list; the column set may differ from batch to
/// batch, and a group's dyn set grows to the union of names seen for it.
void agg_accumulate(AggState& state, const std::vector<const Series*>& keys,
                    const std::vector<const Series*>& values,
                    const std::vector<AggDynInput>& dyn, std::int64_t begin = 0,
                    std::int64_t end = -1);
/// Single-key convenience: forwards to the N-key form with `keys = {&key}`.
void agg_accumulate(AggState& state, const Series& key,
                    const std::vector<const Series*>& values,
                    std::int64_t begin = 0, std::int64_t end = -1);

/// Combine `other` into `into` (associative; for spill + distributed merge).
void agg_merge(AggState& into, const AggState& other);
/// The same, other's groups taken in `order` (indices into its groups).
void agg_merge(AggState& into, const AggState& other,
               const std::vector<std::int64_t>& order);

/// Re-key `src` to a coarser grouping and merge groups that collapse together.
/// `keep` lists src key-column indices in destination order (a subset and/or
/// reorder of src's keys); each src group is re-keyed to just those columns and
/// merged (agg_merge semantics) into the result, so groups sharing the coarse
/// key combine exactly. When `bucket_recut > 0`, the first kept key must be an
/// integer bucket column and is re-floored to (v / bucket_recut) * bucket_recut
/// before regrouping (coarsening a finer time grain to a coarser one). The
/// result shares src's specs and value/field layout; finalize with
/// agg_finalize. Every index in `keep` must be a valid src key column.
AggStatePtr agg_regroup(const AggState& src,
                        const std::vector<std::int32_t>& keep,
                        std::int64_t bucket_recut = 0);

/// Materialize the result: one key column per `key_names` (in order, each
/// keeping its original type) plus one column per spec, in spec order.
DataFrame agg_finalize(const AggState& state,
                       const std::vector<std::string>& key_names);
/// Single-key convenience: forwards to the vector form with `{key_name}`.
DataFrame agg_finalize(const AggState& state, const std::string& key_name);

/// Serialize a partial to a portable byte blob (distributed partials / spill)
/// and reconstruct it; round-trips exactly.
std::string agg_serialize(const AggState& state);
AggStatePtr agg_deserialize(const std::string& blob);

/// Number of groups currently held by `state`.
std::int64_t agg_num_groups(const AggState& state);

/// The specs `state` was built with, in order (key layout excluded). Lets a
/// consumer finalizing a persisted or regrouped state recover each value
/// column's op and output name without re-deriving them.
const std::vector<AggSpec>& agg_specs(const AggState& state);

/// Render group `g`'s composite key to one string per key column (integer keys
/// as decimal, string keys verbatim) - a stable per-group identity, e.g. a
/// rollup row key. `g` must be in [0, agg_num_groups(state)).
std::vector<std::string> agg_group_key(const AggState& state, std::int64_t g);

/// Approximate in-memory bytes held by `state` (spill trigger; not exact).
std::size_t agg_approx_bytes(const AggState& state);

/// Three-way compare of the composite key of group `ga` in `a` against group
/// `gb` in `b`. `a` and `b` must share the same key layout (same group_by
/// keys, e.g. two states built from the same specs).
int agg_key_cmp(const AggState& a, std::int64_t ga, const AggState& b,
                std::int64_t gb);

/// Sort `state`'s groups in place by ascending composite key. Used to write a
/// spill run in the key order a k-way merge needs.
void agg_sort_groups(AggState& state);

/// A fresh state holding only group `g` of `state`, sharing its specs and key
/// layout - serializable via agg_serialize and mergeable via agg_merge into
/// another state. The unit written to and read back from a spill run.
AggStatePtr agg_extract_group(const AggState& state, std::int64_t g);

/// Like group_agg but returns the mergeable partial instead of finalizing: the
/// same one-pass parallel accumulate over `keys`/`values`, stopping before
/// agg_finalize. The caller finalizes (agg_finalize), coarsens (agg_regroup),
/// or persists (agg_extract_group + agg_serialize) the state. Used by the
/// rollup materialize path to store per-group partials.
AggStatePtr group_agg_state(const std::vector<const Series*>& keys,
                            const std::vector<const Series*>& values,
                            std::vector<AggSpec> specs);

/// One value column's already-reduced statistics to seed into an AggState,
/// identified by `value_col` (the AggSpec value_col index the state was built
/// with). `sketch` is set only for a Pct/Hist field, and merged into that
/// field's shared sketch slot.
struct AggSeedValue {
    std::int32_t value_col = -1;
    FieldStat stat;
    const DDSketch* sketch = nullptr;
};

/// Initialize `state`'s key layout for the seed path: `nkeys` String key
/// columns. Call once before agg_seed_group so a state that ends up with zero
/// seeded groups still finalizes with the right key columns.
void agg_seed_begin(AggState& state, std::size_t nkeys);

/// Seed one already-reduced group observation into `state` with no raw events,
/// for a fast path that reads per-group stats (the aggregation tier) instead of
/// events. `state` must have been built by agg_new with the same specs the
/// equivalent scan uses. `str_keys` is the composite key, one string per key
/// column (every seeded key column is a String column). `count` is the group's
/// row count; each `values` entry merges its FieldStat (and sketch) into the
/// value column's per-group slot. Repeated calls for the same composite key
/// merge (FieldStat/sketch merge), exactly like accumulating then merging the
/// equivalent events. Call agg_seed_finalize once, after all agg_seed_group
/// calls and before agg_finalize/agg_serialize.
void agg_seed_group(AggState& state, const std::vector<std::string>& str_keys,
                    std::uint64_t count,
                    const std::vector<AggSeedValue>& values);

/// Fix each value column's finalized integer/float domain from the seeded
/// per-group stats, matching the columnar materializer's per-column rule (a
/// field absent from some groups finalizes as Float64). Marks `state` ready for
/// finalize/serialize/merge. Call once after all agg_seed_group calls.
void agg_seed_finalize(AggState& state);

/// Fused group-by: accumulate the whole batch in one pass over all specs (the
/// sync driver chunks via the parallel_for seam and merges), then finalize.
DataFrame group_agg(const std::vector<const Series*>& keys,
                    const std::vector<const Series*>& values,
                    std::vector<AggSpec> specs,
                    const std::vector<std::string>& key_names);
/// Single-key convenience: forwards to the N-key form.
DataFrame group_agg(const Series& key, const std::vector<const Series*>& values,
                    std::vector<AggSpec> specs, const std::string& key_name);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_AGG_H
