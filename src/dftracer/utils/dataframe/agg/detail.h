#ifndef DFTRACER_UTILS_DATAFRAME_AGG_DETAIL_H
#define DFTRACER_UTILS_DATAFRAME_AGG_DETAIL_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dstd/hash/hash_combine.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <bit>
#include <cmath>
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

// Separator joining a SetUnion group's distinct values into one text cell;
// matches views/view_aggregate.h SET_SEP so the two engines agree.
inline constexpr char AGG_SET_SEP = '\x1e';

// SetUnion's typed result rides AggSpec::param: 0 is the joined text, nonzero
// is a list column. The state records the element type in its own copy of the
// spec once it sees the value column (param = 1 + TypeId), so a merged or
// deserialized partial knows it with no field of its own.
inline bool set_typed(const AggSpec& sp) { return sp.param != 0.0; }

FieldStatDomain col_domain(TypeId t);
std::uint64_t read_bits(const Series& c, std::int64_t i, FieldStatDomain d);
bool is_bytes_key_type(TypeId t);

/// Reorder the groups of `st` so group i becomes old group perm[i].
void agg_permute_groups(AggState& st, const std::vector<std::int64_t>& perm);

/// `st` with its groups in first-row order (the order they were first
/// seen in a frame accumulated in parallel); untouched when they already
/// are, or their first rows are not known.
AggStatePtr agg_in_first_seen_order(AggStatePtr st);

/// The aggregate columns of `st` alone, with no key columns: the same columns
/// agg_finalize writes after its keys, for a caller that writes the keys
/// itself.
DataFrame agg_finalize_values(const AggState& st);

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

/// One dyn cell of a group: a name's running statistic, under that name's id.
/// A group keeps its cells sorted by id, so a lookup is a binary search and the
/// whole group's dyn storage is one block with no name in it.
struct DynCell {
    FieldStat stat;
    std::uint32_t id = 0;
};

/// The Pct reduction's per-name sketch, beside its cell.
struct DynSketchCell {
    DDSketch sketch{};
    std::uint32_t id = 0;
};

/// One dyn input of an accumulate call: the name's id and the domain of this
/// call's column for it (a batch may hold the same name in another domain).
struct DynSlot {
    std::uint32_t id = 0;
    FieldStatDomain domain = FieldStatDomain::F64;
};

/// One group's dyn cells in the light form: `dyn_light_stride` words a cell -
/// the id word (bit `DYN_LIGHT_PRESENT` when the name has a value and the mask
/// keeps no count word) then the cells its mask keeps, in the static light
/// row's order - and, beside them, an {id, net wrap} pair per cell whose
/// integer sum overflowed (a light word holds only the wrapped sum).
struct DynLightCells {
    dstd::Vector<std::uint64_t> words;
    dstd::Vector<std::pair<std::uint32_t, std::int64_t>> carry;
};

/// The id word's "this cell has a value" bit, where the static light row keeps
/// a field's presence bit. Ids stay below it.
constexpr std::uint64_t DYN_LIGHT_PRESENT = std::uint64_t{1} << 31;
constexpr std::uint64_t DYN_LIGHT_ID = DYN_LIGHT_PRESENT - 1;
constexpr std::uint8_t DYN_LIGHT_PRESENT_BIT = 31;

inline bool dyn_stat_before(const DynCell& c, std::uint32_t id) {
    return c.id < id;
}

inline bool dyn_sketch_before(const DynSketchCell& c, std::uint32_t id) {
    return c.id < id;
}

/// The cell of `cells` (sorted by id) for `id`, adding a zero cell in place
/// when the group has not seen that name yet.
inline FieldStat& dyn_cell_of(dstd::Vector<DynCell>& cells, std::uint32_t id) {
    // A call's dyn inputs come in the same order in every row, so the id
    // usually extends the vector.
    if (cells.empty() || cells.back().id < id) {
        cells.emplace_back();
        cells.back().id = id;
        return cells.back().stat;
    }
    auto it = std::lower_bound(cells.begin(), cells.end(), id, dyn_stat_before);
    if (it != cells.end() && it->id == id) return it->stat;
    it = cells.insert(it, DynCell{});
    it->id = id;
    return it->stat;
}

