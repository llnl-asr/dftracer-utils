#ifndef DFTRACER_UTILS_DATAFRAME_AGG_DETAIL_H
#define DFTRACER_UTILS_DATAFRAME_AGG_DETAIL_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/hash/hash_combine.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/sketch.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Private definition of the AggState partial group state, shared across the
// agg_*.cpp translation units (the class is opaque in agg.h). col_domain and
// read_bits are referenced by AggState::group_of and are defined in
// agg_state.cpp.
namespace dftracer::utils::dataframe {

FieldStatDomain col_domain(TypeId t);
std::uint64_t read_bits(const Series& c, std::int64_t i, FieldStatDomain d);
bool is_bytes_key_type(TypeId t);

/// Reorder the groups of `st` so group i becomes old group perm[i].
void agg_permute_groups(AggState& st, const std::vector<std::int64_t>& perm);

/// `st` with its groups in first-row order (the order they were first
/// seen in a frame accumulated in parallel); untouched when they already
/// are, or their first rows are not known.
AggStatePtr agg_in_first_seen_order(AggStatePtr st);

/// The packed row a partitioned group-by scatters: its row index over the
/// low word of its key hash, the key words, the value cells, and a null
/// mask when a value column has one; `stride` words in all.
struct AggPacked {
    std::size_t stride = 0;
    std::size_t W = 0;
    std::size_t nf = 0;
    bool nulls = false;
    bool strings = false;  // a string key: the chunked path has no direct table
};

/// Packed rows in fixed pages, so a partition's share grows without a
/// copy; a page holds `per_page` rows.
struct AggPages {
    static constexpr std::size_t PAGE_WORDS = 4096;
    std::size_t stride = 0;
    std::size_t per_page = 0;
    std::size_t count = 0;
    std::vector<std::unique_ptr<std::uint64_t[]>> pages;
    std::uint64_t* cursor = nullptr;  // the next row's slot in the last page
    std::size_t left = 0;             // rows the last page still holds
    void open(std::size_t s) {
        stride = s;
        per_page = PAGE_WORDS / s;
    }
    __attribute__((noinline)) void grow() {
        pages.emplace_back(new std::uint64_t[PAGE_WORDS]);
        cursor = pages.back().get();
        left = per_page;
    }
    inline std::uint64_t* next() {
        if (left == 0) grow();
        std::uint64_t* r = cursor;
        cursor += stride;
        --left;
        ++count;
        return r;
    }
    // Hand over the pages held (a full one, for a consumer) and start
    // empty.
    std::vector<std::unique_ptr<std::uint64_t[]>> release() {
        std::vector<std::unique_ptr<std::uint64_t[]>> out;
        out.swap(pages);
        count = 0;
        cursor = nullptr;
        left = 0;
        return out;
    }
};

/// The packed layout of `st` over these columns, initializing `st`; false
/// when the shape does not pack (a reducer past mean, a null key, a
/// value column that is not 8 bytes wide).
bool agg_pack_shape(AggState& st, const std::vector<const Series*>& keys,
                    const std::vector<const Series*>& values, AggPacked& out);

/// Scatter the rows [begin, end) into `out[p]`, p from the hash's top
/// word scaled to out.size(), calling `full(p)` whenever out[p] holds a
/// page's worth (the caller takes the page there); false when a row does
/// not pack (a string over 16 bytes), leaving `out` partly filled.
bool agg_pack(const AggState& st, const std::vector<const Series*>& keys,
              const std::vector<const Series*>& values, std::int64_t begin,
              std::int64_t end, const AggPacked& shape,
              std::vector<AggPages>& out,
              const std::function<void(std::size_t)>& full);

/// Accumulate packed rows into `st`.
void agg_accumulate_packed(AggState& st, const std::vector<const Series*>& keys,
                           const std::vector<const Series*>& values,
                           const AggPacked& shape, const AggPages& rows);

/// Merge the partials into one state: the groups the widest partial holds
/// merge in parallel over group ranges (each partial's groups mapped to it
/// first), the rest are appended after. Empty partials are skipped; null
/// when there is none.
AggStatePtr agg_merge_many(std::vector<AggStatePtr>& partials);

/// Default sketch width when AggSpec::param does not set one.
inline constexpr std::size_t AGG_DISTINCT_DEFAULT_K = 1024;
inline constexpr std::size_t AGG_LIST_DEFAULT_K = 8;

inline std::size_t agg_param_k(double param, std::size_t fallback) {
    return param > 0.0 ? static_cast<std::size_t>(param) : fallback;
}

/// Which extremum an ArgMax/ArgMin slot tracks over its `by` column.
enum class ArgDir { Min = 0, Max = 1 };

/// A bounded ordered-list slot's readout order: ascending `by` (Sorted,
/// Bottom) or descending `by` (Top), the repr breaking ties either way.
enum class ListKind { Sorted = 0, Top = 1, Bottom = 2 };

using ListItems = std::vector<std::pair<double, std::string>>;

inline bool list_item_less(ListKind kind,
                           const std::pair<double, std::string>& a,
                           const std::pair<double, std::string>& b) {
    if (a.first != b.first)
        return kind == ListKind::Top ? a.first > b.first : a.first < b.first;
    return a.second < b.second;
}

/// Sort into readout order and, when `k > 0`, drop everything past k. Which
/// element is dropped is a pure function of the contents, so a bounded slot
/// merges order-independently.
inline void list_bound(ListItems& v, ListKind kind, std::size_t k) {
    std::sort(v.begin(), v.end(),
              [kind](const std::pair<double, std::string>& a,
                     const std::pair<double, std::string>& b) {
                  return list_item_less(kind, a, b);
              });
    if (k > 0 && v.size() > k) v.resize(k);
}

/// The k smallest element hashes seen, each carrying its repr (KMV / bottom-k
/// min-hash). Shared by Distinct (the estimate) and Sample (the reprs).
using KmvMap = std::map<std::uint64_t, std::string>;

inline void kmv_trim(KmvMap& m, std::size_t k) {
    while (m.size() > k) m.erase(std::prev(m.end()));
}

/// SpaceSaving counters: at most k (value -> count) pairs.
using SpaceSavingMap = std::map<std::string, std::uint64_t>;

/// Offer `count` occurrences of `value` to a SpaceSaving summary: bump an
/// existing counter, take a free one, else evict the smallest counter and
/// inherit its count. std::map orders by value, so the evicted minimum is the
/// lexicographically smallest of the tied minima and the summary stays a pure
/// function of the multiset of offers.
///
/// Metwally, Agrawal, El Abbadi, "Efficient Computation of Frequent and Top-k
/// Elements in Data Streams", ICDT 2005.
inline void space_saving_offer(SpaceSavingMap& m, std::size_t k,
                               const std::string& value, std::uint64_t count) {
    if (k == 0) return;
    auto it = m.find(value);
    if (it != m.end()) {
        it->second += count;
        return;
    }
    if (m.size() < k) {
        m.emplace(value, count);
        return;
    }
    auto min_it = m.begin();
    for (auto i = m.begin(); i != m.end(); ++i)
        if (i->second < min_it->second) min_it = i;
    const std::uint64_t inherited = min_it->second;
    m.erase(min_it);
    m.emplace(value, inherited + count);
}

// Groups fold by distinct value column, not by spec: sum(dur), mean(dur) and
// var(dur) share one FieldStat per group (accumulated once). Count is
// field-independent (group rows).
class AggState {
   public:
    std::vector<AggSpec> specs;
    std::vector<int> spec_field;         // spec -> field index, or -1 (Count)
    std::vector<std::int32_t> field_vc;  // field -> value_col
    std::vector<FieldStatDomain> field_domain;  // field -> accumulation domain
    std::size_t nf = 0;
    bool inited = false;

