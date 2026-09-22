#include <ankerl/unordered_dense.h>
#include <dftracer/utils/dataframe/agg/detail.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/float16.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

namespace {

// Separator joining a SetUnion group's distinct values into one text cell;
// matches views/view_aggregate.h SET_SEP so the two engines agree.
constexpr char AGG_SET_SEP = '\x1e';

// One group's occupancy scalars from its endpoint delta-map. Reproduces
// view_aggregate.cpp occupancy_summary byte-for-byte: busy is the exact
// interval union (depth-sweep over sorted deltas), active the peak depth, span
// the makespan, total the raw sum(dur).
struct OccResult {
    std::uint64_t busy = 0;
    std::uint64_t active = 0;
    std::uint64_t total = 0;
    std::uint64_t span = 0;
};

OccResult occ_summarize(
    const ankerl::unordered_dense::map<std::uint64_t, std::int64_t>& deltas,
    std::uint64_t total, std::uint64_t ts, std::uint64_t te) {
    OccResult o;
    o.total = total;
    o.span = te > ts ? te - ts : 0;
    if (deltas.empty()) return o;
    std::vector<std::pair<std::uint64_t, std::int64_t>> pts(deltas.begin(),
                                                            deltas.end());
    std::sort(pts.begin(), pts.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    std::int64_t depth = 0;
    std::uint64_t last = 0, busy = 0, peak = 0;
    bool started = false;
    for (const auto& [t, dlt] : pts) {
        if (started && depth > 0) busy += t - last;
        depth += dlt;
        if (depth > 0 && static_cast<std::uint64_t>(depth) > peak)
            peak = static_cast<std::uint64_t>(depth);
        last = t;
        started = true;
    }
    o.busy = busy;
    o.active = peak;
    return o;
}

// KMV (bottom-k min-hash) distinct estimate: (k-1) / u_max, where u_max is the
// largest kept hash normalized to [0, 1). Exact (the kept count) while fewer
// than k distinct hashes have been seen.
//
// Bar-Yossef, Jayram, Kumar, Sivakumar, Trevisan, "Counting Distinct Elements
// in a Data Stream", RANDOM 2002.
double kmv_estimate(const KmvMap& m, std::size_t k) {
    if (k < 2 || m.size() < k) return static_cast<double>(m.size());
    const double u_max =
        static_cast<double>(m.rbegin()->first) / std::ldexp(1.0, 64);
    if (!(u_max > 0.0)) return static_cast<double>(m.size());
    return static_cast<double>(k - 1) / u_max;
}

// Time32/Time64/Timestamp/Duration group key column. Series::flat has no
// parametric constructor, so this builds the dftu_series directly to carry
// the source's own time_unit (and, for Timestamp, timezone).
Series flat_temporal(TypeId type, TimeUnit unit, const std::string& timezone,
                     const void* data, std::int64_t n) {
    auto* col = new dftu_series();
    col->type = type;
    col->encoding = Encoding::Flat;
    col->length = n;
    col->time_unit = unit;
    if (type == TypeId::Timestamp) col->timezone = timezone;
    const std::size_t bytes = buffer_bytes(type, n);
    col->data = Buffer::allocate(bytes);
    if (bytes != 0) std::memcpy(col->data->data(), data, bytes);
    return Series{col};
}

// A String/Binary (32-bit offset) group key column: skey_cols already holds
// each group's raw bytes. Series::strings hardcodes TypeId::String, so a
// Binary key is built directly, mirroring wide_bytes_key below.
Series bytes_key(TypeId type, const std::vector<std::string>& values) {
    auto* col = new dftu_series();
    col->type = type;
    col->encoding = Encoding::Flat;
    col->length = static_cast<std::int64_t>(values.size());
    std::vector<std::int32_t> off(values.size() + 1, 0);
    std::string data;
    for (std::size_t i = 0; i < values.size(); ++i) {
        data += values[i];
        off[i + 1] = static_cast<std::int32_t>(data.size());
    }
    col->offsets = Buffer::allocate(off.size() * sizeof(std::int32_t));
    std::memcpy(col->offsets->data(), off.data(),
                off.size() * sizeof(std::int32_t));
    col->data = Buffer::allocate(data.size());
    if (!data.empty()) std::memcpy(col->data->data(), data.data(), data.size());
    return Series{col};
}

// A LargeString/LargeBinary group key column: skey_cols already holds each
// group's raw bytes (its distinct value). dftu_series_new_string only builds
// the 32-bit offset String/Binary pair, so this is built directly, mirroring
// flat_temporal above.
Series wide_bytes_key(TypeId type, const std::vector<std::string>& values) {
    auto* col = new dftu_series();
    col->type = type;
    col->encoding = Encoding::Flat;
    col->length = static_cast<std::int64_t>(values.size());
    std::vector<std::int64_t> off(values.size() + 1, 0);
    std::string data;
    for (std::size_t i = 0; i < values.size(); ++i) {
        data += values[i];
        off[i + 1] = static_cast<std::int64_t>(data.size());
    }
    col->offsets64 = Buffer::allocate(off.size() * sizeof(std::int64_t));
    std::memcpy(col->offsets64->data(), off.data(),
                off.size() * sizeof(std::int64_t));
    col->data = Buffer::allocate(data.size());
    if (!data.empty()) std::memcpy(col->data->data(), data.data(), data.size());
    return Series{col};
}

// A FixedSizeBinary/Decimal128/Decimal256 group key column: each group's
// value is already `key_byte_width` raw bytes in skey_cols. dftu_series_
// new_flat validates via byte_width(type), which is 0 for FixedSizeBinary
// (its width is a DataType parameter, not a per-TypeId constant), so this is
// built directly; see kernels/group_by.cpp's build_groups for the same
// shape.
Series bytes_flat_key(TypeId type, std::int32_t fixed_size,
                      std::int32_t decimal_precision,
                      std::int32_t decimal_scale,
                      const std::vector<std::string>& values) {
    auto* col = new dftu_series();
    col->type = type;
    col->encoding = Encoding::Flat;
    col->length = static_cast<std::int64_t>(values.size());
    col->fixed_size = fixed_size;
    col->decimal_precision = decimal_precision;
    col->decimal_scale = decimal_scale;
    std::string bytes;
    for (const std::string& v : values) bytes += v;
    col->data = Buffer::allocate(bytes.size());
    if (!bytes.empty())
        std::memcpy(col->data->data(), bytes.data(), bytes.size());
    return Series{col};
}

// One list<string> column from a per-group vector of already-ordered reprs.
Series strings_list(const std::vector<std::vector<std::string>>& rows) {
    std::vector<std::int32_t> off{0};
    std::vector<std::string> flat;
    off.reserve(rows.size() + 1);
    for (const std::vector<std::string>& row : rows) {
        flat.insert(flat.end(), row.begin(), row.end());
        off.push_back(static_cast<std::int32_t>(flat.size()));
    }
    return Series::list(off, Series::strings(flat));
}

// The two-variable readouts over one co-moment slot's raw power sums.
// Reproduces the deleted plugin monoid byte-for-byte, zero-guards included.
double co_readout(AggOp op, double n, double sx, double sy, double sxx,
                  double syy, double sxy) {
    if (n < 2.0) return 0.0;
    const double cxy = sxy - sx * sy / n;
    const double cxx = sxx - sx * sx / n;
    const double cyy = syy - sy * sy / n;
    auto corr = [&]() -> double {
        const double denom = cxx * cyy;
        return denom > 0.0 ? cxy / std::sqrt(denom) : 0.0;
    };
    auto slope = [&]() -> double { return cxx > 0.0 ? cxy / cxx : 0.0; };
    switch (op) {
        case AggOp::CovarPop:
            return cxy / n;
        case AggOp::CovarSamp:
            return cxy / (n - 1.0);
        case AggOp::Corr:
            return corr();
        case AggOp::RegrSlope:
            return slope();
        case AggOp::RegrIntercept:
            return sy / n - slope() * sx / n;
        default:  // RegrR2
            return corr() * corr();
    }
}

}  // namespace

// The null key groups of column k as the output column's validity bitmap.
static void mark_null_keys(Series& col,
                           const std::vector<std::uint8_t>& nulls) {
    std::int64_t count = 0;
    for (std::uint8_t n : nulls) count += n ? 1 : 0;
    if (count == 0) return;
    dftu_series* h = col.handle();
    const std::size_t bytes = static_cast<std::size_t>((h->length + 7) / 8);
    auto validity = Buffer::allocate(bytes);
    std::memset(validity->data(), 0xFF, bytes);
    auto* bits = validity->data();
    for (std::int64_t g = 0; g < h->length; ++g)
        if (nulls[static_cast<std::size_t>(g)])
            bits[g >> 3] &= static_cast<std::uint8_t>(~(1u << (g & 7)));
    h->validity = std::move(validity);
    h->null_count = count;
}

DataFrame agg_finalize(const AggState& st_in,
                       const std::vector<std::string>& key_names) {
    const AggState& st = settled(st_in);
    const std::int64_t ng = st.ngroups();
    const std::size_t ns = st.nspecs();
    DataFrame out;
    // A state finalized without accumulating (empty stream) has no key layout;
    // still emit one empty column per requested key so the schema is complete.
    const std::size_t nk =
        st.nkeys > key_names.size() ? st.nkeys : key_names.size();
    for (std::size_t k = 0; k < nk; ++k) {
        out.names.push_back(k < key_names.size() ? key_names[k]
                                                 : "key" + std::to_string(k));
        if (k >= st.nkeys) {
            out.columns.push_back(Series::strings(
                std::vector<std::string>(static_cast<std::size_t>(ng))));
            continue;
        }
        if (st.key_is_bytes[k]) {
            const TypeId kt =
                k < st.key_type.size() ? st.key_type[k] : TypeId::String;
            if (kt == TypeId::LargeString || kt == TypeId::LargeBinary) {
                out.columns.push_back(wide_bytes_key(kt, st.skey_cols[k]));
            } else if (kt == TypeId::FixedSizeBinary ||
                       kt == TypeId::Decimal128 || kt == TypeId::Decimal256) {
                out.columns.push_back(bytes_flat_key(
                    kt, st.key_byte_width[k], st.key_decimal_precision[k],
                    st.key_decimal_scale[k], st.skey_cols[k]));
            } else {
                // The seed path (agg_seed_begin) marks every key bytes-domain
                // with a placeholder key_type of Int64, since it never sees
                // the source column's real type; String is the only safe
                // default for that case, so only a genuine Binary key (from
                // accumulate on a real Binary column) retags as Binary.
                const TypeId bt =
                    kt == TypeId::Binary ? TypeId::Binary : TypeId::String;
                out.columns.push_back(bytes_key(bt, st.skey_cols[k]));
            }
            continue;
        }
        const FieldStatDomain kd =
            k < st.key_domain.size() ? st.key_domain[k] : FieldStatDomain::I64;
        const TypeId kt =
            k < st.key_type.size() ? st.key_type[k] : TypeId::Int64;
        const std::vector<std::int64_t>& bits = st.ikey_cols[k];
        if (kd == FieldStatDomain::U64) {
            std::vector<std::uint64_t> v(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g)
                v[static_cast<std::size_t>(g)] = std::bit_cast<std::uint64_t>(
                    bits[static_cast<std::size_t>(g)]);
            out.columns.push_back(Series::flat(TypeId::Uint64, v.data(), ng));
        } else if (kd == FieldStatDomain::F64 && kt == TypeId::Float16) {
            // Keyed on the exact half->double promotion (see col_domain), so
            // narrowing back is exact for every value that round-tripped
            // through accumulate: zero, subnormals, normals and infinities
            // all recover their original bits. A NaN key's payload does not
            // survive (float_to_half canonicalizes any NaN mantissa), only
            // that the group stays NaN with its original sign.
            std::vector<std::uint16_t> v(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g)
                v[static_cast<std::size_t>(g)] =
                    float_to_half(static_cast<float>(std::bit_cast<double>(
                        bits[static_cast<std::size_t>(g)])));
            out.columns.push_back(Series::flat(TypeId::Float16, v.data(), ng));
        } else if (kd == FieldStatDomain::F64) {
            std::vector<double> v(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g)
                v[static_cast<std::size_t>(g)] =
                    std::bit_cast<double>(bits[static_cast<std::size_t>(g)]);
            out.columns.push_back(Series::flat_f64(v.data(), ng));
        } else {
            // I64 domain: retag with key_type so a temporal key stays
            // Date32/64/Time32/64/Timestamp/Duration, not a bare Int64.
            // bits is always sign-extended to 64, so the 4-byte types narrow
            // back down.
            const TimeUnit ku = k < st.key_time_unit.size()
                                    ? st.key_time_unit[k]
                                    : TimeUnit::Micro;
            const std::string& ktz =
                k < st.key_timezone.size() ? st.key_timezone[k] : std::string();
            if (kt == TypeId::Time32) {
                std::vector<std::int32_t> v(static_cast<std::size_t>(ng));
                for (std::int64_t g = 0; g < ng; ++g)
                    v[static_cast<std::size_t>(g)] = static_cast<std::int32_t>(
                        bits[static_cast<std::size_t>(g)]);
                out.columns.push_back(flat_temporal(kt, ku, ktz, v.data(), ng));
            } else if (kt == TypeId::Date32) {
                std::vector<std::int32_t> v(static_cast<std::size_t>(ng));
                for (std::int64_t g = 0; g < ng; ++g)
                    v[static_cast<std::size_t>(g)] = static_cast<std::int32_t>(
                        bits[static_cast<std::size_t>(g)]);
                out.columns.push_back(Series::flat(kt, v.data(), ng));
            } else if (kt == TypeId::Time64 || kt == TypeId::Timestamp ||
                       kt == TypeId::Duration) {
                out.columns.push_back(
                    flat_temporal(kt, ku, ktz, bits.data(), ng));
            } else if (kt == TypeId::Date64) {
                out.columns.push_back(Series::flat(kt, bits.data(), ng));
            } else {
                out.columns.push_back(Series::flat_i64(bits.data(), ng));
            }
        }
    }

    for (std::size_t k = 0; k < st.nkeys && k < st.nkey_cols.size(); ++k)
        mark_null_keys(out.columns[k], st.nkey_cols[k]);

    for (std::size_t s = 0; s < ns; ++s) {
        const AggSpec& sp = st.specs[s];
        const int fi = st.spec_field[s];
        out.names.push_back(sp.out);
        auto fs_at = [&](std::int64_t g) -> const FieldStat& {
            return st.fstats[static_cast<std::size_t>(g) * st.nf +
                             static_cast<std::size_t>(fi)];
        };
        const FieldStatDomain dom =
            fi >= 0 ? st.field_domain[static_cast<std::size_t>(fi)]
                    : FieldStatDomain::I64;

        if (sp.op == AggOp::Pct) {
            const int sk =
                fi >= 0 ? st.field_sketch[static_cast<std::size_t>(fi)] : -1;
            std::vector<double> v(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g)
                v[static_cast<std::size_t>(g)] =
                    sk >= 0 ? st.sketches[static_cast<std::size_t>(g) *
                                              st.n_sketch +
                                          static_cast<std::size_t>(sk)]
                                  .quantile(sp.param)
                            : 0.0;
            out.columns.push_back(Series::flat_f64(v.data(), ng));
        } else if (sp.op == AggOp::Hist) {
            // One list<struct{lo,hi,count}> row per group, from the shared
            // sketch's occupied bins (same shape the View emits).
            const int sk =
                fi >= 0 ? st.field_sketch[static_cast<std::size_t>(fi)] : -1;
            std::vector<std::int32_t> off{0};
            std::vector<double> lo, hi;
            std::vector<std::uint64_t> cnt;
            for (std::int64_t g = 0; g < ng; ++g) {
                if (sk >= 0) {
                    for (const auto& bn :
                         st.sketches[static_cast<std::size_t>(g) * st.n_sketch +
                                     static_cast<std::size_t>(sk)]
                             .bins()) {
                        lo.push_back(bn.lower);
                        hi.push_back(bn.upper);
                        cnt.push_back(bn.count);
                    }
                }
                off.push_back(static_cast<std::int32_t>(lo.size()));
            }
            const std::int64_t nb = static_cast<std::int64_t>(lo.size());
            std::vector<Series> fields;
            fields.push_back(Series::flat(TypeId::Float64, lo.data(), nb));
            fields.push_back(Series::flat(TypeId::Float64, hi.data(), nb));
            fields.push_back(Series::flat(TypeId::Uint64, cnt.data(), nb));
            out.columns.push_back(Series::list(
                off,
                Series::structs({"lo", "hi", "count"}, std::move(fields))));
        } else if (sp.op == AggOp::First || sp.op == AggOp::Last) {
            const bool first = sp.op == AggOp::First;
            const std::vector<std::uint64_t>& bits =
                first ? st.fl_first : st.fl_last;
            const std::vector<std::string>& strs =
                first ? st.fl_first_s : st.fl_last_s;
            auto slot = [&](std::int64_t g) {
                return static_cast<std::size_t>(g) * st.nf +
                       static_cast<std::size_t>(fi);
            };
            if (fi >= 0 && st.field_is_str[static_cast<std::size_t>(fi)]) {
                std::vector<std::string> v(static_cast<std::size_t>(ng));
                for (std::int64_t g = 0; g < ng; ++g)
                    v[static_cast<std::size_t>(g)] = strs[slot(g)];
                out.columns.push_back(Series::strings(v));
            } else if (dom == FieldStatDomain::F64) {
                std::vector<double> v(static_cast<std::size_t>(ng));
                for (std::int64_t g = 0; g < ng; ++g)
                    v[static_cast<std::size_t>(g)] =
                        std::bit_cast<double>(bits[slot(g)]);
                out.columns.push_back(Series::flat_f64(v.data(), ng));
            } else if (dom == FieldStatDomain::U64) {
                std::vector<std::uint64_t> v(static_cast<std::size_t>(ng));
                for (std::int64_t g = 0; g < ng; ++g)
                    v[static_cast<std::size_t>(g)] = bits[slot(g)];
                out.columns.push_back(
                    Series::flat(TypeId::Uint64, v.data(), ng));
            } else {
                std::vector<std::int64_t> v(static_cast<std::size_t>(ng));
                for (std::int64_t g = 0; g < ng; ++g)
                    v[static_cast<std::size_t>(g)] =
                        std::bit_cast<std::int64_t>(bits[slot(g)]);
                out.columns.push_back(Series::flat_i64(v.data(), ng));
            }
        } else if (sp.op == AggOp::SumSq) {
            std::vector<double> v(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g)
                v[static_cast<std::size_t>(g)] = fs_at(g).sumsq;
            out.columns.push_back(Series::flat_f64(v.data(), ng));
        } else if (sp.op == AggOp::ArgMax || sp.op == AggOp::ArgMin) {
            const int slot = st.spec_arg[s];
            const int ri = st.spec_arg_repr[s];
            std::vector<std::string> v(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g) {
                if (slot < 0 || ri < 0) continue;
                if (st.arg_has[static_cast<std::size_t>(g) * st.n_arg +
                               static_cast<std::size_t>(slot)])
                    v[static_cast<std::size_t>(g)] =
                        st.arg_repr[static_cast<std::size_t>(g) *
                                        st.n_arg_repr +
                                    static_cast<std::size_t>(ri)];
            }
            out.columns.push_back(Series::strings(v));
        } else if (sp.op == AggOp::BitOr) {
            const int slot = st.spec_bitor[s];
            std::vector<std::uint64_t> v(static_cast<std::size_t>(ng), 0);
            for (std::int64_t g = 0; g < ng && slot >= 0; ++g)
                v[static_cast<std::size_t>(g)] =
                    st.bitor_acc[static_cast<std::size_t>(g) * st.n_bitor +
                                 static_cast<std::size_t>(slot)];
            out.columns.push_back(Series::flat(TypeId::Uint64, v.data(), ng));
        } else if (sp.op == AggOp::Prod) {
            const int slot = st.spec_prod[s];
            std::vector<double> v(static_cast<std::size_t>(ng), 1.0);
            for (std::int64_t g = 0; g < ng && slot >= 0; ++g)
                v[static_cast<std::size_t>(g)] =
                    st.prod_acc[static_cast<std::size_t>(g) * st.n_prod +
                                static_cast<std::size_t>(slot)];
            out.columns.push_back(Series::flat_f64(v.data(), ng));
        } else if (sp.op == AggOp::Distinct) {
            const int slot = st.spec_kmv[s];
            std::vector<std::int64_t> v(static_cast<std::size_t>(ng), 0);
            for (std::int64_t g = 0; g < ng && slot >= 0; ++g) {
                const std::size_t ks = static_cast<std::size_t>(g) * st.n_kmv +
                                       static_cast<std::size_t>(slot);
                v[static_cast<std::size_t>(g)] =
                    static_cast<std::int64_t>(std::llround(kmv_estimate(
                        st.kmv[ks], st.kmv_k[static_cast<std::size_t>(slot)])));
            }
            out.columns.push_back(Series::flat_i64(v.data(), ng));
        } else if (sp.op == AggOp::Sample) {
            const int slot = st.spec_kmv[s];
            std::vector<std::vector<std::string>> rows(
                static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng && slot >= 0; ++g) {
                const std::size_t ks = static_cast<std::size_t>(g) * st.n_kmv +
                                       static_cast<std::size_t>(slot);
                std::vector<std::string>& row =
                    rows[static_cast<std::size_t>(g)];
                for (const auto& [h, val] : st.kmv[ks]) row.push_back(val);
                std::sort(row.begin(), row.end());
            }
            out.columns.push_back(strings_list(rows));
        } else if (sp.op == AggOp::ListSorted || sp.op == AggOp::TopK ||
                   sp.op == AggOp::BottomK) {
            const int slot = st.spec_lst[s];
            std::vector<std::vector<std::string>> rows(
                static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng && slot >= 0; ++g) {
                const std::size_t ls = static_cast<std::size_t>(slot);
                ListItems items =
                    st.lst[static_cast<std::size_t>(g) * st.n_lst + ls];
                list_bound(items, st.lst_kind[ls], st.lst_k[ls]);
                std::vector<std::string>& row =
                    rows[static_cast<std::size_t>(g)];
                row.reserve(items.size());
                for (auto& [by, repr] : items) row.push_back(std::move(repr));
            }
            out.columns.push_back(strings_list(rows));
        } else if (sp.op == AggOp::ApproxTopK) {
            // list<struct{value, count}>, heaviest first, value breaking ties.
            const int slot = st.spec_ss[s];
            std::vector<std::int32_t> off{0};
            std::vector<std::string> vals;
            std::vector<std::uint64_t> cnts;
            for (std::int64_t g = 0; g < ng; ++g) {
                if (slot >= 0) {
                    const SpaceSavingMap& m =
                        st.ss_counters[static_cast<std::size_t>(g) * st.n_ss +
                                       static_cast<std::size_t>(slot)];
                    std::vector<std::pair<std::string, std::uint64_t>> hh(
                        m.begin(), m.end());
                    std::sort(hh.begin(), hh.end(),
                              [](const auto& a, const auto& b) {
                                  if (a.second != b.second)
                                      return a.second > b.second;
                                  return a.first < b.first;
                              });
                    for (auto& [val, cnt] : hh) {
                        vals.push_back(std::move(val));
                        cnts.push_back(cnt);
                    }
                }
                off.push_back(static_cast<std::int32_t>(vals.size()));
            }
            const std::int64_t nv = static_cast<std::int64_t>(vals.size());
            std::vector<Series> fields;
            fields.push_back(Series::strings(vals));
            fields.push_back(Series::flat(TypeId::Uint64, cnts.data(), nv));
            out.columns.push_back(Series::list(
                off, Series::structs({"value", "count"}, std::move(fields))));
        } else if (sp.op == AggOp::Corr || sp.op == AggOp::CovarPop ||
                   sp.op == AggOp::CovarSamp || sp.op == AggOp::RegrSlope ||
                   sp.op == AggOp::RegrIntercept || sp.op == AggOp::RegrR2) {
            const int slot = st.spec_co[s];
            std::vector<double> v(static_cast<std::size_t>(ng), 0.0);
            for (std::int64_t g = 0; g < ng && slot >= 0; ++g) {
                const std::size_t cs = static_cast<std::size_t>(g) * st.n_co +
                                       static_cast<std::size_t>(slot);
                v[static_cast<std::size_t>(g)] =
                    co_readout(sp.op, st.co_n[cs], st.co_sx[cs], st.co_sy[cs],
                               st.co_sxx[cs], st.co_syy[cs], st.co_sxy[cs]);
            }
            out.columns.push_back(Series::flat_f64(v.data(), ng));
        } else if (sp.op == AggOp::SetUnion) {
            const int slot = st.spec_set[s];
            std::vector<std::string> v(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g) {
                std::string joined;
                if (slot >= 0) {
                    const AggState::StringSet& gset =
                        st.sets[static_cast<std::size_t>(g) * st.n_set +
                                static_cast<std::size_t>(slot)];
                    std::vector<std::string_view> sorted(gset.begin(),
                                                         gset.end());
                    std::sort(sorted.begin(), sorted.end());
                    for (const std::string_view x : sorted) {
                        if (!joined.empty()) joined.push_back(AGG_SET_SEP);
                        joined += x;
                    }
                }
                v[static_cast<std::size_t>(g)] = std::move(joined);
            }
            out.columns.push_back(Series::strings(v));
        } else if (sp.op == AggOp::Busy || sp.op == AggOp::Concurrency ||
                   sp.op == AggOp::Utilization || sp.op == AggOp::Active) {
            const int slot = st.spec_occ[s];
            std::vector<double> v(static_cast<std::size_t>(ng), 0.0);
            for (std::int64_t g = 0; g < ng; ++g) {
                if (slot < 0) continue;
                const std::size_t b = static_cast<std::size_t>(g) * st.n_occ +
                                      static_cast<std::size_t>(slot);
                const OccResult o =
                    occ_summarize(st.occ_deltas[b], st.occ_total[b],
                                  st.occ_ts[b], st.occ_te[b]);
                double r = 0.0;
                switch (sp.op) {
                    case AggOp::Busy:
                        r = static_cast<double>(o.busy);
                        break;
                    case AggOp::Concurrency:
                        r = o.busy ? static_cast<double>(o.total) /
                                         static_cast<double>(o.busy)
                                   : 0.0;
                        break;
                    case AggOp::Utilization:
                        r = (o.busy && o.span) ? static_cast<double>(o.busy) /
                                                     static_cast<double>(o.span)
                                               : 0.0;
                        break;
                    default:  // Active
                        r = static_cast<double>(o.active);
                }
                v[static_cast<std::size_t>(g)] = r;
            }
            out.columns.push_back(Series::flat_f64(v.data(), ng));
        } else if (sp.op == AggOp::Count) {
            std::vector<std::int64_t> v(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g)
                v[static_cast<std::size_t>(g)] = static_cast<std::int64_t>(
                    st.counts[static_cast<std::size_t>(g)]);
            out.columns.push_back(Series::flat_i64(v.data(), ng));
        } else if (sp.op == AggOp::CountValid) {
            std::vector<std::int64_t> v(static_cast<std::size_t>(ng));
            for (std::int64_t g = 0; g < ng; ++g)
                v[static_cast<std::size_t>(g)] =
                    static_cast<std::int64_t>(fs_at(g).n);
            out.columns.push_back(Series::flat_i64(v.data(), ng));
        } else if (sp.op == AggOp::Sum || sp.op == AggOp::Min ||
                   sp.op == AggOp::Max) {
            auto exact = [&](const FieldStat& f) -> std::int64_t {
                return sp.op == AggOp::Sum   ? f.esum
                       : sp.op == AggOp::Min ? f.emin
                                             : f.emax;
            };
            auto approx = [&](const FieldStat& f) -> double {
                return sp.op == AggOp::Sum   ? f.sum
                       : sp.op == AggOp::Min ? f.min
                                             : f.max;
            };
            // A group with no present value has no minimum or maximum: null,
            // not the accumulator's seed. Its sum is 0, as SQL and pandas
            // report it.
            std::vector<std::uint8_t> valid;
            const std::uint8_t* validity = nullptr;
            if (sp.op != AggOp::Sum) {
                valid.assign(static_cast<std::size_t>((ng + 7) / 8), 0);
                bool any_empty = false;
                for (std::int64_t g = 0; g < ng; ++g) {
                    if (fs_at(g).n > 0)
                        valid[static_cast<std::size_t>(g >> 3)] |=
                            static_cast<std::uint8_t>(1u << (g & 7));
                    else
                        any_empty = true;
                }
                if (any_empty) validity = valid.data();
            }
            if (dom == FieldStatDomain::U64) {
                std::vector<std::uint64_t> v(static_cast<std::size_t>(ng));
                for (std::int64_t g = 0; g < ng; ++g)
                    v[static_cast<std::size_t>(g)] =
                        std::bit_cast<std::uint64_t>(exact(fs_at(g)));
                out.columns.push_back(
                    Series::flat(TypeId::Uint64, v.data(), ng, validity));
            } else if (dom == FieldStatDomain::I64) {
                std::vector<std::int64_t> v(static_cast<std::size_t>(ng));
                for (std::int64_t g = 0; g < ng; ++g)
                    v[static_cast<std::size_t>(g)] = exact(fs_at(g));
                out.columns.push_back(Series::flat_i64(v.data(), ng, validity));
            } else {
                std::vector<double> v(static_cast<std::size_t>(ng));
                for (std::int64_t g = 0; g < ng; ++g)
                    v[static_cast<std::size_t>(g)] = approx(fs_at(g));
                out.columns.push_back(Series::flat_f64(v.data(), ng, validity));
            }
        } else {  // Mean/Var/Std/Skew/Kurt -> Float64
            // Each moment needs that many present values to exist at all
            // (one for a mean, two for a sample variance, three and four for
            // the higher moments); below that the cell is null, not 0.
            const std::uint64_t needed = sp.op == AggOp::Mean   ? 1
                                         : sp.op == AggOp::Var  ? 2
                                         : sp.op == AggOp::Std  ? 2
                                         : sp.op == AggOp::Skew ? 3
                                                                : 4;
            std::vector<double> v(static_cast<std::size_t>(ng));
            std::vector<std::uint8_t> valid(
                static_cast<std::size_t>((ng + 7) / 8), 0);
            bool any_missing = false;
            for (std::int64_t g = 0; g < ng; ++g) {
                const FieldStat& f = fs_at(g);
                double r = 0.0;
                switch (sp.op) {
                    case AggOp::Mean:
                        r = f.mean();
                        break;
                    case AggOp::Var:
                        r = f.variance(true);
                        break;
                    case AggOp::Std:
                        r = f.stddev(true);
                        break;
                    case AggOp::Skew:
                        r = f.skewness();
                        break;
                    default:
                        r = f.kurtosis();
                }
                v[static_cast<std::size_t>(g)] = r;
                if (f.n >= needed)
                    valid[static_cast<std::size_t>(g >> 3)] |=
                        static_cast<std::uint8_t>(1u << (g & 7));
                else
                    any_missing = true;
            }
            out.columns.push_back(Series::flat_f64(
                v.data(), ng, any_missing ? valid.data() : nullptr));
        }
    }

    if (st.has_dyn) {
        // Union of discovered argument names, sorted (std::set) so the dyn
        // column order is deterministic and matches the GroupMap path. Columns
        // are laid out name-outer, reduction-inner.
        std::set<std::string> names;
        for (const std::map<std::string, FieldStat>& gmap : st.dyn_fs)
            for (const auto& [name, fs] : gmap) names.insert(name);
        for (const std::string& name : names) {
            const auto dit = st.dyn_domain.find(name);
            const FieldStatDomain dom =
                dit != st.dyn_domain.end() ? dit->second : FieldStatDomain::I64;
            for (const AggDynSpec& d : st.dyn_specs) {
                out.names.push_back(d.out_prefix + name);
                auto fs_of = [&](std::int64_t g) -> const FieldStat* {
                    const std::map<std::string, FieldStat>& gmap =
                        st.dyn_fs[static_cast<std::size_t>(g)];
                    const auto it = gmap.find(name);
                    return it != gmap.end() ? &it->second : nullptr;
                };
                if (d.op == AggOp::Pct) {
                    std::vector<double> v(static_cast<std::size_t>(ng), 0.0);
                    for (std::int64_t g = 0; g < ng; ++g) {
                        const std::map<std::string, DDSketch>& gsk =
                            st.dyn_sketch[static_cast<std::size_t>(g)];
                        const auto it = gsk.find(name);
                        if (it != gsk.end())
                            v[static_cast<std::size_t>(g)] =
                                it->second.quantile(d.param);
                    }
                    out.columns.push_back(Series::flat_f64(v.data(), ng));
                } else if (d.op == AggOp::Count) {
                    std::vector<std::int64_t> v(static_cast<std::size_t>(ng),
                                                0);
                    for (std::int64_t g = 0; g < ng; ++g)
                        if (const FieldStat* f = fs_of(g))
                            v[static_cast<std::size_t>(g)] =
                                static_cast<std::int64_t>(f->n);
                    out.columns.push_back(Series::flat_i64(v.data(), ng));
                } else if ((d.op == AggOp::Sum || d.op == AggOp::Min ||
                            d.op == AggOp::Max) &&
                           dom != FieldStatDomain::F64) {
                    auto exact = [&](const FieldStat& f) -> std::int64_t {
                        return d.op == AggOp::Sum   ? f.esum
                               : d.op == AggOp::Min ? f.emin
                                                    : f.emax;
                    };
                    if (dom == FieldStatDomain::U64) {
                        std::vector<std::uint64_t> v(
                            static_cast<std::size_t>(ng), 0);
                        for (std::int64_t g = 0; g < ng; ++g)
                            if (const FieldStat* f = fs_of(g))
                                v[static_cast<std::size_t>(g)] =
                                    std::bit_cast<std::uint64_t>(exact(*f));
                        out.columns.push_back(
                            Series::flat(TypeId::Uint64, v.data(), ng));
                    } else {
                        std::vector<std::int64_t> v(
                            static_cast<std::size_t>(ng), 0);
                        for (std::int64_t g = 0; g < ng; ++g)
                            if (const FieldStat* f = fs_of(g))
                                v[static_cast<std::size_t>(g)] = exact(*f);
                        out.columns.push_back(Series::flat_i64(v.data(), ng));
                    }
                } else {
                    std::vector<double> v(static_cast<std::size_t>(ng), 0.0);
                    for (std::int64_t g = 0; g < ng; ++g) {
                        const FieldStat* f = fs_of(g);
                        if (!f) continue;
                        double r = 0.0;
                        switch (d.op) {
                            case AggOp::Sum:
                                r = f->sum;
                                break;
                            case AggOp::Min:
                                r = f->n ? f->min : 0.0;
                                break;
                            case AggOp::Max:
                                r = f->n ? f->max : 0.0;
                                break;
                            case AggOp::SumSq:
                                r = f->sumsq;
                                break;
                            case AggOp::Mean:
                                r = f->mean();
                                break;
                            case AggOp::Var:
                                r = f->variance(true);
                                break;
                            case AggOp::Std:
                                r = f->stddev(true);
                                break;
                            case AggOp::Skew:
                                r = f->skewness();
                                break;
                            case AggOp::Kurt:
                                r = f->kurtosis();
                                break;
                            default:
                                r = 0.0;
                        }
                        v[static_cast<std::size_t>(g)] = r;
                    }
                    out.columns.push_back(Series::flat_f64(v.data(), ng));
                }
            }
        }
    }
    return out;
}

DataFrame agg_finalize(const AggState& st, const std::string& key_name) {
    return agg_finalize(st, std::vector<std::string>{key_name});
}

}  // namespace dftracer::utils::dataframe