inline DDSketch& dyn_sketch_cell_of(dstd::Vector<DynSketchCell>& cells,
                                    std::uint32_t id) {
    if (cells.empty() || cells.back().id < id) {
        cells.emplace_back();
        cells.back().id = id;
        return cells.back().sketch;
    }
    auto it =
        std::lower_bound(cells.begin(), cells.end(), id, dyn_sketch_before);
    if (it != cells.end() && it->id == id) return it->sketch;
    it = cells.insert(it, DynSketchCell{});
    it->id = id;
    return it->sketch;
}

/// The stat of `id` in `cells`, or null when the group has not seen the name.
inline const FieldStat* dyn_find_cell(const dstd::Vector<DynCell>& cells,
                                      std::uint32_t id) {
    const auto it =
        std::lower_bound(cells.begin(), cells.end(), id, dyn_stat_before);
    return it != cells.end() && it->id == id ? &it->stat : nullptr;
}

inline const DDSketch* dyn_find_sketch(const dstd::Vector<DynSketchCell>& cells,
                                       std::uint32_t id) {
    const auto it =
        std::lower_bound(cells.begin(), cells.end(), id, dyn_sketch_before);
    return it != cells.end() && it->id == id ? &it->sketch : nullptr;
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
    // 1 where a String key column holds canonical JSON text, so finalize
    // keeps the flag on the group key.
    std::vector<char> key_json;  // nkeys
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
    // A group's row is a presence word (bit f: field f has a value) and then,
    // per field, only the cells its reducers read, in the order count, sum,
    // min, max, as bits in the field's domain: a field that is only summed (or
    // only min'd, or only max'd) takes one word where a general cell took four.
    // A variance or standard deviation adds three words after those (the mean
    // as the pair shift + cmean, and the sum of squared deviations cm2), the
    // fields FieldStat's Welford update keeps for them, so a std column takes
    // seven words at most where a FieldStat took fourteen.
    // `light` holds light_stride words a group.
    static constexpr std::uint8_t LIGHT_SUM = 1;   // sum and mean read it
    static constexpr std::uint8_t LIGHT_MIN = 2;
    static constexpr std::uint8_t LIGHT_MAX = 4;
    static constexpr std::uint8_t LIGHT_N = 8;     // count, mean: a count word
    static constexpr std::uint8_t LIGHT_MOM = 16;  // var, std: three words
    std::vector<std::uint64_t> light;
    std::vector<std::uint8_t> light_mask;  // field -> LIGHT_* cells it keeps
    std::vector<std::uint32_t>
        light_off;  // field -> word offset in a group row
    std::size_t light_stride = 0;
    // The cell layout from the specs; the state must hold no group yet.
    void init_light() {
        light_mask.assign(nf, 0);
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const int f = spec_field[i];
            if (f < 0) continue;
            switch (specs[i].op) {
                case AggOp::Sum:
                    light_mask[static_cast<std::size_t>(f)] |= LIGHT_SUM;
                    break;
                case AggOp::Mean:
                    light_mask[static_cast<std::size_t>(f)] |=
                        LIGHT_SUM | LIGHT_N;
                    break;
                case AggOp::CountValid:
                    light_mask[static_cast<std::size_t>(f)] |= LIGHT_N;
                    break;
                case AggOp::Min:
                    light_mask[static_cast<std::size_t>(f)] |= LIGHT_MIN;
                    break;
                case AggOp::Max:
                    light_mask[static_cast<std::size_t>(f)] |= LIGHT_MAX;
                    break;
                case AggOp::Var:
                case AggOp::Std:
                    light_mask[static_cast<std::size_t>(f)] |=
                        LIGHT_MOM | LIGHT_N;
                    break;
                default:
                    break;
            }
        }
        light_off.assign(nf, 0);
        light_stride = 1;  // the presence word
        for (std::size_t f = 0; f < nf; ++f) {
            // The presence word holds 64 fields; a later one keeps a count.
            if (f >= 64) light_mask[f] |= LIGHT_N;
            light_off[f] = static_cast<std::uint32_t>(light_stride);
            light_stride += light_words(light_mask[f]);
        }
        light.clear();
    }
    // Net wraps of an integer light sum by `light` index (FieldStat::ecarry),
    // present only once that sum wraps.
    ankerl::unordered_dense::map<std::size_t, std::int64_t> light_carry;
    bool light_on = false;
    // Per group, the least row index that fed it, kept by the accumulate
    // loops and the merge (a group that came in any other way, a
    // deserialize, has none: the vector is then shorter than the groups
    // and the order is not restored).
    std::vector<std::int64_t> group_first_row;
    // Row index of row 0 of the batch being accumulated (agg_set_row_base).
    std::int64_t row_base = 0;
    // When set, the original row of batch row i is row_ids[i] (a batch that
    // is a gather of rows, agg_set_row_ids); row_base is then not used.
    const std::int64_t* row_ids = nullptr;
    std::int64_t row_of(std::int64_t i) const {
        return row_ids ? row_ids[i] : row_base + i;
    }
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

    // SketchQuantile: one slot per distinct value column holding the merge of
    // every valid serialized DDSketch of the group; the quantile is read at
    // finalize, so specs with different `param` share a slot. An empty sketch
    // marks a group with no valid input. A sketch of another accuracy than
    // the default is skipped (merge_default_accuracy).
    bool has_msk = false;
    std::size_t n_msk = 0;
    std::vector<int> spec_msk;     // spec -> slot, or -1
    std::vector<std::int32_t> msk_val_col;
    std::vector<MergeSketch> msk;  // groups * n_msk

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
    // One CoStat per group and slot: central co-moments, merged with the Chan
    // combination (associative, so any partitioning gives the one-pass result).
    bool has_co = false;
    std::size_t n_co = 0;
    std::vector<int> spec_co;  // spec -> slot, or -1
    std::vector<std::int32_t> co_val_col;
    std::vector<std::int32_t> co_by_col;
    std::vector<CoStat> co;    // each groups * n_co

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
    // without a name pre-scan. A name is interned once and named by id from
    // then on: `dyn_names` is id -> name, `dyn_ids` the reverse lookup
    // (transparent, so a string_view finds its id without a copy) and
    // `dyn_domain` the domain a name's finalized column keeps (the domain of
    // the first input column seen for it, so a group missing a name still lands
    // in that name's stable column type).
    bool has_dyn = false;
    bool dyn_has_sketch = false;
    std::vector<AggDynSpec> dyn_specs;
    dstd::Vector<dstd::String> dyn_names;
    dstd::HashMap<std::string, std::uint32_t, TransparentStringHash,
                  TransparentStringEqual>
        dyn_ids;
    dstd::Vector<FieldStatDomain> dyn_domain;
    dstd::Vector<dstd::Vector<DynCell>> dyn_fs;            // ngroups
    dstd::Vector<dstd::Vector<DynSketchCell>> dyn_sketch;  // ngroups
    // The cells the light form holds instead, when `dyn_light_on` (the two
    // are never both populated). One mask for every name - a dyn spec applies
    // to every name - so a group's cells are fixed-size word records and no
    // cell holds a FieldStat.
    dstd::Vector<DynLightCells> dyn_light;  // ngroups
    std::size_t dyn_light_stride = 0;  // words a cell, the id word included
    bool dyn_light_ok = false;  // every dyn spec reads what the light row keeps
    bool dyn_light_on = false;
    std::uint8_t dyn_light_mask = 0;
    // The current accumulate call's dyn inputs, resolved once before its row
    // loop so no row touches a name.
    dstd::Vector<DynSlot> dyn_slots;

    /// The id no dyn name has.
    static constexpr std::uint32_t NO_DYN_ID = 0xFFFFFFFFu;

    std::string_view dyn_name(std::uint32_t id) const {
        const dstd::String& n = dyn_names[static_cast<std::size_t>(id)];
        return std::string_view(n.data(), n.size());
    }
    /// The id of a dyn name, or NO_DYN_ID when this state has not seen it.
    std::uint32_t dyn_id_of(std::string_view name) const {
        const auto it = dyn_ids.find(name);
        return it == dyn_ids.end() ? NO_DYN_ID : it->second;
    }
    /// The id of a dyn name, interning the name with `domain` on first use.
    std::uint32_t dyn_intern(std::string_view name, FieldStatDomain domain) {
        const auto it = dyn_ids.find(name);
        if (it != dyn_ids.end()) return it->second;
        const auto id = static_cast<std::uint32_t>(dyn_names.size());
        dyn_names.emplace_back(name.data(), name.size());
        dyn_domain.push_back(domain);
        dyn_ids.emplace(std::string(name), id);
        return id;
    }
    /// Intern every name of `other`, returning other's id -> this state's id.
    /// Mutates the name table, so call it before a parallel merge.
    dstd::Vector<std::uint32_t> dyn_remap(const AggState& other) {
        dstd::Vector<std::uint32_t> out;
        out.reserve(other.dyn_names.size());
        for (std::size_t i = 0; i < other.dyn_names.size(); ++i)
            out.push_back(
                dyn_intern(other.dyn_name(static_cast<std::uint32_t>(i)),
                           other.dyn_domain[i]));
        return out;
    }

    /// The light cells a dyn spec reads out of a name's cell, by the same rules
    /// init_light uses for a field. `fits` is false for an op the light row
    /// cannot hold (the higher moments, the raw value, the collections).
    static std::uint8_t dyn_light_bits(AggOp op, bool& fits) {
        fits = true;
        switch (op) {
            case AggOp::Count:
            case AggOp::CountValid:
                return LIGHT_N;  // the reduction reads the cell's own count
            case AggOp::Sum:
                return LIGHT_SUM;
            case AggOp::Mean:
                return static_cast<std::uint8_t>(LIGHT_SUM | LIGHT_N);
            case AggOp::Min:
                return LIGHT_MIN;
            case AggOp::Max:
                return LIGHT_MAX;
            case AggOp::Var:
            case AggOp::Std:
                return static_cast<std::uint8_t>(LIGHT_MOM | LIGHT_N);
            case AggOp::Pct:
                return 0;  // the reduction reads its sketch, not the cell
            default:
                fits = false;
                return 0;
        }
    }

    /// The cells a light field of `mask` keeps, the presence word aside.
    static std::size_t light_words(std::uint8_t mask) {
        return static_cast<std::size_t>((mask & LIGHT_N) != 0) +
               static_cast<std::size_t>((mask & LIGHT_SUM) != 0) +
               static_cast<std::size_t>((mask & LIGHT_MIN) != 0) +
               static_cast<std::size_t>((mask & LIGHT_MAX) != 0) +
               3 * static_cast<std::size_t>((mask & LIGHT_MOM) != 0);
    }

    /// The light dyn mask and record size this state's specs need, or
    /// `dyn_light_ok` false for the specs the light row cannot hold.
    void dyn_light_layout() {
        bool fits = has_dyn;
        std::uint8_t mask = 0;
        for (const AggDynSpec& d : dyn_specs) {
            bool op_fits = true;
            const std::uint8_t bits = dyn_light_bits(d.op, op_fits);
            if (!op_fits) fits = false;
            mask = static_cast<std::uint8_t>(mask | bits);
        }
        dyn_light_ok = fits;
        dyn_light_mask = fits ? mask : 0;
        dyn_light_stride = 1 + light_words(dyn_light_mask);
    }

    /// The light cell of `id` in group `g`, or null when the group has none.
    const std::uint64_t* dyn_light_find(std::int64_t g,
                                        std::uint32_t id) const {
        const dstd::Vector<std::uint64_t>& w =
            dyn_light[static_cast<std::size_t>(g)].words;
        const std::size_t at = dyn_light_lower(w, id);
        return at < w.size() && (w[at] & DYN_LIGHT_ID) == id ? &w[at] : nullptr;
    }

    /// The light cell of `id` in group `g`, adding one in id order when the
    /// group has not seen that name yet.
    std::uint64_t* dyn_light_cell(std::int64_t g, std::uint32_t id) {
        dstd::Vector<std::uint64_t>& w =
            dyn_light[static_cast<std::size_t>(g)].words;
        const std::size_t at = dyn_light_lower(w, id);
        if (at == w.size() || (w[at] & DYN_LIGHT_ID) != id) {
            w.insert(w.begin() + static_cast<std::ptrdiff_t>(at),
                     dyn_light_stride, 0);
            w[at] = id;
        }
        return &w[at];
    }

    /// The word index of `id`'s cell in a group's `words`, or its size when the
    /// cell is absent: the cells sit at a fixed stride in id order.
    std::size_t dyn_light_lower(const dstd::Vector<std::uint64_t>& w,
                                std::uint32_t id) const {
        std::size_t lo = 0, hi = w.size() / dyn_light_stride;
        // A call's dyn inputs come in the same order in every row, so the id
        // usually extends the block.
        if (hi != 0 && (w[(hi - 1) * dyn_light_stride] & DYN_LIGHT_ID) < id)
            lo = hi;
        else
            while (lo < hi) {
                const std::size_t m = (lo + hi) / 2;
                if ((w[m * dyn_light_stride] & DYN_LIGHT_ID) < id)
                    lo = m + 1;
                else
                    hi = m;
            }
        return lo * dyn_light_stride;
    }

    /// The net wrap of `id`'s sum in group `g`, 0 when it never overflowed.
    std::int64_t dyn_light_carry_of(std::int64_t g, std::uint32_t id) const {
        const dstd::Vector<std::pair<std::uint32_t, std::int64_t>>& c =
            dyn_light[static_cast<std::size_t>(g)].carry;
        const auto it =
            std::lower_bound(c.begin(), c.end(), id,
                             [](const std::pair<std::uint32_t, std::int64_t>& p,
                                std::uint32_t v) { return p.first < v; });
        return it != c.end() && it->first == id ? it->second : 0;
    }

    /// Note `wrap` more on `id`'s sum in group `g`.
    void dyn_light_carry_add(std::int64_t g, std::uint32_t id,
                             std::int64_t wrap) {
        dstd::Vector<std::pair<std::uint32_t, std::int64_t>>& c =
            dyn_light[static_cast<std::size_t>(g)].carry;
        const auto it =
            std::lower_bound(c.begin(), c.end(), id,
                             [](const std::pair<std::uint32_t, std::int64_t>& p,
                                std::uint32_t v) { return p.first < v; });
        if (it != c.end() && it->first == id)
            it->second += wrap;
        else
            c.insert(it, std::make_pair(id, wrap));
    }

    /// Set `id`'s wrap in group `g`, dropping the entry at zero.
    void dyn_light_carry_set(std::int64_t g, std::uint32_t id,
                             std::int64_t wrap) {
        dstd::Vector<std::pair<std::uint32_t, std::int64_t>>& c =
            dyn_light[static_cast<std::size_t>(g)].carry;
        const auto it =
            std::lower_bound(c.begin(), c.end(), id,
                             [](const std::pair<std::uint32_t, std::int64_t>& p,
                                std::uint32_t v) { return p.first < v; });
        if (it != c.end() && it->first == id) {
            if (wrap == 0)
                c.erase(it);
            else
                it->second = wrap;
        } else if (wrap != 0) {
            c.insert(it, std::make_pair(id, wrap));
        }
    }

    /// The running stat `id`'s light cell holds, its exact sum included.
    FieldStat dyn_light_stat_of(std::int64_t g, std::uint32_t id,
                                const std::uint64_t* cell) const {
        FieldStat f =
            light_words_stat(cell, 1, dyn_light_mask, DYN_LIGHT_PRESENT_BIT,
                             dyn_domain[static_cast<std::size_t>(id)]);
        f.ecarry = dyn_light_carry_of(g, id);
        return f;
    }

    /// The stat of dyn name `id` in group `g`, false when the group has no cell
    /// for it.
    bool dyn_stat_at(std::int64_t g, std::uint32_t id, FieldStat& out) const {
        if (dyn_light_on) {
            const std::uint64_t* cell = dyn_light_find(g, id);
            if (cell == nullptr) return false;
            out = dyn_light_stat_of(g, id, cell);
            return true;
        }
        const FieldStat* f =
            dyn_find_cell(dyn_fs[static_cast<std::size_t>(g)], id);
        if (f == nullptr) return false;
        out = *f;
        return true;
    }

    /// Visit a group's dyn cells in id order. The stat is only valid for the
    /// call: the light form builds it from the cell's words.
    template <class F>
    void dyn_each_cell(std::int64_t g, F&& f) const {
        if (dyn_light_on) {
            const dstd::Vector<std::uint64_t>& w =
                dyn_light[static_cast<std::size_t>(g)].words;
            for (std::size_t off = 0; off < w.size(); off += dyn_light_stride) {
                const auto id =
                    static_cast<std::uint32_t>(w[off] & DYN_LIGHT_ID);
                FieldStat st = light_words_stat(
                    w.data() + off, 1, dyn_light_mask, DYN_LIGHT_PRESENT_BIT,
                    dyn_domain[static_cast<std::size_t>(id)]);
                st.ecarry = dyn_light_carry_of(g, id);
                f(id, st);
            }
            return;
        }
        for (const DynCell& c : dyn_fs[static_cast<std::size_t>(g)])
            f(c.id, c.stat);
    }

    /// Merge `o` into `id`'s light cell in group `g`: the cell's own stat is
    /// unpacked, so the moments combine exactly as FieldStat::merge does.
    /// `o`'s domain must be the one the name keeps.
    void dyn_light_merge(std::int64_t g, std::uint32_t id, const FieldStat& o) {
        if (o.n == 0) return;
        std::uint64_t* cell = dyn_light_cell(g, id);
        FieldStat dst =
            light_words_stat(cell, 1, dyn_light_mask, DYN_LIGHT_PRESENT_BIT,
                             dyn_domain[static_cast<std::size_t>(id)]);
        dst.ecarry = dyn_light_carry_of(g, id);
        dst.merge(o);
        dyn_light_store(g, id, cell, dst);
    }

    /// Whether this state's light form can hold every cell `other` brings: a
    /// cell's words are read in its name's domain, so a cell of another domain
    /// needs the full form.
    bool dyn_light_holds(const AggState& other,
                         const dstd::Vector<std::uint32_t>* map) const {
        const auto my_id = [&](std::uint32_t id) {
            return map == nullptr ? id : (*map)[static_cast<std::size_t>(id)];
        };
        if (other.dyn_light_on) {
            for (std::size_t i = 0; i < other.dyn_names.size(); ++i)
                if (other.dyn_domain[i] !=
                    dyn_domain[my_id(static_cast<std::uint32_t>(i))])
                    return false;
            return true;
        }
        for (const dstd::Vector<DynCell>& cells : other.dyn_fs)
            for (const DynCell& c : cells)
                if (c.stat.n != 0 && c.stat.domain != dyn_domain[my_id(c.id)])
                    return false;
        return true;
    }

    /// Take the light dyn form for a merge destination that holds no group
    /// yet, when it can hold every cell `other` brings. A destination already
    /// light keeps it, or settles back to full cells for `other`.
    bool dyn_light_open_for_merge(const AggState& other,
                                  const dstd::Vector<std::uint32_t>* map) {
        if (!dyn_light_ok) return false;
        if (dyn_light_on) {
            if (!other.has_dyn || dyn_light_holds(other, map)) return true;
            dyn_settle();
            return false;
        }
        if (other.has_dyn && !dyn_light_holds(other, map)) return false;
        for (const dstd::Vector<DynCell>& cells : dyn_fs)
            if (!cells.empty()) return false;
        dyn_light_on = true;
        dyn_light.assign(static_cast<std::size_t>(ngroups()), DynLightCells{});
        dyn_fs.clear();
        dyn_fs.shrink_to_fit();
        return true;
    }

    /// The light cells as full DynCells (a representation change only).
    void dyn_settle() {
        if (!dyn_light_on) return;
        dyn_fs.resize(static_cast<std::size_t>(ngroups()));
        for (std::int64_t g = 0; g < ngroups(); ++g) {
            dstd::Vector<DynCell>& cells = dyn_fs[static_cast<std::size_t>(g)];
            // Read the light cells before dropping them.
            dyn_each_cell(g, [&](std::uint32_t id, const FieldStat& f) {
                cells.push_back(DynCell{f, id});
            });
        }
        dyn_light_on = false;
        dyn_light.clear();
        dyn_light.shrink_to_fit();
    }

    /// `f`'s cells into a light cell's words, its net wrap beside them.
    void dyn_light_store(std::int64_t g, std::uint32_t id, std::uint64_t* cell,
                         const FieldStat& f) {
        const std::uint8_t mask = dyn_light_mask;
        std::size_t pos = 1;
        if ((mask & LIGHT_N) != 0) {
            cell[pos++] = f.n;
        } else {
            cell[0] = (cell[0] & DYN_LIGHT_ID) |
                      (f.n != 0 ? DYN_LIGHT_PRESENT : std::uint64_t{0});
        }
        if (dyn_domain[static_cast<std::size_t>(id)] == FieldStatDomain::F64) {
            if ((mask & LIGHT_SUM) != 0)
                cell[pos++] = std::bit_cast<std::uint64_t>(f.sum);
            if ((mask & LIGHT_MIN) != 0)
                cell[pos++] = std::bit_cast<std::uint64_t>(f.min);
            if ((mask & LIGHT_MAX) != 0)
                cell[pos++] = std::bit_cast<std::uint64_t>(f.max);
        } else {
            if ((mask & LIGHT_SUM) != 0)
                cell[pos++] = std::bit_cast<std::uint64_t>(f.esum);
            if ((mask & LIGHT_MIN) != 0)
                cell[pos++] = std::bit_cast<std::uint64_t>(f.emin);
            if ((mask & LIGHT_MAX) != 0)
                cell[pos++] = std::bit_cast<std::uint64_t>(f.emax);
        }
        if ((mask & LIGHT_MOM) != 0) {
            cell[pos++] = std::bit_cast<std::uint64_t>(f.shift);
            cell[pos++] = std::bit_cast<std::uint64_t>(f.cmean);
            cell[pos++] = std::bit_cast<std::uint64_t>(f.cm2);
        }
        dyn_light_carry_set(g, id, f.ecarry);
    }

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

        spec_msk.assign(specs.size(), -1);
        msk_val_col.clear();
        n_msk = 0;
        for (std::size_t s = 0; s < specs.size(); ++s) {
            if (specs[s].op != AggOp::SketchQuantile) continue;
            int slot = -1;
            for (std::size_t k = 0; k < n_msk; ++k)
                if (msk_val_col[k] == specs[s].value_col) {
                    slot = static_cast<int>(k);
                    break;
                }
            if (slot < 0) {
                slot = static_cast<int>(n_msk++);
                msk_val_col.push_back(specs[s].value_col);
            }
            spec_msk[s] = slot;
        }
        has_msk = n_msk > 0;

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
        dyn_light_layout();
    }

    // The groups a state is expected to hold (an estimate from a probe of the
    // rows), reserved once the first group arrives, so the per-group arrays
    // are allocated once and not regrown (and freed) as the groups come in.
    std::size_t groups_hint = 0;
    void reserve_groups(std::size_t h) {
        counts.reserve(h);
        group_first_row.reserve(h);
        next_in_bucket.reserve(h);
        for (std::size_t k = 0; k < nkeys; ++k) {
            nkey_cols[k].reserve(h);
            if (key_is_bytes[k])
                skey_cols[k].reserve(h);
            else
                ikey_cols[k].reserve(h);
        }
        key_buckets.reserve(h);
        if (light_on)
            light.reserve(h * light_stride);
        else
            fstats.reserve(h * nf);
        if (dyn_light_on) dyn_light.reserve(h);
    }
    void grow_group() {
        counts.push_back(0);
        if (light_on)
            light.resize(light.size() + light_stride);
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
        if (has_msk) msk.resize(msk.size() + n_msk);
        if (has_kmv) kmv.resize(kmv.size() + n_kmv);
        if (has_lst) lst.resize(lst.size() + n_lst);
        if (has_ss) ss_counters.resize(ss_counters.size() + n_ss);
        if (has_co) co.resize(co.size() + n_co);
        if (has_set) sets.resize(sets.size() + n_set);
        if (has_occ) {
            occ_deltas.resize(occ_deltas.size() + n_occ);
            occ_total.resize(occ_total.size() + n_occ, 0);
            occ_ts.resize(occ_ts.size() + n_occ,
                          (std::numeric_limits<std::uint64_t>::max)());
            occ_te.resize(occ_te.size() + n_occ, 0);
        }
        if (has_dyn) {
            if (dyn_light_on)
                dyn_light.emplace_back();
            else
                dyn_fs.emplace_back();
            if (dyn_has_sketch) dyn_sketch.emplace_back();
        }
    }
    // The field stat of slot i (group * nf + field) from its light cells.
    FieldStat light_stat(std::size_t i) const {
        const std::size_t fld = i % nf;
        FieldStat f = light_words_stat(
            light.data() + (i / nf) * light_stride, light_off[fld],
            light_mask[fld], static_cast<std::uint8_t>(fld), field_domain[fld]);
        if (!light_carry.empty())
            if (const auto it = light_carry.find(i); it != light_carry.end())
                f.ecarry = it->second;
        return f;
    }

    // The stat a light row's cell holds: `row` is the group's row, `off` the
    // cell's word offset in it, `bit` the presence bit (the field's index, or
    // the id word's bit for a dyn cell) and `domain` the accumulation domain.
    static FieldStat light_words_stat(const std::uint64_t* row,
                                      std::uint32_t off, std::uint8_t mask,
                                      std::uint8_t bit,
                                      FieldStatDomain domain) {
        const std::uint64_t* c = row + off;
        FieldStat f;
        std::size_t pos = 0;
        if (mask & LIGHT_N)
            f.n = c[pos++];
        else
            f.n = (row[0] >> bit) & 1;
        if (f.n == 0) return f;
        struct Cells {
            std::uint64_t sum = 0, lo = 0, hi = 0;
        } l;
        if (mask & LIGHT_SUM) l.sum = c[pos++];
        if (mask & LIGHT_MIN) l.lo = c[pos++];
        if (mask & LIGHT_MAX) l.hi = c[pos++];
        if (mask & LIGHT_MOM) {
            f.shift = std::bit_cast<double>(c[pos++]);
            f.cmean = std::bit_cast<double>(c[pos++]);
            f.cm2 = std::bit_cast<double>(c[pos++]);
        }
        f.domain = domain;
        switch (domain) {
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
        return f;
    }
    // The field stat of slot i, wherever it is kept.
    FieldStat field_stat(std::size_t i) const {
        return light_on ? light_stat(i) : fstats[i];
    }
    void settle_light() {
        if (!light_on) return;
        std::vector<FieldStat> settled_stats(ngroups() * nf);
        for (std::size_t i = 0; i < settled_stats.size(); ++i)
            settled_stats[i] = light_stat(i);
        fstats = std::move(settled_stats);
        light_on = false;
        light_carry.clear();
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
        // Before the lookup: reserving rehashes key_buckets, which would leave
        // `it` below pointing at a bucket that moved.
        if (groups_hint != 0 && ngroups() == 0) {
            reserve_groups(groups_hint);
            groups_hint = 0;
        }
        [[maybe_unused]] auto [it, fresh] =
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
    /// `dyn_map` maps other's dyn ids to this state's; null when the two
    /// states share one name table.
    void merge_group(std::int64_t g, const AggState& other, std::int64_t j,
                     const dstd::Vector<std::uint32_t>* dyn_map = nullptr) {
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
        for (std::size_t fj = 0; fj < nf; ++fj) {
            FieldStat unpacked;
            fstats[db + fj].merge(other.light_on
                                      ? (unpacked = other.light_stat(sb + fj))
                                      : other.fstats[sb + fj]);
        }
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
        if (has_msk) {
            const std::size_t dm = static_cast<std::size_t>(g) * n_msk;
            const std::size_t sm = static_cast<std::size_t>(j) * n_msk;
            for (std::size_t slot = 0; slot < n_msk; ++slot)
                merge_default_accuracy(msk[dm + slot], other.msk[sm + slot]);
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
            for (std::size_t slot = 0; slot < n_co; ++slot)
                co[dc + slot].merge(other.co[sc + slot]);
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
            auto other_id = [&](std::uint32_t id) {
                return dyn_map ? (*dyn_map)[static_cast<std::size_t>(id)] : id;
            };
            if (dyn_light_on) {
                other.dyn_each_cell(j,
                                    [&](std::uint32_t id, const FieldStat& f) {
                                        dyn_light_merge(g, other_id(id), f);
                                    });
            } else {
                dstd::Vector<DynCell>& cells =
                    dyn_fs[static_cast<std::size_t>(g)];
                other.dyn_each_cell(
                    j, [&](std::uint32_t id, const FieldStat& f) {
                        dyn_cell_of(cells, other_id(id)).merge(f);
                    });
            }
            if (dyn_has_sketch) {
                dstd::Vector<DynSketchCell>& sk =
                    dyn_sketch[static_cast<std::size_t>(g)];
                for (const DynSketchCell& c :
                     other.dyn_sketch[static_cast<std::size_t>(j)])
                    dyn_sketch_cell_of(sk, other_id(c.id)).merge(c.sketch);
            }
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
        has_msk = other.has_msk;
        n_msk = other.n_msk;
        spec_msk = other.spec_msk;
        msk_val_col = other.msk_val_col;
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
        dyn_names = other.dyn_names;
        dyn_ids = other.dyn_ids;
        dyn_domain = other.dyn_domain;
        dyn_light_layout();
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