    // Composite key over N columns: each column keeps its own type (Int64
    // stays Int64, String stays String) - `key_is_bytes[k]` selects which of
    // `ikey_cols[k]` / `skey_cols[k]` holds column k's per-group values.
    // `skey_cols` holds raw bytes regardless of logical type (String/
    // LargeString length-prefixed via string_at, FixedSizeBinary/Decimal128/
    // Decimal256 fixed-width via key_byte_width), so hashing and equality on
    // it are always plain byte comparison. Groups are located by a combined
    // hash of the N cells (hash_combine, no string concatenation) with a
    // column-by-column equality check on collision.
    std::size_t nkeys = 0;
    std::vector<char> key_is_bytes;  // nkeys
    // Domain the raw bits in ikey_cols reinterpret to on finalize/render, so a
    // Uint64 hash key > 2^63 or a Float64 key keeps its real type.
    std::vector<FieldStatDomain> key_domain;  // nkeys
    // The key column's exact TypeId (Date32/64, Time32/64, Timestamp,
    // Duration all widen to the I64 domain above); finalize retags the group
    // key with this instead of a bare Int64.
    std::vector<TypeId> key_type;  // nkeys
    // Time32/Time64/Timestamp/Duration key_type only: the unit (and, for
    // Timestamp, the timezone) the raw I64 bits in ikey_cols are expressed
    // in, so finalize retags the group key with the source's own unit
    // instead of defaulting to Micro/naive.
    std::vector<TimeUnit> key_time_unit;    // nkeys
    std::vector<std::string> key_timezone;  // nkeys
    // FixedSizeBinary/Decimal128/Decimal256 key_type only: the per-row byte
    // width (FixedSizeBinary's own fixed_size, or 16/32 for Decimal128/256)
    // read_bits-of-the-bytes-domain slices skey_cols cells at, and finalize
    // retags the group key with. 0 (unused) for String/LargeString, whose
    // cells are already length-delimited.
    std::vector<std::int32_t> key_byte_width;  // nkeys
    // Decimal128/Decimal256 key_type only: the source column's precision and
    // scale, so finalize retags the group key with the exact decimal type
    // instead of a bare Decimal128(0, 0).
    std::vector<std::int32_t> key_decimal_precision;   // nkeys
    std::vector<std::int32_t> key_decimal_scale;       // nkeys
    std::vector<std::vector<std::int64_t>> ikey_cols;  // nkeys * ngroups
    std::vector<std::vector<std::string>> skey_cols;   // nkeys * ngroups
    // 1 where column k's key is null for the group: a null key is its own
    // group, distinct from 0 / "" (the value cell then holds nothing).
    std::vector<std::vector<std::uint8_t>> nkey_cols;  // nkeys * ngroups
    // The key hash of a row to its first group, one flat probe per row; a
    // group with the same hash as an earlier one (a true collision) chains
    // from it through `next_in_bucket`, so no bucket owns a vector.
    ankerl::unordered_dense::map<std::uint64_t, std::int64_t> key_buckets;
    std::vector<std::int64_t> next_in_bucket;  // per group, -1 ends a chain
    // Up to three 8-byte keys over dense value ranges index their group
    // here (the keys packed as mixed-radix digits over direct_lo /
    // direct_span), -1 for one not seen yet; a cache in front of
    // key_buckets, which still holds every group. Empty when the ranges are
    // too wide or a key is not that shape; cleared on a rebuild.
    std::vector<std::int64_t> direct_groups;
    // An open-addressing table in front of key_buckets for string-keyed
    // rows: each key cell as words (a string of up to 16 bytes as its length
    // and two words), a slot the hash's top bits over the group id, the
    // groups' words in fast_words (fast_stride each) and their hash in
    // fast_hash. A group the words cannot hold has FAST_NONE there and is
    // reached through key_buckets only. Refused once a group arrives by
    // another path (a merge).
    static constexpr std::uint64_t FAST_NONE = ~std::uint64_t{0};
    std::vector<std::uint64_t> fast_slots;
    std::vector<std::uint64_t> fast_words;
    std::vector<std::uint64_t> fast_hash;
    std::size_t fast_stride = 0;
    bool fast_refused = false;
    std::uint64_t direct_lo[3] = {0, 0, 0};
    std::uint64_t direct_span[3] = {0, 0, 0};
    bool direct_refused = false;
    std::vector<std::uint64_t> counts;  // per group: rows seen
    // The plain loop's accumulator when every reducer is count / sum /
    // mean / min / max: a count and three cells as bits in the field's
    // domain, one line for two fields where FieldStat takes one each.
    // While `light_on`, `light` (groups * nf) stands in for `fstats`, which
    // is empty; settle_light() folds it into fstats before anything else
    // reads them.
    struct LightStat {
        std::uint64_t n = 0;
        std::uint64_t sum = 0;
        std::uint64_t lo = 0;
        std::uint64_t hi = 0;
    };
    std::vector<LightStat> light;
    bool light_on = false;
    // Per group, the least row index that fed it, kept by the accumulate
    // loops and the merge (a group that came in any other way, a
    // deserialize, has none: the vector is then shorter than the groups
    // and the order is not restored).
    std::vector<std::int64_t> group_first_row;
    std::vector<FieldStat> fstats;  // groups * nf

    // First/Last state, allocated (groups * nf) only when `has_fl`. First/Last
    // are order-independent: each keeps the value at the smallest / largest
    // global row index seen for the group, so parallel-chunk merge order does
    // not matter. `fl_*_idx == -1` means no non-null value yet.
    bool has_fl = false;
    std::vector<char> field_is_str;       // field -> value column is String
    std::vector<std::uint64_t> fl_first;  // fixed-width raw bits
    std::vector<std::uint64_t> fl_last;
    std::vector<std::string> fl_first_s;  // string values
    std::vector<std::string> fl_last_s;
    std::vector<std::int64_t> fl_first_idx;
    std::vector<std::int64_t> fl_last_idx;

    // Per-group DDSketch state for Pct, allocated (groups * n_sketch) only when
    // `has_sketch`. All Pct specs on one field share a single sketch slot (the
    // quantiles read the same sketch); `field_sketch[field] == -1` marks a
    // field with no sketch.
    bool has_sketch = false;
    std::size_t n_sketch = 0;
    std::vector<int> field_sketch;
    std::vector<DDSketch> sketches;

    // ArgMax/ArgMin: a slot per distinct (by_col, direction) pair holds the
    // extreme `by` seen, and every spec bound to that slot keeps its own repr
    // of its own `value_col` at that same row. Sharing the slot is what makes N
    // specs naming one `by` column report one whole winning row. A tie on `by`
    // is broken on the repr of the lowest-indexed spec bound to the slot and
    // then adopted for the whole row, so the result is order-independent under
    // parallel merge. `arg_by_col`/`arg_val_col` are raw indices into the
    // `values` array passed to accumulate.
    bool has_arg = false;
    std::size_t n_arg = 0;                         // (by_col, direction) slots
    std::size_t n_arg_repr = 0;                    // ArgMax/ArgMin specs
    std::vector<int> spec_arg;                     // spec -> slot, or -1
    std::vector<int> spec_arg_repr;                // spec -> repr index, or -1
    std::vector<std::int32_t> arg_by_col;          // slot -> by_col
    std::vector<ArgDir> arg_dir;                   // slot -> direction
    std::vector<std::vector<int>> arg_slot_reprs;  // slot -> reprs, ascending
    std::vector<std::int32_t> arg_val_col;         // repr -> value_col
    std::vector<double> arg_by;                    // groups * n_arg
    std::vector<char> arg_has;                     // groups * n_arg
    std::vector<std::string> arg_repr;             // groups * n_arg_repr

    // BitOr: one slot per distinct value column; the accumulator is the u64 OR
    // (identity 0), so merge is an OR.
    bool has_bitor = false;
    std::size_t n_bitor = 0;
    std::vector<int> spec_bitor;           // spec -> slot, or -1
    std::vector<std::int32_t> bitor_val_col;
    std::vector<std::uint64_t> bitor_acc;  // groups * n_bitor

    // Prod: one slot per distinct value column; the accumulator is the
    // Float64 product (identity 1), so merge is a multiply.
    bool has_prod = false;
    std::size_t n_prod = 0;
    std::vector<int> spec_prod;    // spec -> slot, or -1
    std::vector<std::int32_t> prod_val_col;
    std::vector<double> prod_acc;  // groups * n_prod

    // Distinct + Sample: one KMV slot per distinct (value column, k) pair,
    // holding the k smallest element hashes with their reprs. Merge is a
    // union re-trimmed to k, so the sketch is order-independent.
    bool has_kmv = false;
    std::size_t n_kmv = 0;
    std::vector<int> spec_kmv;  // spec -> slot, or -1
    std::vector<std::int32_t> kmv_val_col;
    std::vector<std::size_t> kmv_k;
    std::vector<KmvMap> kmv;    // groups * n_kmv

    // ListSorted + TopK + BottomK: one slot per distinct (value column, by
    // column, kind, k); `lst_k[slot] == 0` is the unbounded ListSorted.
    bool has_lst = false;
    std::size_t n_lst = 0;
    std::vector<int> spec_lst;  // spec -> slot, or -1
    std::vector<std::int32_t> lst_val_col;
    std::vector<std::int32_t> lst_by_col;
    std::vector<ListKind> lst_kind;
    std::vector<std::size_t> lst_k;
    std::vector<ListItems> lst;  // groups * n_lst

    // ApproxTopK: one SpaceSaving summary per distinct (value column, k).
    bool has_ss = false;
    std::size_t n_ss = 0;
    std::vector<int> spec_ss;                 // spec -> slot, or -1
    std::vector<std::int32_t> ss_val_col;
    std::vector<std::size_t> ss_k;
    std::vector<SpaceSavingMap> ss_counters;  // groups * n_ss

    // Co-moments (Corr/CovarPop/CovarSamp/RegrSlope/RegrIntercept/RegrR2): one
    // slot per distinct (value column, by column) pair over x = by, y = value.
    // The six raw power sums are additive, so merge is a component-wise add.
    bool has_co = false;
    std::size_t n_co = 0;
    std::vector<int> spec_co;  // spec -> slot, or -1
    std::vector<std::int32_t> co_val_col;
    std::vector<std::int32_t> co_by_col;
    std::vector<double> co_n;  // each groups * n_co
    std::vector<double> co_sx;
    std::vector<double> co_sy;
    std::vector<double> co_sxx;
    std::vector<double> co_syy;
    std::vector<double> co_sxy;

    // SetUnion: one slot per SetUnion spec (no field-level dedup, mirroring the
    // View's AggSchema). `set_val_col` is a raw index into `values`.
    bool has_set = false;
    std::size_t n_set = 0;
    std::vector<int> spec_set;  // spec -> set slot, or -1
    std::vector<std::int32_t> set_val_col;
    // Sorted once at finalize; a hash set takes the inserts.
    using StringSet = ankerl::unordered_dense::set<std::string>;
    std::vector<StringSet> sets;  // groups * n_set

    // Occupancy: one slot per distinct (ts col, dur col, occ_cell) triple (all
    // occupancy ops on the same inputs share one delta-map, mirroring the
    // View's single per-group state). A slot holds a sparse +1/-1 endpoint
    // delta-map plus sum(dur)/min(ts)/max(end), all associative so
    // parallel/spilled partials merge exactly.
    using OccDeltas = ankerl::unordered_dense::map<std::uint64_t, std::int64_t>;
    bool has_occ = false;
    std::size_t n_occ = 0;
    std::vector<int> spec_occ;              // spec -> occ slot, or -1
    std::vector<std::int32_t> occ_val_col;  // slot -> ts value_col
    std::vector<std::int32_t> occ_by_col;   // slot -> dur by_col
    std::vector<std::uint64_t> occ_cell;    // slot -> endpoint-snap tolerance
    std::vector<OccDeltas> occ_deltas;      // groups * n_occ
    std::vector<std::uint64_t> occ_total;   // groups * n_occ
    std::vector<std::uint64_t> occ_ts;      // groups * n_occ (min; init max)
    std::vector<std::uint64_t> occ_te;      // groups * n_occ (max; init 0)

    // Name-keyed dyn side-table: per group, one FieldStat (and, when a Pct
    // reduction is configured, one DDSketch) per auto-discovered argument name.
    // The name set is data-dependent (grows during accumulate) and merges by
    // name-union, so a single streaming pass builds per-argument aggregates
    // without a name pre-scan. `dyn_domain` fixes each name's finalized column
    // type once (the domain of the first input column seen for it), so a group
    // missing a name still lands in that name's stable column type.
    bool has_dyn = false;
    bool dyn_has_sketch = false;
    std::vector<AggDynSpec> dyn_specs;
    std::vector<std::map<std::string, FieldStat>> dyn_fs;     // ngroups
    std::vector<std::map<std::string, DDSketch>> dyn_sketch;  // ngroups
    std::map<std::string, FieldStatDomain> dyn_domain;

    std::size_t nspecs() const { return specs.size(); }
    std::int64_t ngroups() const {
        return static_cast<std::int64_t>(counts.size());
    }

    // The distinct-field layout is a pure function of the specs.
    void init_layout() {
        spec_field.assign(specs.size(), -1);
        field_vc.clear();
        std::unordered_map<std::int32_t, int> seen;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            const AggSpec& sp = specs[s];
            if (sp.op == AggOp::Count || sp.op == AggOp::BitOr ||
                sp.op == AggOp::Prod || agg_uses_raw_value(sp.op) ||
                agg_uses_by_col(sp.op) || sp.value_col < 0)
                continue;
            auto it = seen.find(sp.value_col);
            if (it == seen.end()) {
                int fi = static_cast<int>(field_vc.size());
                seen.emplace(sp.value_col, fi);
                field_vc.push_back(sp.value_col);
                spec_field[s] = fi;
            } else {
                spec_field[s] = it->second;
            }
        }
        nf = field_vc.size();
        // Size the per-field layout up front (domain refined on first
        // accumulate) so a state finalized with no accumulate/seed still has
        // valid slots.
        field_domain.assign(nf, FieldStatDomain::F64);
        field_is_str.assign(nf, 0);
        has_fl = false;
        for (const AggSpec& sp : specs)
            if (sp.op == AggOp::First || sp.op == AggOp::Last) has_fl = true;

        // One shared sketch slot per field that any Pct or Hist spec
        // references (all quantiles and the histogram read the same sketch).
        field_sketch.assign(nf, -1);
        n_sketch = 0;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            if (specs[s].op != AggOp::Pct && specs[s].op != AggOp::Hist)
                continue;
            const int fi = spec_field[s];
            if (fi >= 0 && field_sketch[static_cast<std::size_t>(fi)] < 0)
                field_sketch[static_cast<std::size_t>(fi)] =
                    static_cast<int>(n_sketch++);
        }
        has_sketch = n_sketch > 0;

        spec_arg.assign(specs.size(), -1);
        spec_arg_repr.assign(specs.size(), -1);
        arg_by_col.clear();
        arg_dir.clear();
        arg_slot_reprs.clear();
        arg_val_col.clear();
        n_arg = 0;
        n_arg_repr = 0;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            const AggSpec& sp = specs[s];
            if (sp.op != AggOp::ArgMax && sp.op != AggOp::ArgMin) continue;
            const ArgDir dir =
                sp.op == AggOp::ArgMax ? ArgDir::Max : ArgDir::Min;
            int slot = -1;
            for (std::size_t k = 0; k < n_arg; ++k)
                if (arg_by_col[k] == sp.by_col && arg_dir[k] == dir) {
                    slot = static_cast<int>(k);
                    break;
                }
            if (slot < 0) {
                slot = static_cast<int>(n_arg++);
                arg_by_col.push_back(sp.by_col);
                arg_dir.push_back(dir);
                arg_slot_reprs.emplace_back();
            }
            const int ri = static_cast<int>(n_arg_repr++);
            spec_arg[s] = slot;
            spec_arg_repr[s] = ri;
            arg_val_col.push_back(sp.value_col);
            arg_slot_reprs[static_cast<std::size_t>(slot)].push_back(ri);
        }
        has_arg = n_arg > 0;

        spec_bitor.assign(specs.size(), -1);
        bitor_val_col.clear();
        n_bitor = 0;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            if (specs[s].op != AggOp::BitOr) continue;
            int slot = -1;
            for (std::size_t k = 0; k < n_bitor; ++k)
                if (bitor_val_col[k] == specs[s].value_col) {
                    slot = static_cast<int>(k);
                    break;
                }
            if (slot < 0) {
                slot = static_cast<int>(n_bitor++);
                bitor_val_col.push_back(specs[s].value_col);
            }
            spec_bitor[s] = slot;
        }
        has_bitor = n_bitor > 0;

        spec_prod.assign(specs.size(), -1);
        prod_val_col.clear();
        n_prod = 0;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            if (specs[s].op != AggOp::Prod) continue;
            int slot = -1;
            for (std::size_t k = 0; k < n_prod; ++k)
                if (prod_val_col[k] == specs[s].value_col) {
                    slot = static_cast<int>(k);
                    break;
                }
            if (slot < 0) {
                slot = static_cast<int>(n_prod++);
                prod_val_col.push_back(specs[s].value_col);
            }
            spec_prod[s] = slot;
        }
        has_prod = n_prod > 0;

        spec_kmv.assign(specs.size(), -1);
        kmv_val_col.clear();
        kmv_k.clear();
        n_kmv = 0;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            const AggSpec& sp = specs[s];
            if (sp.op != AggOp::Distinct && sp.op != AggOp::Sample) continue;
            const std::size_t k = agg_param_k(
                sp.param, sp.op == AggOp::Distinct ? AGG_DISTINCT_DEFAULT_K
                                                   : AGG_LIST_DEFAULT_K);
            int slot = -1;
            for (std::size_t j = 0; j < n_kmv; ++j)
                if (kmv_val_col[j] == sp.value_col && kmv_k[j] == k) {
                    slot = static_cast<int>(j);
                    break;
                }
            if (slot < 0) {
                slot = static_cast<int>(n_kmv++);
                kmv_val_col.push_back(sp.value_col);
                kmv_k.push_back(k);
            }
            spec_kmv[s] = slot;
        }
        has_kmv = n_kmv > 0;

        spec_lst.assign(specs.size(), -1);
        lst_val_col.clear();
        lst_by_col.clear();
        lst_kind.clear();
        lst_k.clear();
        n_lst = 0;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            const AggSpec& sp = specs[s];
            if (sp.op != AggOp::ListSorted && sp.op != AggOp::TopK &&
                sp.op != AggOp::BottomK)
                continue;
            const ListKind kind = sp.op == AggOp::TopK      ? ListKind::Top
                                  : sp.op == AggOp::BottomK ? ListKind::Bottom
                                                            : ListKind::Sorted;
            const std::size_t k =
                sp.op == AggOp::ListSorted
                    ? 0
                    : agg_param_k(sp.param, AGG_LIST_DEFAULT_K);
            int slot = -1;
            for (std::size_t j = 0; j < n_lst; ++j)
                if (lst_val_col[j] == sp.value_col &&
                    lst_by_col[j] == sp.by_col && lst_kind[j] == kind &&
                    lst_k[j] == k) {
                    slot = static_cast<int>(j);
                    break;
                }
            if (slot < 0) {
                slot = static_cast<int>(n_lst++);
                lst_val_col.push_back(sp.value_col);
                lst_by_col.push_back(sp.by_col);
                lst_kind.push_back(kind);
                lst_k.push_back(k);
            }
            spec_lst[s] = slot;
        }
        has_lst = n_lst > 0;

        spec_ss.assign(specs.size(), -1);
        ss_val_col.clear();
        ss_k.clear();
        n_ss = 0;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            const AggSpec& sp = specs[s];
            if (sp.op != AggOp::ApproxTopK) continue;
            const std::size_t k = agg_param_k(sp.param, AGG_LIST_DEFAULT_K);
            int slot = -1;
            for (std::size_t j = 0; j < n_ss; ++j)
                if (ss_val_col[j] == sp.value_col && ss_k[j] == k) {
                    slot = static_cast<int>(j);
                    break;
                }
            if (slot < 0) {
                slot = static_cast<int>(n_ss++);
                ss_val_col.push_back(sp.value_col);
                ss_k.push_back(k);
            }
            spec_ss[s] = slot;
        }
        has_ss = n_ss > 0;

        spec_co.assign(specs.size(), -1);
        co_val_col.clear();
        co_by_col.clear();
        n_co = 0;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            const AggSpec& sp = specs[s];
            if (sp.op != AggOp::Corr && sp.op != AggOp::CovarPop &&
                sp.op != AggOp::CovarSamp && sp.op != AggOp::RegrSlope &&
                sp.op != AggOp::RegrIntercept && sp.op != AggOp::RegrR2)
                continue;
            int slot = -1;
            for (std::size_t j = 0; j < n_co; ++j)
                if (co_val_col[j] == sp.value_col &&
                    co_by_col[j] == sp.by_col) {
                    slot = static_cast<int>(j);
                    break;
                }
            if (slot < 0) {
                slot = static_cast<int>(n_co++);
                co_val_col.push_back(sp.value_col);
                co_by_col.push_back(sp.by_col);
            }
            spec_co[s] = slot;
        }
        has_co = n_co > 0;

        spec_set.assign(specs.size(), -1);
        set_val_col.clear();
        n_set = 0;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            if (specs[s].op != AggOp::SetUnion) continue;
            spec_set[s] = static_cast<int>(n_set++);
            set_val_col.push_back(specs[s].value_col);
        }
        has_set = n_set > 0;

        spec_occ.assign(specs.size(), -1);
        occ_val_col.clear();
        occ_by_col.clear();
        occ_cell.clear();
        n_occ = 0;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            const AggSpec& sp = specs[s];
            if (sp.op != AggOp::Busy && sp.op != AggOp::Concurrency &&
                sp.op != AggOp::Utilization && sp.op != AggOp::Active)
                continue;
            const std::uint64_t cell = static_cast<std::uint64_t>(sp.param);
            int slot = -1;
            for (std::size_t k = 0; k < n_occ; ++k)
                if (occ_val_col[k] == sp.value_col &&
                    occ_by_col[k] == sp.by_col && occ_cell[k] == cell) {
                    slot = static_cast<int>(k);
                    break;
                }
            if (slot < 0) {
                slot = static_cast<int>(n_occ++);
                occ_val_col.push_back(sp.value_col);
                occ_by_col.push_back(sp.by_col);
                occ_cell.push_back(cell);
            }
            spec_occ[s] = slot;
        }
        has_occ = n_occ > 0;

        has_dyn = !dyn_specs.empty();
        dyn_has_sketch = false;
        for (const AggDynSpec& d : dyn_specs)
            if (d.op == AggOp::Pct) dyn_has_sketch = true;
    }

    void grow_group() {
        counts.push_back(0);
        if (light_on)
            light.resize(light.size() + nf);
        else
            fstats.resize(fstats.size() + nf);
        if (has_fl) {
            fl_first.resize(fl_first.size() + nf, 0);
            fl_last.resize(fl_last.size() + nf, 0);
            fl_first_s.resize(fl_first_s.size() + nf);
            fl_last_s.resize(fl_last_s.size() + nf);
            fl_first_idx.resize(fl_first_idx.size() + nf, -1);
            fl_last_idx.resize(fl_last_idx.size() + nf, -1);
        }
        if (has_sketch) sketches.resize(sketches.size() + n_sketch);
        if (has_arg) {
            arg_by.resize(arg_by.size() + n_arg, 0.0);
            arg_has.resize(arg_has.size() + n_arg, 0);
            arg_repr.resize(arg_repr.size() + n_arg_repr);
        }
        if (has_bitor) bitor_acc.resize(bitor_acc.size() + n_bitor, 0);
        if (has_prod) prod_acc.resize(prod_acc.size() + n_prod, 1.0);
        if (has_kmv) kmv.resize(kmv.size() + n_kmv);
        if (has_lst) lst.resize(lst.size() + n_lst);
        if (has_ss) ss_counters.resize(ss_counters.size() + n_ss);
        if (has_co) {
            co_n.resize(co_n.size() + n_co, 0.0);
            co_sx.resize(co_sx.size() + n_co, 0.0);
            co_sy.resize(co_sy.size() + n_co, 0.0);
            co_sxx.resize(co_sxx.size() + n_co, 0.0);
            co_syy.resize(co_syy.size() + n_co, 0.0);
            co_sxy.resize(co_sxy.size() + n_co, 0.0);
        }
        if (has_set) sets.resize(sets.size() + n_set);
        if (has_occ) {
            occ_deltas.resize(occ_deltas.size() + n_occ);
            occ_total.resize(occ_total.size() + n_occ, 0);
            occ_ts.resize(occ_ts.size() + n_occ,
                          (std::numeric_limits<std::uint64_t>::max)());
            occ_te.resize(occ_te.size() + n_occ, 0);
        }
        if (has_dyn) {
            dyn_fs.emplace_back();
            if (dyn_has_sketch) dyn_sketch.emplace_back();
        }
    }
    void settle_light() {
        if (!light_on) return;
        light_on = false;
        fstats.assign(light.size(), FieldStat{});
        for (std::size_t i = 0; i < light.size(); ++i) {
            const LightStat& l = light[i];
            FieldStat& f = fstats[i];
            f.n = l.n;
            if (l.n == 0) continue;
            f.domain = field_domain[i % nf];
            switch (f.domain) {
                case FieldStatDomain::F64:
                    f.sum = std::bit_cast<double>(l.sum);
                    f.min = std::bit_cast<double>(l.lo);
                    f.max = std::bit_cast<double>(l.hi);
                    break;
                case FieldStatDomain::I64:
                    f.esum = std::bit_cast<std::int64_t>(l.sum);
                    f.emin = std::bit_cast<std::int64_t>(l.lo);
                    f.emax = std::bit_cast<std::int64_t>(l.hi);
                    f.sum = static_cast<double>(f.esum);
                    f.min = static_cast<double>(f.emin);
                    f.max = static_cast<double>(f.emax);
                    break;
                case FieldStatDomain::U64:
                    f.esum = std::bit_cast<std::int64_t>(l.sum);
                    f.emin = std::bit_cast<std::int64_t>(l.lo);
                    f.emax = std::bit_cast<std::int64_t>(l.hi);
                    f.sum = static_cast<double>(l.sum);
                    f.min = static_cast<double>(l.lo);
                    f.max = static_cast<double>(l.hi);
                    break;
            }
        }
        light.clear();
        light.shrink_to_fit();
    }

    // Shared lookup for both live rows (get_int/get_str read a Series cell) and
    // merge (they read another AggState's already-materialized key columns):
    // hash the N cells, probe the bucket for an exact column-by-column match,
    // else append a new group.
    template <class GetInt, class GetStr, class GetNull>
    std::int64_t find_or_add_group(GetInt&& get_int, GetStr&& get_str,
                                   GetNull&& get_null) {
        std::size_t h = 0;
        for (std::size_t k = 0; k < nkeys; ++k) {
            const std::size_t cv =
                get_null(k) ? std::size_t{dftracer::utils::hash::GOLDEN_RATIO}
                : key_is_bytes[k] ? std::hash<std::string_view>{}(get_str(k))
                                  : static_cast<std::size_t>(get_int(k));
            dftracer::utils::hash_combine(h, cv);
        }
        auto [it, fresh] =
            key_buckets.try_emplace(static_cast<std::uint64_t>(h), -1);
        std::int64_t last = -1;
        for (std::int64_t g = it->second; g >= 0;
             g = next_in_bucket[static_cast<std::size_t>(g)]) {
            bool match = true;
            for (std::size_t k = 0; k < nkeys && match; ++k) {
                const bool gnull =
                    nkey_cols[k][static_cast<std::size_t>(g)] != 0;
                if (gnull || get_null(k))
                    match = gnull && get_null(k);
                else
                    match = key_is_bytes[k]
                                ? (skey_cols[k][static_cast<std::size_t>(g)] ==
                                   get_str(k))
                                : (ikey_cols[k][static_cast<std::size_t>(g)] ==
                                   get_int(k));
            }
            if (match) return g;
            last = g;
        }
        const std::int64_t g = ngroups();
        for (std::size_t k = 0; k < nkeys; ++k) {
            const bool null = get_null(k);
            nkey_cols[k].push_back(null ? 1 : 0);
            if (key_is_bytes[k])
                skey_cols[k].emplace_back(null ? std::string_view{}
                                               : get_str(k));
            else
                ikey_cols[k].push_back(null ? 0 : get_int(k));
        }
        next_in_bucket.push_back(-1);
        if (last < 0)
            it->second = g;
        else
            next_in_bucket[static_cast<std::size_t>(last)] = g;
        (void)fresh;
        grow_group();
        return g;
    }
    std::int64_t group_of(const std::vector<const Series*>& keys,
                          std::int64_t i) {
        // A non-string key column widens to int64 by its own domain, not
        // plain read_i64: a Uint64/Float64 key (e.g. a hash or a fractional
        // arg) would otherwise read as 0 for every row and collapse into one
        // group.
        return find_or_add_group(
            [&](std::size_t k) {
                return static_cast<std::int64_t>(
                    read_bits(*keys[k], i, col_domain(keys[k]->type())));
            },
            [&](std::size_t k) -> std::string_view {
                // String/LargeString are length-delimited (string_at handles
                // both offset widths); FixedSizeBinary/Decimal128/Decimal256
                // are `key_byte_width[k]` raw bytes per row, no offsets.
                const std::int32_t w = key_byte_width[k];
                if (w <= 0) return keys[k]->string_at(i);
                return std::string_view(keys[k]->data<char>() + i * w,
                                        static_cast<std::size_t>(w));
            },
            [&](std::size_t k) { return keys[k]->is_null(i); });
    }
    // The group here holding other's group j, or -1; adds nothing, so
    // callers may probe concurrently.
    std::int64_t find_group_of_other(const AggState& other,
                                     std::int64_t j) const {
        const auto sj = static_cast<std::size_t>(j);
        std::size_t h = 0;
        for (std::size_t k = 0; k < nkeys; ++k) {
            const std::size_t cv =
                other.nkey_cols[k][sj] != 0
                    ? std::size_t{dftracer::utils::hash::GOLDEN_RATIO}
                : key_is_bytes[k]
                    ? std::hash<std::string_view>{}(other.skey_cols[k][sj])
                    : static_cast<std::size_t>(other.ikey_cols[k][sj]);
            dftracer::utils::hash_combine(h, cv);
        }
        const auto it = key_buckets.find(static_cast<std::uint64_t>(h));
        if (it == key_buckets.end()) return -1;
        for (std::int64_t g = it->second; g >= 0;
             g = next_in_bucket[static_cast<std::size_t>(g)]) {
            const auto sg = static_cast<std::size_t>(g);
            bool match = true;
            for (std::size_t k = 0; k < nkeys && match; ++k) {
                const bool gnull = nkey_cols[k][sg] != 0;
                const bool onull = other.nkey_cols[k][sj] != 0;
                if (gnull || onull)
                    match = gnull && onull;
                else
                    match = key_is_bytes[k]
                                ? skey_cols[k][sg] == other.skey_cols[k][sj]
                                : ikey_cols[k][sg] == other.ikey_cols[k][sj];
            }
            if (match) return g;
        }
        return -1;
    }
    std::int64_t group_of_other(const AggState& other, std::int64_t j) {
        return find_or_add_group(
            [&](std::size_t k) {
                return other.ikey_cols[k][static_cast<std::size_t>(j)];
            },
            [&](std::size_t k) -> std::string_view {
                return other.skey_cols[k][static_cast<std::size_t>(j)];
            },
            [&](std::size_t k) {
                return other.nkey_cols[k][static_cast<std::size_t>(j)] != 0;
            });
    }

    // Recompute key_buckets from already-populated ikey_cols/skey_cols (after
    // deserialize), without re-appending group storage.
    void rebuild_key_buckets(std::int64_t ng) {
        key_buckets.clear();
        direct_groups.clear();
        next_in_bucket.assign(static_cast<std::size_t>(ng), -1);
        for (std::int64_t g = 0; g < ng; ++g) {
            std::size_t h = 0;
            for (std::size_t k = 0; k < nkeys; ++k) {
                const std::size_t cv =
                    nkey_cols[k][static_cast<std::size_t>(g)]
                        ? std::size_t{dftracer::utils::hash::GOLDEN_RATIO}
                    : key_is_bytes[k]
                        ? std::hash<std::string_view>{}(
                              skey_cols[k][static_cast<std::size_t>(g)])
                        : static_cast<std::size_t>(
                              ikey_cols[k][static_cast<std::size_t>(g)]);
                dftracer::utils::hash_combine(h, cv);
            }
            auto [it, fresh] =
                key_buckets.try_emplace(static_cast<std::uint64_t>(h), g);
            if (!fresh) {
                std::int64_t tail = it->second;
                while (next_in_bucket[static_cast<std::size_t>(tail)] >= 0)
                    tail = next_in_bucket[static_cast<std::size_t>(tail)];
                next_in_bucket[static_cast<std::size_t>(tail)] = g;
            }
        }
    }

    // Merge group `j` of `other` into this state's group `g` (already located).
    // Shared by agg_merge (1:1 keys) and agg_regroup (projected keys); assumes
    // this state adopted `other`'s spec/field layout.
    void merge_group(std::int64_t g, const AggState& other, std::int64_t j) {
        counts[static_cast<std::size_t>(g)] +=
            other.counts[static_cast<std::size_t>(j)];
        if (static_cast<std::size_t>(j) < other.group_first_row.size()) {
            const std::int64_t r =
                other.group_first_row[static_cast<std::size_t>(j)];
            if (static_cast<std::size_t>(g) == group_first_row.size())
                group_first_row.push_back(r);
            else if (static_cast<std::size_t>(g) < group_first_row.size())
                group_first_row[static_cast<std::size_t>(g)] =
                    std::min(group_first_row[static_cast<std::size_t>(g)], r);
        }
        const std::size_t db = static_cast<std::size_t>(g) * nf;
        const std::size_t sb = static_cast<std::size_t>(j) * nf;
        for (std::size_t fj = 0; fj < nf; ++fj)
            fstats[db + fj].merge(other.fstats[sb + fj]);
        if (has_fl) {
            for (std::size_t fj = 0; fj < nf; ++fj) {
                const std::size_t d = db + fj, s = sb + fj;
                const std::int64_t ofi = other.fl_first_idx[s];
                if (ofi >= 0 &&
                    (fl_first_idx[d] < 0 || ofi < fl_first_idx[d])) {
                    fl_first_idx[d] = ofi;
                    fl_first[d] = other.fl_first[s];
                    fl_first_s[d] = other.fl_first_s[s];
                }
                const std::int64_t oli = other.fl_last_idx[s];
                if (oli > fl_last_idx[d]) {
                    fl_last_idx[d] = oli;
                    fl_last[d] = other.fl_last[s];
                    fl_last_s[d] = other.fl_last_s[s];
                }
            }
        }
        if (has_sketch) {
            const std::size_t ds = static_cast<std::size_t>(g) * n_sketch;
            const std::size_t ss = static_cast<std::size_t>(j) * n_sketch;
            for (std::size_t sk = 0; sk < n_sketch; ++sk)
                sketches[ds + sk].merge(other.sketches[ss + sk]);
        }
        if (has_arg) {
            const std::size_t da = static_cast<std::size_t>(g) * n_arg;
            const std::size_t sa = static_cast<std::size_t>(j) * n_arg;
            const std::size_t dr = static_cast<std::size_t>(g) * n_arg_repr;
            const std::size_t sr = static_cast<std::size_t>(j) * n_arg_repr;
            for (std::size_t slot = 0; slot < n_arg; ++slot) {
                if (!other.arg_has[sa + slot]) continue;
                const std::vector<int>& reprs = arg_slot_reprs[slot];
                const bool adopt =
                    !arg_has[da + slot] ||
                    (arg_dir[slot] == ArgDir::Max
                         ? other.arg_by[sa + slot] > arg_by[da + slot]
                         : other.arg_by[sa + slot] < arg_by[da + slot]);
                if (!adopt) continue;
                arg_by[da + slot] = other.arg_by[sa + slot];
                arg_has[da + slot] = 1;
                for (int ri : reprs)
                    arg_repr[dr + static_cast<std::size_t>(ri)] =
                        other.arg_repr[sr + static_cast<std::size_t>(ri)];
            }
        }
        if (has_bitor) {
            const std::size_t dbo = static_cast<std::size_t>(g) * n_bitor;
            const std::size_t sbo = static_cast<std::size_t>(j) * n_bitor;
            for (std::size_t slot = 0; slot < n_bitor; ++slot)
                bitor_acc[dbo + slot] |= other.bitor_acc[sbo + slot];
        }
        if (has_prod) {
            const std::size_t dp = static_cast<std::size_t>(g) * n_prod;
            const std::size_t sp = static_cast<std::size_t>(j) * n_prod;
            for (std::size_t slot = 0; slot < n_prod; ++slot)
                prod_acc[dp + slot] *= other.prod_acc[sp + slot];
        }
        if (has_kmv) {
            const std::size_t dk = static_cast<std::size_t>(g) * n_kmv;
            const std::size_t sk2 = static_cast<std::size_t>(j) * n_kmv;
            for (std::size_t slot = 0; slot < n_kmv; ++slot) {
                KmvMap& dst = kmv[dk + slot];
                for (const auto& [h, v] : other.kmv[sk2 + slot])
                    dst.emplace(h, v);
                kmv_trim(dst, kmv_k[slot]);
            }
        }
        if (has_lst) {
            const std::size_t dl = static_cast<std::size_t>(g) * n_lst;
            const std::size_t sl2 = static_cast<std::size_t>(j) * n_lst;
            for (std::size_t slot = 0; slot < n_lst; ++slot) {
                ListItems& dst = lst[dl + slot];
                const ListItems& src = other.lst[sl2 + slot];
                dst.insert(dst.end(), src.begin(), src.end());
                list_bound(dst, lst_kind[slot], lst_k[slot]);
            }
        }
        if (has_ss) {
            const std::size_t dss = static_cast<std::size_t>(g) * n_ss;
            const std::size_t sss = static_cast<std::size_t>(j) * n_ss;
            for (std::size_t slot = 0; slot < n_ss; ++slot)
                for (const auto& [v, c] : other.ss_counters[sss + slot])
                    space_saving_offer(ss_counters[dss + slot], ss_k[slot], v,
                                       c);
        }
        if (has_co) {
            const std::size_t dc = static_cast<std::size_t>(g) * n_co;
            const std::size_t sc = static_cast<std::size_t>(j) * n_co;
            for (std::size_t slot = 0; slot < n_co; ++slot) {
                co_n[dc + slot] += other.co_n[sc + slot];
                co_sx[dc + slot] += other.co_sx[sc + slot];
                co_sy[dc + slot] += other.co_sy[sc + slot];
                co_sxx[dc + slot] += other.co_sxx[sc + slot];
                co_syy[dc + slot] += other.co_syy[sc + slot];
                co_sxy[dc + slot] += other.co_sxy[sc + slot];
            }
        }
        if (has_set) {
            const std::size_t ds = static_cast<std::size_t>(g) * n_set;
            const std::size_t ss = static_cast<std::size_t>(j) * n_set;
            for (std::size_t slot = 0; slot < n_set; ++slot)
                sets[ds + slot].insert(other.sets[ss + slot].begin(),
                                       other.sets[ss + slot].end());
        }
        if (has_occ) {
            const std::size_t ds = static_cast<std::size_t>(g) * n_occ;
            const std::size_t ss = static_cast<std::size_t>(j) * n_occ;
            for (std::size_t slot = 0; slot < n_occ; ++slot) {
                occ_total[ds + slot] += other.occ_total[ss + slot];
                if (other.occ_ts[ss + slot] < occ_ts[ds + slot])
                    occ_ts[ds + slot] = other.occ_ts[ss + slot];
                if (other.occ_te[ss + slot] > occ_te[ds + slot])
                    occ_te[ds + slot] = other.occ_te[ss + slot];
                for (const auto& [t, dlt] : other.occ_deltas[ss + slot])
                    occ_deltas[ds + slot][t] += dlt;
            }
        }
        if (has_dyn) {
            for (const auto& [name, sm] :
                 other.dyn_fs[static_cast<std::size_t>(j)])
                dyn_fs[static_cast<std::size_t>(g)][name].merge(sm);
            if (dyn_has_sketch)
                for (const auto& [name, sk] :
                     other.dyn_sketch[static_cast<std::size_t>(j)])
                    dyn_sketch[static_cast<std::size_t>(g)][name].merge(sk);
        }
    }

    // Adopt `other`'s spec/field/layout metadata into an empty (uninited) state
    // without copying any group data. Shared by agg_merge and agg_regroup.
    void adopt_layout(const AggState& other) {
        specs = other.specs;
        spec_field = other.spec_field;
        field_vc = other.field_vc;
        field_domain = other.field_domain;
        field_is_str = other.field_is_str;
        nf = other.nf;
        has_fl = other.has_fl;
        has_sketch = other.has_sketch;
        n_sketch = other.n_sketch;
        field_sketch = other.field_sketch;
        has_arg = other.has_arg;
        n_arg = other.n_arg;
        n_arg_repr = other.n_arg_repr;
        spec_arg = other.spec_arg;
        spec_arg_repr = other.spec_arg_repr;
        arg_by_col = other.arg_by_col;
        arg_dir = other.arg_dir;
        arg_slot_reprs = other.arg_slot_reprs;
        arg_val_col = other.arg_val_col;
        has_bitor = other.has_bitor;
        n_bitor = other.n_bitor;
        spec_bitor = other.spec_bitor;
        bitor_val_col = other.bitor_val_col;
        has_prod = other.has_prod;
        n_prod = other.n_prod;
        spec_prod = other.spec_prod;
        prod_val_col = other.prod_val_col;
        has_kmv = other.has_kmv;
        n_kmv = other.n_kmv;
        spec_kmv = other.spec_kmv;
        kmv_val_col = other.kmv_val_col;
        kmv_k = other.kmv_k;
        has_lst = other.has_lst;
        n_lst = other.n_lst;
        spec_lst = other.spec_lst;
        lst_val_col = other.lst_val_col;
        lst_by_col = other.lst_by_col;
        lst_kind = other.lst_kind;
        lst_k = other.lst_k;
        has_ss = other.has_ss;
        n_ss = other.n_ss;
        spec_ss = other.spec_ss;
        ss_val_col = other.ss_val_col;
        ss_k = other.ss_k;
        has_co = other.has_co;
        n_co = other.n_co;
        spec_co = other.spec_co;
        co_val_col = other.co_val_col;
        co_by_col = other.co_by_col;
        has_set = other.has_set;
        n_set = other.n_set;
        spec_set = other.spec_set;
        set_val_col = other.set_val_col;
        has_occ = other.has_occ;
        n_occ = other.n_occ;
        spec_occ = other.spec_occ;
        occ_val_col = other.occ_val_col;
        occ_by_col = other.occ_by_col;
        occ_cell = other.occ_cell;
        has_dyn = other.has_dyn;
        dyn_has_sketch = other.dyn_has_sketch;
        dyn_specs = other.dyn_specs;
        dyn_domain = other.dyn_domain;
    }
};

/// `st` with its light accumulators folded into fstats (a representation
/// change only, so a const reader may ask for it).
inline const AggState& settled(const AggState& st) {
    const_cast<AggState&>(st).settle_light();
    return st;
}

/// Whether every group of `st` has a first row (agg_first_rows).
inline bool agg_first_rows_known(const AggState& st) {
    return st.group_first_row.size() == static_cast<std::size_t>(st.ngroups());
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_AGG_DETAIL_H
