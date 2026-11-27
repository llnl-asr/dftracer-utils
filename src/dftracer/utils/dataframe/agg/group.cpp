// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/platform_compat.h>  // hardware_concurrency
#include <dftracer/utils/dataframe/agg/computed.h>
#include <dftracer/utils/dataframe/agg/detail.h>
#include <dftracer/utils/dataframe/batch_ops.h>     // concat, take
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>  // dftu_series
#include <dftracer/utils/dataframe/internal/column_read.h>  // read_u64
#include <dftracer/utils/dataframe/internal/expr_windows.h>
#include <dftracer/utils/dataframe/kernels/sort.h>          // argsort
#include <dftracer/utils/dataframe/parallel.h>              // parallel_for
#include <dftracer/utils/dstd/concurrent_queue.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>  // row hashing
#include <dftracer/utils/dstd/hash/xxh3.h>        // string key hashing
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <utility>

namespace dftracer::utils::dataframe {

namespace {
std::atomic<std::uint64_t> in_place_finalizes{0};
}  // namespace

std::uint64_t agg_in_place_finalizes() {
    return in_place_finalizes.load(std::memory_order_relaxed);
}

namespace {
constexpr std::int64_t AGG_GRAIN = 1 << 16;
// Groups in a probe of the first rows from which a partitioned driver pays off.
constexpr std::int64_t MANY_GROUPS = 2048;
// Rows the by-key driver folds to decide whether the groups are many; in a
// frame the lanes can hold, the lanes win until the groups reach
// LANE_MANY_GROUPS, which a probe of LANE_ROUTE_PROBE rows estimates.
constexpr std::int64_t LANE_MANY_GROUPS = 10000;
constexpr std::int64_t LANE_ROUTE_PROBE = AGG_GRAIN / 16;
constexpr std::int64_t ROUTE_PROBE = AGG_GRAIN / 4;
}  // namespace

namespace {
bool by_ref(const Series& c) {
    const Encoding e = c.encoding();
    return (e == Encoding::Dictionary || e == Encoding::View) &&
           (c.type() == TypeId::String || c.type() == TypeId::Binary);
}

// A column the drivers below read as it is: FLAT, or a by-reference view the
// accumulator takes without a copy.
bool in_place_keep(const Series& c) {
    return c.encoding() == Encoding::Flat || by_ref(c);
}

Series flat_or_ref(const Series& c, bool key) {
    return c.encoding() == Encoding::Flat || (key && by_ref(c))
               ? c.share()
               : c.materialize();
}

// How many groups n rows hold, from how often a probe of them saw each one
// (Chao1: the groups seen plus those that singletons suggest).
std::size_t estimate_groups(const AggState& probe, std::int64_t n) {
    double f1 = 0, f2 = 0;
    for (const std::uint64_t c : probe.counts) {
        f1 += c == 1;
        f2 += c == 2;
    }
    const double seen = static_cast<double>(probe.ngroups());
    return static_cast<std::size_t>(
        std::min(static_cast<double>(n), seen + f1 * f1 / (2.0 * (f2 + 1))));
}

// The groups `rows` of the frame are expected to show when it holds `groups`
// in all (uniformly drawn, with a margin).
std::size_t lane_groups(std::size_t groups, std::int64_t rows) {
    const auto all = std::max(static_cast<double>(groups), 1.0);
    const double seen =
        all * (1.0 - std::exp(-static_cast<double>(rows) / all));
    return static_cast<std::size_t>(seen * 1.05) + 16;
}

// The first error a parallel pass reports; the rest are dropped.
struct FirstError {
    std::mutex mutex;
    dstd::Optional<dstd::Error> error;
    void note(dstd::Result<void>&& r) {
        if (r.has_value()) return;
        const std::lock_guard<std::mutex> lock(mutex);
        if (!error) error = r.error();
    }
};

// The computed value columns of a group-by, compiled once: `windows` gives
// them for a row window and `slots[j]` is the value index its column j fills.
struct Computed {
    dstd::Optional<ExprWindows> windows;
    dstd::Vector<std::size_t> slots;
    std::int64_t rows = 0;
    bool any() const { return windows.has_value(); }
};

dstd::Result<Computed> compile_computed(const AggComputed* c) {
    Computed out;
    if (c == nullptr) return out;
    dstd::Vector<Expr> roots;
    for (std::size_t i = 0; i < c->roots.size(); ++i)
        if (c->roots[i].valid()) {
            roots.push_back(c->roots[i]);
            out.slots.push_back(i);
        }
    if (roots.empty()) return out;
    dstd::Result<ExprWindows> w = ExprWindows::compile(roots, c->inputs);
    if (!w) return w.error();
    out.windows.emplace(std::move(*w));
    out.rows = c->inputs.empty() ? 0 : c->inputs[0]->length();
    return out;
}

// The rows a driver folds: the keys', else a value column's, else the
// computed columns'.
std::int64_t fold_rows(dstd::Span<const Series* const> keys,
                       dstd::Span<const Series* const> values,
                       const Computed* cw) {
    if (!keys.empty()) return keys[0]->length();
    for (const Series* v : values)
        if (v != nullptr) return v->length();
    return cw != nullptr ? cw->rows : 0;
}

// Rows [b, e) of the keys and values as flat columns of e - b rows, the
// computed values evaluated for those rows alone.
struct Window {
    dstd::Vector<Series> owned;
    dstd::Vector<const Series*> keys;
    dstd::Vector<const Series*> values;
};

dstd::Result<void> load_window(dstd::Span<const Series* const> keys,
                               dstd::Span<const Series* const> values,
                               const Computed& cw, std::int64_t b,
                               std::int64_t e, Window& w) {
    dstd::Result<dstd::Vector<Series>> made = cw.windows->eval(b, e - b);
    if (!made) return made.error();
    w.owned.clear();
    w.owned.reserve(keys.size() + values.size());
    w.keys.clear();
    w.values.assign(values.size(), nullptr);
    for (const Series* k : keys) {
        w.owned.push_back(flat_or_ref(k->slice(b, e - b), true));
        w.keys.push_back(&w.owned.back());
    }
    for (std::size_t j = 0; j < cw.slots.size(); ++j) {
        w.owned.push_back(flat_or_ref((*made)[j], false));
        w.values[cw.slots[j]] = &w.owned.back();
    }
    for (std::size_t i = 0; i < values.size(); ++i)
        if (w.values[i] == nullptr) {
            w.owned.push_back(flat_or_ref(values[i]->slice(b, e - b), false));
            w.values[i] = &w.owned.back();
        }
    return {};
}
}  // namespace

static dstd::Result<AggStatePtr> group_agg_lanes(
    dstd::Span<const Series* const> in_keys,
    dstd::Span<const Series* const> in_values, dstd::Vector<AggSpec> specs,
    std::atomic<bool>* many = nullptr, const Computed* cw = nullptr) {
    // The accumulator reads FLAT columns; a view (a filter's selection, a
    // dictionary) is materialized once here, for every caller.
    dstd::Vector<Series> owned;
    owned.reserve(in_keys.size() + in_values.size());
    auto flat = [&](dstd::Span<const Series* const> cols) {
        dstd::Vector<const Series*> out;
        out.reserve(cols.size());
        for (const Series* c : cols) {
            if (c == nullptr || c->encoding() == Encoding::Flat ||
                (&cols == &in_keys && by_ref(*c))) {
                out.push_back(c);
            } else {
                owned.push_back(c->materialize());
                out.push_back(&owned.back());
            }
        }
        return out;
    };
    const dstd::Vector<const Series*> keys = flat(in_keys);
    const dstd::Vector<const Series*> values = flat(in_values);
    const bool computed = cw != nullptr && cw->any();
    const std::int64_t n = fold_rows(keys, values, cw);
    // A UDAF that two threads must not feed at once takes this single state.
    if (n <= AGG_GRAIN || agg_specs_serial(specs)) {
        auto st = agg_new(std::move(specs));
        if (!computed) {
            dstd::Result<void> done = agg_accumulate(*st, keys, values);
            if (!done) return done.error();
            return st;
        }
        // One chunk of computed values at a time, in row order.
        Window w;
        std::int64_t b = 0;
        do {
            const std::int64_t e = std::min(n, b + AGG_GRAIN);
            if (dstd::Result<void> ok = load_window(keys, values, *cw, b, e, w);
                !ok)
                return ok.error();
            agg_set_row_base(*st, b);
            if (dstd::Result<void> done = agg_accumulate(*st, w.keys, w.values);
                !done)
                return done.error();
            b = e;
        } while (b < n);
        agg_set_row_base(*st, 0);
        return st;
    }
    // Fixed lanes, not a partial per thread: lane l folds chunks l, l+LANES,
    // ... in row order and the lanes merge in lane order, so the association
    // of a float sum is the same on every run and for every thread count.
    // Every lane re-discovers every group, as the per-thread partials did.
    constexpr std::int64_t LANES = 16;
    constexpr std::int64_t LANE_PROBE = AGG_GRAIN / 8;
    constexpr std::int64_t LANE_STEP = AGG_GRAIN / 4;
    const std::int64_t chunks = (n + AGG_GRAIN - 1) / AGG_GRAIN;
    const std::int64_t nl = std::min(LANES, chunks);
    dstd::Vector<AggStatePtr> lanes(static_cast<std::size_t>(nl));
    FirstError failed;
    parallel_for(nl, 1, [&](std::int64_t l0, std::int64_t l1) {
        for (std::int64_t l = l0; l < l1; ++l) {
            AggStatePtr st = agg_new(specs);
            st->rows_hint = AGG_GRAIN;
            std::int64_t lane_rows = 0;
            for (std::int64_t c = l; c < chunks; c += nl)
                lane_rows += std::min(n, (c + 1) * AGG_GRAIN) - c * AGG_GRAIN;
            Window w;
            for (std::int64_t c = l; c < chunks; c += nl) {
                std::int64_t b = c * AGG_GRAIN;
                const std::int64_t e = std::min(n, (c + 1) * AGG_GRAIN);
                // Computed values exist for this chunk's rows alone, which
                // the state then counts from its first row.
                const std::int64_t at = computed ? b : 0;
                if (computed) {
                    dstd::Result<void> ok =
                        load_window(keys, values, *cw, b, e, w);
                    if (!ok) {
                        failed.note(std::move(ok));
                        return;
                    }
                    agg_set_row_base(*st, b);
                }
                // The lane's first rows size its tables, so they are
                // allocated once and not regrown (and freed) as the groups
                // come in; lane 0's also tell whether the groups are many.
                const bool first = c == l;
                const bool decides = first && l == 0 && many != nullptr;
                const std::int64_t piece = decides ? LANE_ROUTE_PROBE
                                           : first ? LANE_PROBE
                                                   : e - b;
                // A lane that can be dropped folds in pieces and looks.
                const std::int64_t step = many != nullptr ? LANE_STEP : e - b;
                while (b < e) {
                    if (many != nullptr &&
                        many->load(std::memory_order_relaxed))
                        return;
                    const std::int64_t pe = std::min(
                        e, b + (b == c * AGG_GRAIN && first ? piece : step));
                    dstd::Result<void> done =
                        computed ? agg_accumulate(*st, w.keys, w.values, b - at,
                                                  pe - at)
                                 : agg_accumulate(*st, keys, values, b, pe);
                    if (!done) {
                        failed.note(std::move(done));
                        return;
                    }
                    if (first && b == c * AGG_GRAIN) {
                        const std::size_t all = estimate_groups(*st, n);
                        if (decides &&
                            all >= static_cast<std::size_t>(LANE_MANY_GROUPS)) {
                            many->store(true, std::memory_order_relaxed);
                            return;
                        }
                        if (all >= static_cast<std::size_t>(MANY_GROUPS))
                            st->reserve_groups(lane_groups(all, lane_rows));
                    }
                    b = pe;
                }
            }
            agg_set_row_base(*st, 0);
            lanes[static_cast<std::size_t>(l)] = std::move(st);
        }
    });
    if (failed.error) return *failed.error;
    if (many != nullptr && many->load()) return AggStatePtr();
    // The merged groups go back into the order the frame first shows them.
    AggStatePtr merged = agg_in_first_seen_order(agg_merge_many(lanes));
    if (!merged) return agg_new(std::move(specs));
    if (dstd::Result<void> checked = agg_check(*merged); !checked)
        return checked.error();
    return merged;
}

dstd::Result<AggStatePtr> group_agg_state(
    dstd::Span<const Series* const> in_keys,
    dstd::Span<const Series* const> in_values, dstd::Vector<AggSpec> specs) {
    return group_agg_lanes(in_keys, in_values, std::move(specs));
}

namespace {

// Partition results (disjoint groups) joined, the groups back in the order the
// frame first shows them; `first[p]` holds the original first row of each of
// partition p's groups. Nullopt when no partition has a group.
dstd::Optional<DataFrame> stitch_in_first_seen_order(
    dstd::Vector<DataFrame>& parts,
    dstd::Vector<dstd::Vector<std::int64_t>>& first, std::size_t drop_keys) {
    // A pass whose caller already has the key columns leaves them out: they
    // are not joined and put in order only to be thrown away.
    if (drop_keys != 0)
        for (DataFrame& d : parts)
            if (d.columns.size() >= drop_keys) {
                d.names.erase(
                    d.names.begin(),
                    d.names.begin() + static_cast<std::ptrdiff_t>(drop_keys));
                d.columns.erase(
                    d.columns.begin(),
                    d.columns.begin() + static_cast<std::ptrdiff_t>(drop_keys));
            }
    dstd::Vector<std::size_t> live;
    dstd::Vector<std::int64_t> first_all;
    for (std::size_t p = 0; p < parts.size(); ++p) {
        if (parts[p].columns.empty()) continue;
        live.push_back(p);
        first_all.insert(first_all.end(), first[p].begin(), first[p].end());
    }
    if (live.empty()) return dstd::nullopt;
    const Series first_col = Series::flat_i64(
        first_all.data(), static_cast<std::int64_t>(first_all.size()));
    const Series order = argsort(first_col, false);
    // One column at a time: the partitions' copies of it joined and put in
    // order, then dropped, so what is held beside the result is a column or
    // two and not a second and third copy of the whole output.
    DataFrame out;
    const std::size_t ncols = parts[live[0]].columns.size();
    for (std::size_t c = 0; c < ncols; ++c) {
        dstd::Vector<DataFrame> ones;
        ones.reserve(live.size());
        for (const std::size_t p : live) {
            DataFrame one;
            one.names.push_back(parts[p].names[c]);
            one.columns.push_back(std::move(parts[p].columns[c]));
            ones.push_back(std::move(one));
        }
        dstd::Vector<const DataFrame*> ptrs;
        ptrs.reserve(ones.size());
        for (const DataFrame& one : ones) ptrs.push_back(&one);
        auto joined = concat(ptrs, ConcatHow::Vertical);
        if (!joined)
            dstd::fatal("group_by: partition columns differ in type",
                        joined.error().c_str());
        DataFrame cat = std::move(*joined);
        ones.clear();
        DataFrame ordered = take(cat, order);
        out.names.push_back(std::move(ordered.names[0]));
        out.columns.push_back(std::move(ordered.columns[0]));
    }
    return out;
}

// The bytes of one cell of a flat numeric column the in-place finalize can
// copy by width; 0 for a type it leaves to the stitch path.
std::size_t scatter_width(TypeId t) {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Uint8:
            return 1;
        case TypeId::Int16:
        case TypeId::Uint16:
            return 2;
        case TypeId::Int32:
        case TypeId::Uint32:
        case TypeId::Float32:
            return 4;
        case TypeId::Int64:
        case TypeId::Uint64:
        case TypeId::Float64:
            return 8;
        default:
            return 0;
    }
}

// A key column the in-place finalize writes from the states: a String, or an
// integer that finalize writes as Int64 or Uint64. Other keys (temporal,
// decimal, binary, json) take the stitch path.
enum class KeyKind { None, Str, I64, U64 };

KeyKind key_kind(const AggState& st, std::size_t k) {
    if (st.key_is_bytes[k]) {
        const TypeId kt =
            k < st.key_type.size() ? st.key_type[k] : TypeId::String;
        const bool json = k < st.key_json.size() && st.key_json[k];
        return kt == TypeId::String && !json ? KeyKind::Str : KeyKind::None;
    }
    const FieldStatDomain kd =
        k < st.key_domain.size() ? st.key_domain[k] : FieldStatDomain::I64;
    const TypeId kt = k < st.key_type.size() ? st.key_type[k] : TypeId::Int64;
    if (kd == FieldStatDomain::U64) return KeyKind::U64;
    if (kd == FieldStatDomain::I64 &&
        (kt == TypeId::Int8 || kt == TypeId::Int16 || kt == TypeId::Int32 ||
         kt == TypeId::Int64))
        return KeyKind::I64;
    return KeyKind::None;
}

// The partition results written straight into the output. Every partition
// holds disjoint groups, so once all have folded, each group's final row is
// known (its rank by first row); the output columns are allocated once at full
// size and each partition's cells are copied to their rows as it is finalized.
// What is held beside the output is one partition's small columns per thread,
// not every partition's columns at once, and there is no concat or reorder
// copy. The values are the ones finalize writes: a partition's columns come
// from the same code, only their destination differs. Nullopt, with the
// states untouched, for a shape it does not write: a key that is not a string
// or a plain integer, a column that is not a flat number, dynamic columns.
dstd::Optional<DataFrame> finalize_in_place(
    dstd::Vector<AggStatePtr>& states, dstd::Span<const dstd::String> key_names,
    std::size_t drop_keys) {
    if constexpr (std::endian::native != std::endian::little)
        return dstd::nullopt;  // the validity words are written as words
    if (common::Env::get("DFTRACER_UTILS_GROUPBY_STITCH"))
        return dstd::nullopt;  // a test switch: the stitch path as the oracle

    dstd::Vector<std::size_t> live;   // partitions that hold groups
    dstd::Vector<std::int64_t> base;  // first group index of each live one
    std::int64_t G = 0;
    for (std::size_t p = 0; p < states.size(); ++p) {
        const AggState* st = states[p].get();
        if (st == nullptr || st->ngroups() == 0) continue;
        if (st->has_dyn || st->has_udaf || !agg_first_rows_known(*st))
            return dstd::nullopt;
        live.push_back(p);
        base.push_back(G);
        G += st->ngroups();
    }
    if (live.empty() || G > INT32_MAX) return dstd::nullopt;
    const AggState& s0 = *states[live[0]];

    // Keys: the same kind in every partition, and a string column's bytes
    // must fit 32-bit offsets.
    dstd::Vector<KeyKind> kinds;
    if (drop_keys == 0) {
        if (s0.nkeys != key_names.size()) return dstd::nullopt;
        for (std::size_t k = 0; k < s0.nkeys; ++k) {
            const KeyKind kk = key_kind(s0, k);
            if (kk == KeyKind::None) return dstd::nullopt;
            std::uint64_t bytes = 0;
            for (const std::size_t p : live) {
                const AggState& st = *states[p];
                if (st.nkeys != s0.nkeys || key_kind(st, k) != kk)
                    return dstd::nullopt;
                if (kk == KeyKind::Str)
                    for (const dstd::String& v : st.skey_cols[k])
                        bytes += v.size();
            }
            if (bytes > static_cast<std::uint64_t>(INT32_MAX))
                return dstd::nullopt;
            kinds.push_back(kk);
        }
    }

    // The aggregate columns' schema, from the first partition's cells.
    DataFrame piece0 = agg_finalize_values(s0);
    const std::size_t ncols = piece0.columns.size();
    dstd::Vector<std::size_t> width(ncols);
    dstd::Vector<TypeId> ctype(ncols);
    for (std::size_t c = 0; c < ncols; ++c) {
        const dftu_series* h = piece0.columns[c].handle();
        width[c] = scatter_width(h->type);
        if (width[c] == 0 || h->encoding != Encoding::Flat ||
            h->offsets.block() || !h->nested.empty())
            return dstd::nullopt;
        ctype[c] = h->type;
    }

    // Each group's final row: the rank of its first row among all groups.
    dstd::Vector<std::int64_t> first_all;
    first_all.reserve(static_cast<std::size_t>(G));
    dstd::Vector<std::uint16_t> li_of(static_cast<std::size_t>(G));
    for (std::size_t li = 0; li < live.size(); ++li) {
        const AggState& st = *states[live[li]];
        first_all.insert(first_all.end(), st.group_first_row.begin(),
                         st.group_first_row.end());
        std::fill_n(li_of.begin() + base[li], st.ngroups(),
                    static_cast<std::uint16_t>(li));
    }
    dstd::Vector<std::int32_t> order(static_cast<std::size_t>(G));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::int32_t a, std::int32_t b) {
        return first_all[static_cast<std::size_t>(a)] <
               first_all[static_cast<std::size_t>(b)];
    });
    dstd::Vector<std::int32_t> rank(static_cast<std::size_t>(G));
    for (std::int64_t i = 0; i < G; ++i)
        rank[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])] =
            static_cast<std::int32_t>(i);
    first_all = {};

    DataFrame out;
    // A validity bitmap for output rows, all valid, with `clear` marking nulls.
    const auto validity_bytes = static_cast<std::size_t>((G + 7) / 8);
    // Key columns, from the states, in output order.
    for (std::size_t k = 0; k < kinds.size(); ++k) {
        auto* h = new dftu_series();
        Series col{h};
        h->length = G;
        dstd::Vector<std::uint8_t> null_row;  // output row is a null key
        std::int64_t nulls = 0;
        auto resolve = [&](std::int64_t i, const AggState*& st,
                           std::int64_t& g) {
            const std::int32_t idx = order[static_cast<std::size_t>(i)];
            const std::size_t li = li_of[static_cast<std::size_t>(idx)];
            st = states[live[li]].get();
            g = idx - base[li];
        };
        if (kinds[k] == KeyKind::Str) {
            h->type = TypeId::String;
            h->offsets = new_buffer(static_cast<std::size_t>(G + 1) *
                                    sizeof(std::int32_t));
            auto* off = reinterpret_cast<std::int32_t*>(writable(h->offsets));
            std::int64_t total = 0;
            off[0] = 0;
            for (std::int64_t i = 0; i < G; ++i) {
                const AggState* st;
                std::int64_t g;
                resolve(i, st, g);
                total += static_cast<std::int64_t>(
                    st->skey_cols[k][static_cast<std::size_t>(g)].size());
                Series::check_string_bytes(static_cast<std::size_t>(total));
                off[i + 1] = static_cast<std::int32_t>(total);
            }
            h->data = new_buffer(static_cast<std::size_t>(total));
            std::uint8_t* bytes = writable(h->data);
            parallel_for(G, 1 << 14, [&](std::int64_t b, std::int64_t e) {
                for (std::int64_t i = b; i < e; ++i) {
                    const AggState* st;
                    std::int64_t g;
                    resolve(i, st, g);
                    const dstd::String& v =
                        st->skey_cols[k][static_cast<std::size_t>(g)];
                    if (!v.empty())
                        std::memcpy(bytes + off[i], v.data(), v.size());
                }
            });
        } else {
            h->type = kinds[k] == KeyKind::U64 ? TypeId::Uint64 : TypeId::Int64;
            h->data = new_buffer(static_cast<std::size_t>(G) * 8);
            auto* dst = reinterpret_cast<std::int64_t*>(writable(h->data));
            parallel_for(G, 1 << 14, [&](std::int64_t b, std::int64_t e) {
                for (std::int64_t i = b; i < e; ++i) {
                    const AggState* st;
                    std::int64_t g;
                    resolve(i, st, g);
                    dst[i] = st->ikey_cols[k][static_cast<std::size_t>(g)];
                }
            });
        }
        // Null key groups, as finalize marks them.
        for (std::int64_t i = 0; i < G; ++i) {
            const AggState* st;
            std::int64_t g;
            resolve(i, st, g);
            if (st->nkey_cols[k][static_cast<std::size_t>(g)] == 0) continue;
            if (null_row.empty()) null_row.assign(validity_bytes, 0xFF);
            null_row[static_cast<std::size_t>(i >> 3)] &=
                static_cast<std::uint8_t>(~(1u << (i & 7)));
            ++nulls;
        }
        if (nulls > 0) {
            h->validity = new_buffer(validity_bytes);
            std::memcpy(writable(h->validity), null_row.data(), validity_bytes);
            h->null_count = nulls;
        }
        out.names.push_back(dstd::SharedString{key_names[k]});
        out.columns.push_back(std::move(col));
    }

    // Aggregate columns: allocated once, filled partition by partition.
    dstd::Vector<dftu_series*> handle(ncols);
    dstd::Vector<std::uint8_t*> dst(ncols);
    const auto words = static_cast<std::size_t>((G + 63) / 64);
    dstd::Vector<std::atomic<std::uint64_t*>> vbits(ncols);
    dstd::Vector<std::mutex> vmu(ncols);
    dstd::Vector<std::atomic<std::int64_t>> nulls_of(ncols);
    for (std::size_t c = 0; c < ncols; ++c) {
        auto* h = new dftu_series();
        Series col{h};
        h->type = ctype[c];
        h->length = G;
        h->data = new_buffer(static_cast<std::size_t>(G) * width[c]);
        handle[c] = h;
        dst[c] = writable(h->data);
        vbits[c].store(nullptr);
        nulls_of[c].store(0);
        out.names.push_back(piece0.names[c]);
        out.columns.push_back(std::move(col));
    }
    auto validity_of = [&](std::size_t c) {
        std::uint64_t* b = vbits[c].load(std::memory_order_acquire);
        if (b != nullptr) return b;
        const std::lock_guard<std::mutex> lock(vmu[c]);
        b = vbits[c].load(std::memory_order_relaxed);
        if (b == nullptr) {
            handle[c]->validity = new_buffer(words * 8);
            std::memset(writable(handle[c]->validity), 0xFF, words * 8);
            b = reinterpret_cast<std::uint64_t*>(writable(handle[c]->validity));
            vbits[c].store(b, std::memory_order_release);
        }
        return b;
    };
    auto scatter = [&](std::size_t li, DataFrame& piece) {
        const std::int64_t at = base[li];
        const std::int64_t ng = states[live[li]]->ngroups();
        if (piece.columns.size() != ncols)
            dstd::fatal("group_by: partition columns differ");
        for (std::size_t c = 0; c < ncols; ++c) {
            const dftu_series* ph = piece.columns[c].handle();
            if (ph->type != ctype[c] || ph->length != ng ||
                ph->encoding != Encoding::Flat || ph->offsets.block())
                dstd::fatal("group_by: partition columns differ");
            const std::size_t w = width[c];
            const std::uint8_t* src =
                ph->data.block() ? ph->data.data() : nullptr;
            std::uint8_t* out_c = dst[c];
            if (w == 8) {
                for (std::int64_t g = 0; g < ng; ++g)
                    std::memcpy(
                        out_c + static_cast<std::size_t>(
                                    rank[static_cast<std::size_t>(at + g)]) *
                                    8,
                        src + static_cast<std::size_t>(g) * 8, 8);
            } else {
                for (std::int64_t g = 0; g < ng; ++g)
                    std::memcpy(
                        out_c + static_cast<std::size_t>(
                                    rank[static_cast<std::size_t>(at + g)]) *
                                    w,
                        src + static_cast<std::size_t>(g) * w, w);
            }
            if (!ph->validity.block()) continue;
            std::uint64_t* bits = validity_of(c);
            const std::uint8_t* pv = ph->validity.data();
            std::int64_t nulls = 0;
            for (std::int64_t g = 0; g < ng; ++g) {
                if ((pv[g >> 3] >> (g & 7)) & 1) continue;
                const auto r = static_cast<std::uint64_t>(
                    rank[static_cast<std::size_t>(at + g)]);
                // The builtin, not std::atomic_ref: Apple's libc++ of the
                // wheel toolchain does not have it.
                __atomic_fetch_and(&bits[r >> 6],
                                   ~(std::uint64_t{1} << (r & 63)),
                                   __ATOMIC_RELAXED);
                ++nulls;
            }
            nulls_of[c].fetch_add(nulls, std::memory_order_relaxed);
        }
    };
    scatter(0, piece0);
    piece0 = DataFrame{};
    states[live[0]].reset();  // frees the settled moments
    parallel_for(static_cast<std::int64_t>(live.size()), 1,
                 [&](std::int64_t l0, std::int64_t l1) {
                     for (std::int64_t li = l0; li < l1; ++li) {
                         if (li == 0) continue;
                         DataFrame piece =
                             agg_finalize_values(*states[live[li]]);
                         scatter(static_cast<std::size_t>(li), piece);
                         states[live[li]].reset();
                     }
                 });
    for (std::size_t c = 0; c < ncols; ++c) {
        const std::int64_t nulls = nulls_of[c].load();
        if (nulls == 0)
            handle[c]->validity = {};  // a bitmap with no null in it
        handle[c]->null_count = nulls;
    }
    in_place_finalizes.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// Aggregates whose bits do not depend on the order rows reach a group: count,
// min, max and an integer sum. The packed driver's pages arrive in no fixed
// order, so it takes only these; a float sum or a mean goes to the by-key
// driver, which folds a group's rows in row order.
bool order_free(dstd::Span<const AggSpec> specs,
                dstd::Span<const Series* const> values) {
    for (const AggSpec& sp : specs) {
        switch (sp.op) {
            case AggOp::Count:
            case AggOp::CountValid:
            case AggOp::Min:
            case AggOp::Max:
                break;
            case AggOp::Sum:
                if (sp.value_col < 0) return false;
                switch (
                    values[static_cast<std::size_t>(sp.value_col)]->type()) {
                    case TypeId::Int8:
                    case TypeId::Int16:
                    case TypeId::Int32:
                    case TypeId::Int64:
                    case TypeId::Uint8:
                    case TypeId::Uint16:
                    case TypeId::Uint32:
                    case TypeId::Uint64:
                        break;
                    default:
                        return false;
                }
                break;
            default:
                return false;
        }
    }
    return true;
}

// Many groups on a string key: each chunk scatters its rows (row and
// hash, key words, values) into a page per partition by hash, and a full
// page goes to the partition's queue, where the thread that filled it (or
// whoever holds the partition) folds it into the partition's state, whose
// table fits the first-level cache; no barrier between packing and
// aggregating, and the pages in flight stay few. The groups go back into
// first-seen order. Nullopt when the shape does not pack or the groups
// are few, where the chunked partials are cheaper than the scatter.
dstd::Optional<DataFrame> group_agg_partitioned(
    dstd::Span<const Series* const> in_keys,
    dstd::Span<const Series* const> in_values, dstd::Span<const AggSpec> specs,
    dstd::Span<const dstd::String> key_names, std::size_t drop_keys = 0) {
    // Four partitions per thread: tables small enough for the first-level
    // cache, and the final drain's rounds divide the thread count.
    const auto threads = static_cast<std::int64_t>(std::max<std::size_t>(
        2, dftracer::utils::common::hardware_concurrency()));
    const auto PARTITIONS = static_cast<std::size_t>(4 * threads);
    const std::int64_t n = in_keys.empty() ? 0 : in_keys[0]->length();
    if (n <= 16 * AGG_GRAIN) return dstd::nullopt;
    if (!order_free(specs, in_values)) return dstd::nullopt;
    if (std::none_of(in_keys.begin(), in_keys.end(), [](const Series* c) {
            switch (c->type()) {
                case TypeId::String:
                case TypeId::Binary:
                case TypeId::LargeString:
                case TypeId::LargeBinary:
                    return true;
                default:
                    return false;
            }
        }))
        return dstd::nullopt;
    dstd::Vector<Series> owned;
    owned.reserve(in_keys.size() + in_values.size());
    auto flat = [&](dstd::Span<const Series* const> cols, bool key) {
        dstd::Vector<const Series*> out;
        for (const Series* c : cols) {
            owned.push_back(flat_or_ref(*c, key));
            out.push_back(&owned.back());
        }
        return out;
    };
    const dstd::Vector<const Series*> keys = flat(in_keys, true);
    const dstd::Vector<const Series*> values = flat(in_values, false);
    AggPacked shape;
    AggStatePtr probe =
        agg_new(dstd::Vector<AggSpec>(specs.begin(), specs.end()));
    // Integer keys have the direct table, which the scatter cannot beat.
    if (!agg_pack_shape(*probe, keys, values, shape) || !shape.strings)
        return dstd::nullopt;
    // A rejected layout is reported by the plain driver, which runs next.
    if (!agg_accumulate(*probe, keys, values, 0, AGG_GRAIN / 4))
        return dstd::nullopt;
    if (probe->ngroups() < MANY_GROUPS) return dstd::nullopt;

    // No barriers: a thread packs its chunk into a page per partition and,
    // when a page fills, queues it on the partition and drains that queue
    // into the partition's state if no one else holds it (else the holder
    // will). The pages in flight are the ones filled while a holder was
    // busy, so memory is the partial pages plus little.
    struct Partition {
        std::atomic<bool> held{false};  // the state's owner
        dstd::ConcurrentQueue<dstd::UniquePtr<std::uint64_t[]>> queue;
        AggStatePtr state;
    };
    dstd::Vector<Partition> partitions(PARTITIONS);
    for (Partition& part : partitions)
        part.state = agg_new(dstd::Vector<AggSpec>(specs.begin(), specs.end()));
    const std::size_t per_page = AggPages::PAGE_WORDS / shape.stride;
    auto drain = [&](std::size_t p) {
        Partition& part = partitions[p];
        if (part.held.exchange(true, std::memory_order_acquire)) return;
        for (;;) {
            AggPages view;
            view.open(shape.stride);
            view.pages.resize(64);
            const std::size_t got =
                part.queue.try_dequeue_bulk(view.pages.begin(), 64);
            if (got == 0) break;
            view.pages.resize(got);
            view.count = got * per_page;
            agg_accumulate_packed(*part.state, keys, values, shape, view);
        }
        part.held.store(false, std::memory_order_release);
    };
    using Pages = dstd::Vector<AggPages>;
    dstd::Vector<dstd::UniquePtr<Pages>> pool;
    std::mutex pool_mutex;
    std::atomic<bool> packed{true};
    parallel_for(n, AGG_GRAIN, [&](std::int64_t b, std::int64_t e) {
        if (!packed.load(std::memory_order_relaxed)) return;
        dstd::UniquePtr<Pages> pages;
        {
            const std::lock_guard<std::mutex> lock(pool_mutex);
            if (!pool.empty()) {
                pages = std::move(pool.back());
                pool.pop_back();
            }
        }
        if (!pages) pages = dstd::make_unique<Pages>(PARTITIONS);
        auto full = [&](std::size_t p) {
            for (auto& page : (*pages)[p].release())
                partitions[p].queue.enqueue(std::move(page));
            drain(p);
        };
        if (!agg_pack(*probe, keys, values, b, e, shape, *pages, full))
            packed.store(false, std::memory_order_relaxed);
        const std::lock_guard<std::mutex> lock(pool_mutex);
        pool.push_back(std::move(pages));
    });
    if (!packed.load()) return dstd::nullopt;
    // The partial pages and whatever is still queued, one partition per
    // task, no holder to wait for.
    dstd::Vector<AggStatePtr> states(PARTITIONS);
    parallel_for(static_cast<std::int64_t>(PARTITIONS), 1,
                 [&](std::int64_t p0, std::int64_t p1) {
                     for (std::int64_t p = p0; p < p1; ++p) {
                         const auto pi = static_cast<std::size_t>(p);
                         drain(pi);
                         for (const dstd::UniquePtr<Pages>& pages : pool) {
                             AggPages& rest = (*pages)[pi];
                             if (rest.count > 0)
                                 agg_accumulate_packed(*partitions[pi].state,
                                                       keys, values, shape,
                                                       rest);
                             rest.pages.clear();
                         }
                         states[pi] = std::move(partitions[pi].state);
                     }
                 });
    pool.clear();
    if (dstd::Optional<DataFrame> out =
            finalize_in_place(states, key_names, drop_keys))
        return out;
    dstd::Vector<DataFrame> parts(PARTITIONS);
    dstd::Vector<dstd::Vector<std::int64_t>> first(PARTITIONS);
    parallel_for(static_cast<std::int64_t>(PARTITIONS), 1,
                 [&](std::int64_t p0, std::int64_t p1) {
                     for (std::int64_t p = p0; p < p1; ++p) {
                         const auto pi = static_cast<std::size_t>(p);
                         if (states[pi]->ngroups() == 0) continue;
                         first[pi] = states[pi]->group_first_row;
                         parts[pi] = agg_finalize(*states[pi], key_names);
                         states[pi].reset();  // frees the settled moments
                     }
                 });
    return stitch_in_first_seen_order(parts, first, drop_keys);
}

// Whether the by-key driver can take these keys and aggregates: keys whose
// equality is the equality of their bytes, and no state that depends on the
// order of rows (a tie, a bounded counter), which would change with the order
// the morsels reach a partition.
bool by_key_eligible(dstd::Span<const Series* const> in_keys,
                     dstd::Span<const AggSpec> specs) {
    for (const Series* c : in_keys) {
        switch (c->type()) {
            case TypeId::String:
            case TypeId::Int8:
            case TypeId::Int16:
            case TypeId::Int32:
            case TypeId::Int64:
            case TypeId::Uint8:
            case TypeId::Uint16:
            case TypeId::Uint32:
            case TypeId::Uint64:
                break;
            default:
                return false;
        }
    }
    AggStatePtr probe =
        agg_new(dstd::Vector<AggSpec>(specs.begin(), specs.end()));
    return !(probe->has_arg || probe->has_apos || probe->has_lst ||
             probe->has_ss);
}

// Many groups on a key the packed driver cannot hold (a string over 16 bytes,
// an integer): every row goes to a partition by a hash of its key, a morsel at
// a time, and is folded into a state of that partition, so the groups are
// disjoint, nothing is merged, and the state is one copy of the groups where
// the per-thread partials hold one copy per thread. No array or copy has a
// size in rows beyond the morsels in flight. Each row keeps its original row
// number, so First, Last and the group order hold. Nullopt for a key whose
// equality is not the equality of its bytes (floats, decimals, lists), state
// that depends on the order of rows (arg_max, top-k, space-saving), a small
// frame or few groups, where the per-thread partials are cheaper.
dstd::Result<dstd::Optional<DataFrame>> group_agg_by_key_hash(
    dstd::Span<const Series* const> in_keys,
    dstd::Span<const Series* const> in_values, dstd::Span<const AggSpec> specs,
    dstd::Span<const dstd::String> key_names, std::size_t drop_keys = 0,
    const Computed* cw = nullptr) {
    using Maybe = dstd::Result<dstd::Optional<DataFrame>>;
    auto none = [] { return Maybe(dstd::Optional<DataFrame>()); };
    // Threshold set from benchmarks/groupby_analyzer_bench.py; below it the
    // single state wins. Up to 4 * AGG_GRAIN rows the lanes win until the
    // groups are estimated at LANE_MANY_GROUPS; above, until a probe shows
    // MANY_GROUPS. A group sees its rows in row order, so the bits do not
    // depend on the frame size or the thread count.
    constexpr std::int64_t MIN_ROWS = AGG_GRAIN;
    const std::int64_t n = in_keys.empty() ? 0 : in_keys[0]->length();
    if (n <= MIN_ROWS || agg_specs_serial(specs)) return none();
    if (!by_key_eligible(in_keys, specs)) return none();
    dstd::Vector<Series> owned;
    owned.reserve(in_keys.size() + in_values.size());
    auto flat = [&](dstd::Span<const Series* const> cols, bool key) {
        dstd::Vector<const Series*> out;
        for (const Series* c : cols) {
            if (c == nullptr) {
                out.push_back(nullptr);
                continue;
            }
            owned.push_back(flat_or_ref(*c, key));
            out.push_back(&owned.back());
        }
        return out;
    };
    const dstd::Vector<const Series*> keys = flat(in_keys, true);
    const dstd::Vector<const Series*> values = flat(in_values, false);
    const bool computed = cw != nullptr && cw->any();
    std::size_t groups_hint = 0;
    {
        AggStatePtr probe =
            agg_new(dstd::Vector<AggSpec>(specs.begin(), specs.end()));
        // A rejected layout is reported by the state driver, which runs next.
        const bool lane_range = n <= 4 * AGG_GRAIN;
        const std::int64_t rows =
            std::min(n, lane_range ? LANE_ROUTE_PROBE : ROUTE_PROBE);
        Window w;
        if (computed && !load_window(keys, values, *cw, 0, rows, w))
            return none();
        const bool folded =
            computed
                ? agg_accumulate(*probe, w.keys, w.values).has_value()
                : agg_accumulate(*probe, keys, values, 0, rows).has_value();
        if (!folded) return none();
        const bool few = lane_range
                             ? estimate_groups(*probe, n) <
                                   static_cast<std::size_t>(LANE_MANY_GROUPS)
                             : probe->ngroups() < MANY_GROUPS;
        if (few) return none();
        groups_hint = estimate_groups(*probe, n);
    }

    const auto threads = static_cast<std::int64_t>(std::max<std::size_t>(
        2, dftracer::utils::common::hardware_concurrency()));
    const auto PARTITIONS = static_cast<std::size_t>(4 * threads);
    dstd::Vector<bool> nullable(keys.size());
    for (std::size_t j = 0; j < keys.size(); ++j)
        nullable[j] = keys[j]->null_count() > 0;
    constexpr std::uint64_t NULL_KEY =
        dftracer::utils::dstd::hash::GOLDEN_RATIO;

    // The key and value columns, shared, not copied. A computed value is not
    // in it: each morsel evaluates its rows into `made`.
    DataFrame src;
    for (const Series* c : keys) {
        src.names.push_back(
            dstd::SharedString{dstd::fmt<"k{}">(src.columns.size())});
        src.columns.push_back(c->share());
    }
    dstd::Vector<std::size_t> src_at(values.size(), 0);
    for (std::size_t j = 0; j < values.size(); ++j) {
        if (values[j] == nullptr) continue;
        src_at[j] = src.columns.size();
        src.names.push_back(
            dstd::SharedString{dstd::fmt<"v{}">(src.columns.size())});
        src.columns.push_back(values[j]->share());
    }
    dstd::Vector<std::size_t> made_at(values.size(), 0);
    if (computed)
        for (std::size_t j = 0; j < cw->slots.size(); ++j)
            made_at[cw->slots[j]] = j;
    dstd::Vector<AggStatePtr> states(PARTITIONS);
    for (AggStatePtr& st : states) {
        st = agg_new(dstd::Vector<AggSpec>(specs.begin(), specs.end()));
        // An equal share, and a fifth over for the partitions a hash makes
        // larger than the mean.
        st->groups_hint = groups_hint / PARTITIONS * 6 / 5 + 16;
    }
    // Morsels go through in windows. A window's morsels are hashed together,
    // each into its rows in order of partition, then every partition folds
    // its rows from the window's morsels in morsel order, so a group sees its
    // rows in row order whatever the threads do: the same bits on every run
    // and for every thread count. What is held at once is one window's row
    // numbers and the few thousand rows a partition gathers, not the frame.
    constexpr std::int64_t WINDOW = 4;
    const std::int64_t nmorsels = (n + AGG_GRAIN - 1) / AGG_GRAIN;
    struct Bucketed {
        dstd::Vector<std::uint32_t> rows;  // row - morsel start, by partition
        dstd::Vector<std::int64_t> off;    // PARTITIONS + 1 offsets into rows
    };
    dstd::Vector<Bucketed> win(static_cast<std::size_t>(WINDOW));
    // The computed values of each morsel in the window, its rows from 0.
    dstd::Vector<DataFrame> made(static_cast<std::size_t>(WINDOW));
    auto bucket_morsel = [&](std::int64_t m, Bucketed& out) {
        const std::int64_t b = m * AGG_GRAIN;
        const std::int64_t e = std::min(n, b + AGG_GRAIN);
        dstd::Vector<std::int32_t> bucket(static_cast<std::size_t>(e - b));
        out.off.assign(PARTITIONS + 1, 0);
        for (std::int64_t i = b; i < e; ++i) {
            std::uint64_t h = 0;
            for (std::size_t j = 0; j < keys.size(); ++j) {
                const Series& c = *keys[j];
                std::uint64_t v;
                if (nullable[j] && c.is_null(i))
                    v = NULL_KEY;
                else if (c.type() == TypeId::String)
                    v = dftracer::utils::dstd::hash::xxh3_64(c.string_at(i));
                else
                    v = read_u64(c, i);
                h = dftracer::utils::dstd::hash::splitmix64(h ^ v);
            }
            const auto p =
                static_cast<std::int32_t>(((h >> 32) * PARTITIONS) >> 32);
            bucket[static_cast<std::size_t>(i - b)] = p;
            ++out.off[static_cast<std::size_t>(p) + 1];
        }
        for (std::size_t p = 0; p < PARTITIONS; ++p)
            out.off[p + 1] += out.off[p];
        out.rows.resize(static_cast<std::size_t>(e - b));
        dstd::Vector<std::int64_t> cur(out.off.begin(), out.off.end() - 1);
        for (std::int64_t i = b; i < e; ++i)
            out.rows[static_cast<std::size_t>(cur[static_cast<std::size_t>(
                bucket[static_cast<std::size_t>(i - b)])]++)] =
                static_cast<std::uint32_t>(i - b);
    };
    FirstError failed;
    for (std::int64_t w0 = 0; w0 < nmorsels; w0 += WINDOW) {
        const std::int64_t wn = std::min(WINDOW, nmorsels - w0);
        parallel_for(wn, 1, [&](std::int64_t m0, std::int64_t m1) {
            for (std::int64_t m = m0; m < m1; ++m) {
                bucket_morsel(w0 + m, win[static_cast<std::size_t>(m)]);
                if (!computed) continue;
                const std::int64_t b = (w0 + m) * AGG_GRAIN;
                dstd::Result<dstd::Vector<Series>> cols =
                    cw->windows->eval(b, std::min(n, b + AGG_GRAIN) - b);
                if (!cols) {
                    failed.note(dstd::Result<void>(cols.error()));
                    continue;
                }
                DataFrame& out = made[static_cast<std::size_t>(m)];
                out = DataFrame{};
                for (Series& c : *cols) {
                    out.names.push_back(dstd::SharedString{
                        dstd::fmt<"c{}">(out.columns.size())});
                    out.columns.push_back(flat_or_ref(c, false));
                }
            }
        });
        if (failed.error) return *failed.error;
        parallel_for(
            static_cast<std::int64_t>(PARTITIONS), 1,
            [&](std::int64_t p0, std::int64_t p1) {
                for (std::int64_t p = p0; p < p1; ++p) {
                    const auto pi = static_cast<std::size_t>(p);
                    AggState& st = *states[pi];
                    for (std::int64_t m = 0; m < wn; ++m) {
                        const Bucketed& bk = win[static_cast<std::size_t>(m)];
                        if (bk.off[pi + 1] == bk.off[pi]) continue;
                        const std::int64_t at = (w0 + m) * AGG_GRAIN;
                        dstd::Vector<std::int64_t> mine;
                        mine.reserve(static_cast<std::size_t>(bk.off[pi + 1] -
                                                              bk.off[pi]));
                        for (std::int64_t r = bk.off[pi]; r < bk.off[pi + 1];
                             ++r)
                            mine.push_back(
                                at + bk.rows[static_cast<std::size_t>(r)]);
                        const DataFrame sub = take(src, mine);
                        DataFrame sub_made;
                        if (computed) {
                            dstd::Vector<std::int64_t> local;
                            local.reserve(mine.size());
                            for (const std::int64_t r : mine)
                                local.push_back(r - at);
                            sub_made =
                                take(made[static_cast<std::size_t>(m)], local);
                        }
                        dstd::Vector<const Series*> sk, sv;
                        for (std::size_t j = 0; j < keys.size(); ++j)
                            sk.push_back(&sub.columns[j]);
                        for (std::size_t j = 0; j < values.size(); ++j)
                            sv.push_back(values[j] != nullptr
                                             ? &sub.columns[src_at[j]]
                                             : &sub_made.columns[made_at[j]]);
                        st.row_ids = mine.data();
                        dstd::Result<void> done = agg_accumulate(st, sk, sv);
                        st.row_ids = nullptr;
                        if (!done) {
                            failed.note(std::move(done));
                            return;
                        }
                    }
                }
            });
        if (failed.error) return *failed.error;
    }
    if (dstd::Optional<DataFrame> out =
            finalize_in_place(states, key_names, drop_keys))
        return Maybe(std::move(out));
    dstd::Vector<DataFrame> parts(PARTITIONS);
    dstd::Vector<dstd::Vector<std::int64_t>> first(PARTITIONS);
    std::atomic<bool> ok{true};
    parallel_for(static_cast<std::int64_t>(PARTITIONS), 1,
                 [&](std::int64_t p0, std::int64_t p1) {
                     for (std::int64_t p = p0; p < p1; ++p) {
                         const auto pi = static_cast<std::size_t>(p);
                         AggState& st = *states[pi];
                         if (!st.inited || st.ngroups() == 0) continue;
                         if (!agg_first_rows_known(st)) {
                             ok.store(false, std::memory_order_relaxed);
                             continue;
                         }
                         first[pi] = st.group_first_row;
                         parts[pi] = agg_finalize(st, key_names);
                         failed.note(agg_check(st));
                         states[pi].reset();  // frees the settled moments
                     }
                 });
    if (failed.error) return *failed.error;
    if (!ok.load()) return none();
    return Maybe(stitch_in_first_seen_order(parts, first, drop_keys));
}

// `drop_keys` leaves the first columns (the keys) out of the result: a column
// batch after the first has them already.
dstd::Result<DataFrame> group_agg_whole(
    dstd::Span<const Series* const> keys,
    dstd::Span<const Series* const> values, dstd::Vector<AggSpec> specs,
    dstd::Span<const dstd::String> key_names, std::size_t drop_keys = 0,
    const AggComputed* computed = nullptr) {
    dstd::Result<Computed> compiled = compile_computed(computed);
    if (!compiled) return compiled.error();
    const Computed& cw = *compiled;
    // The packed driver reads whole columns; computed values go to the
    // drivers below, which evaluate them a window at a time.
    if (!cw.any())
        if (dstd::Optional<DataFrame> out = group_agg_partitioned(
                keys, values, specs, key_names, drop_keys))
            return std::move(*out);
    // A frame the lanes can hold is folded in lanes at once, and lane 0's
    // first rows decide whether the groups are many: then the lanes are
    // dropped and the by-key driver runs, else nothing waited.
    const std::int64_t n = keys.empty() ? 0 : keys[0]->length();
    std::atomic<bool> many{false};
    AggStatePtr folded;
    if (n > AGG_GRAIN && n <= 4 * AGG_GRAIN && !agg_specs_serial(specs) &&
        by_key_eligible(keys, specs)) {
        dstd::Result<AggStatePtr> lanes = group_agg_lanes(
            keys, values, dstd::Vector<AggSpec>(specs), &many, &cw);
        if (!lanes) return lanes.error();
        folded = std::move(*lanes);
    }
    if (!folded) {
        dstd::Result<dstd::Optional<DataFrame>> hashed = group_agg_by_key_hash(
            keys, values, specs, key_names, drop_keys, &cw);
        if (!hashed) return hashed.error();
        if (*hashed) return std::move(**hashed);
        dstd::Result<AggStatePtr> lanes =
            group_agg_lanes(keys, values, std::move(specs), nullptr, &cw);
        if (!lanes) return lanes.error();
        folded = std::move(*lanes);
    }
    dstd::Result<AggStatePtr> state = std::move(folded);
    DataFrame whole = agg_finalize(**state, key_names);
    if (dstd::Result<void> checked = agg_check(**state); !checked)
        return checked.error();
    if (drop_keys != 0 && whole.columns.size() >= drop_keys) {
        whole.names.erase(
            whole.names.begin(),
            whole.names.begin() + static_cast<std::ptrdiff_t>(drop_keys));
        whole.columns.erase(
            whole.columns.begin(),
            whole.columns.begin() + static_cast<std::ptrdiff_t>(drop_keys));
    }
    return whole;
}

// Aggregates whose state is a FieldStat of one value column and nothing else.
bool one_column_state(AggOp op) {
    switch (op) {
        case AggOp::Count:
        case AggOp::CountValid:
        case AggOp::Sum:
        case AggOp::Min:
        case AggOp::Max:
        case AggOp::Mean:
        case AggOp::Var:
        case AggOp::Std:
        case AggOp::Skew:
        case AggOp::Kurt:
        case AggOp::SkewUnbiased:
        case AggOp::KurtUnbiased:
        case AggOp::SumSq:
            return true;
        default:
            return false;
    }
}

// Bytes of one (column, group) cell of state for the aggregates over it: a
// light row's words (count, then sum, min, max as asked, and the three moment
// words of a variance or standard deviation), or a whole FieldStat when a
// skew, a kurtosis or a sum of squares needs its higher moments.
std::int64_t cell_bytes(dstd::Span<const AggSpec> specs, std::int32_t col) {
    bool sum = false, min = false, max = false, mom = false;
    for (const AggSpec& sp : specs) {
        if (sp.value_col != col) continue;
        switch (sp.op) {
            case AggOp::Count:
            case AggOp::CountValid:
                break;
            case AggOp::Sum:
            case AggOp::Mean:
                sum = true;
                break;
            case AggOp::Min:
                min = true;
                break;
            case AggOp::Max:
                max = true;
                break;
            case AggOp::Var:
            case AggOp::Std:
                mom = true;
                break;
            default:
                return static_cast<std::int64_t>(sizeof(FieldStat));
        }
    }
    if (!mom) return 32;  // the count, sum, min and max cells
    return static_cast<std::int64_t>(8 * (1 + 3 + static_cast<int>(sum) +
                                          static_cast<int>(min) +
                                          static_cast<int>(max)));
}

// Many groups over many value columns: the state of every (column, group)
// cell would be held at once. The columns are aggregated a few at a time
// instead, each batch its own pass over the keys, so the state held is that of
// the batch, and the groups come out in first-seen order in every pass.
// Nullopt when the aggregates, or the number of groups, do not call for it.
dstd::Result<dstd::Optional<DataFrame>> group_agg_in_batches(
    dstd::Span<const Series* const> keys,
    dstd::Span<const Series* const> values, dstd::Span<const AggSpec> specs,
    dstd::Span<const dstd::String> key_names, const AggComputed* computed) {
    using Maybe = dstd::Result<dstd::Optional<DataFrame>>;
    auto none = [] { return Maybe(dstd::Optional<DataFrame>()); };
    // The state one batch may hold. The passes over the keys grow as columns /
    // batch width.
    constexpr std::int64_t BATCH_STATE_BYTES = 8 << 20;
    constexpr std::size_t FIRST_BATCH = 1;
    const std::int64_t n = keys.empty() ? 0 : keys[0]->length();
    if (n <= 4 * AGG_GRAIN) return none();
    for (const AggSpec& sp : specs)
        if (!one_column_state(sp.op)) return none();
    // The value columns the aggregates read, in order of first use.
    dstd::Vector<std::int32_t> used;
    for (const AggSpec& sp : specs)
        if (sp.value_col >= 0 &&
            std::find(used.begin(), used.end(), sp.value_col) == used.end())
            used.push_back(sp.value_col);
    if (used.size() <= FIRST_BATCH) return none();
    // The count, sum, min and max cells are small next to the output; it is
    // the moments (a FieldStat a cell) that a batch bounds.
    if (std::none_of(used.begin(), used.end(),
                     [&](std::int32_t c) { return cell_bytes(specs, c) > 32; }))
        return none();
    {
        dstd::Vector<Series> owned;
        owned.reserve(keys.size());
        dstd::Vector<const Series*> ks;
        for (const Series* c : keys) {
            owned.push_back(flat_or_ref(*c, true));
            ks.push_back(&owned.back());
        }
        dstd::Result<Computed> cw = compile_computed(computed);
        if (!cw) return cw.error();
        Window w;
        dstd::Vector<Series> owned_values;
        owned_values.reserve(values.size());
        dstd::Vector<const Series*> vs;
        if (cw->any()) {
            if (!load_window(ks, values, *cw, 0, AGG_GRAIN / 4, w))
                return none();
            ks = w.keys;
            vs = w.values;
        } else {
            for (const Series* c : values) {
                owned_values.push_back(flat_or_ref(*c, false));
                vs.push_back(&owned_values.back());
            }
        }
        AggStatePtr probe =
            agg_new(dstd::Vector<AggSpec>(specs.begin(), specs.end()));
        // A rejected layout is reported by the whole-frame driver, which runs
        // next.
        if (!agg_accumulate(*probe, ks, vs, 0, AGG_GRAIN / 4)) return none();
        if (probe->ngroups() < MANY_GROUPS) return none();
    }
    // One batch: the aggregates over `cols`, reading a values list of those
    // columns alone. Count (no column) rides with the first batch.
    auto run = [&](std::size_t from, std::size_t count, bool with_counts) {
        const dstd::Vector<std::int32_t> cols(used.begin() + from,
                                              used.begin() + from + count);
        dstd::Vector<AggSpec> sub;
        for (const AggSpec& sp : specs) {
            AggSpec s = sp;
            if (sp.value_col < 0) {
                if (!with_counts) continue;
            } else {
                const auto at =
                    std::find(cols.begin(), cols.end(), sp.value_col);
                if (at == cols.end()) continue;
                s.value_col = static_cast<std::int32_t>(at - cols.begin());
            }
            sub.push_back(std::move(s));
        }
        dstd::Vector<const Series*> vs;
        dstd::Vector<Expr> roots;
        for (const std::int32_t c : cols) {
            vs.push_back(values[static_cast<std::size_t>(c)]);
            if (computed != nullptr)
                roots.push_back(computed->roots[static_cast<std::size_t>(c)]);
        }
        const AggComputed part{roots, computed != nullptr
                                          ? computed->inputs
                                          : dstd::Span<const Series* const>()};
        return group_agg_whole(keys, vs, std::move(sub), key_names,
                               from == 0 ? 0 : keys.size(),
                               computed != nullptr ? &part : nullptr);
    };
    dstd::Vector<dstd::SharedString> made_names;
    dstd::Vector<Series> made;
    dstd::Vector<Series> key_cols;
    auto keep = [&](DataFrame& df, bool first) {
        for (std::size_t j = 0; j < df.columns.size(); ++j) {
            if (first && j < keys.size()) {
                key_cols.push_back(std::move(df.columns[j]));
            } else {
                made_names.push_back(df.names[j]);
                made.push_back(std::move(df.columns[j]));
            }
        }
    };
    dstd::Result<DataFrame> head_run = run(0, FIRST_BATCH, true);
    if (!head_run) return head_run.error();
    DataFrame head = std::move(*head_run);
    const std::int64_t groups =
        head.columns.empty() ? 0 : head.columns[0].length();
    keep(head, true);
    std::int64_t widest = 0;
    for (const std::int32_t c : used)
        widest = std::max(widest, cell_bytes(specs, c));
    const std::int64_t width = std::max<std::int64_t>(
        1, BATCH_STATE_BYTES / std::max<std::int64_t>(1, groups * widest));
    for (std::size_t at = FIRST_BATCH; at < used.size();) {
        const std::size_t count =
            static_cast<std::size_t>(std::min<std::int64_t>(
                width, static_cast<std::int64_t>(used.size() - at)));
        dstd::Result<DataFrame> next_run = run(at, count, false);
        if (!next_run) return next_run.error();
        DataFrame next = std::move(*next_run);
        keep(next, false);
        at += count;
    }
    // The columns back in the order the aggregates were asked for.
    DataFrame out;
    for (std::size_t k = 0; k < keys.size(); ++k) {
        out.names.push_back(dstd::SharedString{key_names[k]});
        out.columns.push_back(std::move(key_cols[k]));
    }
    for (const AggSpec& sp : specs)
        for (std::size_t j = 0; j < made.size(); ++j)
            if (made_names[j] == sp.out) {
                out.names.push_back(made_names[j]);
                out.columns.push_back(std::move(made[j]));
                break;
            }
    return Maybe(std::move(out));
}

}  // namespace

namespace {

dstd::Result<DataFrame> group_agg_impl(dstd::Span<const Series* const> keys,
                                       dstd::Span<const Series* const> values,
                                       dstd::Vector<AggSpec> specs,
                                       dstd::Span<const dstd::String> key_names,
                                       const AggComputed* computed) {
    dstd::Vector<Series> joined;
    joined.reserve(keys.size() + values.size());
    auto flat = [&](dstd::Span<const Series* const> in)
        -> dstd::Result<dstd::Vector<const Series*>> {
        dstd::Vector<const Series*> out;
        out.reserve(in.size());
        for (const Series* c : in) {
            if (c && c->encoding() == Encoding::Chunked) {
                dstd::Result<Series> j = join_chunks(*c);
                if (!j) return j.error();
                joined.push_back(std::move(*j));
                c = &joined.back();
            }
            // One gather of the whole column here, in place of one per driver
            // below: the accumulator reads FLAT columns.
            if (c && !in_place_keep(*c)) {
                joined.push_back(flat_or_ref(*c, false));
                c = &joined.back();
            }
            out.push_back(c);
        }
        return out;
    };
    const dstd::Result<dstd::Vector<const Series*>> keys_flat = flat(keys);
    if (!keys_flat) return keys_flat.error();
    const dstd::Result<dstd::Vector<const Series*>> values_flat = flat(values);
    if (!values_flat) return values_flat.error();
    const dstd::Vector<const Series*>& flat_keys = *keys_flat;
    const dstd::Vector<const Series*>& flat_values = *values_flat;
    dstd::Result<dstd::Optional<DataFrame>> batched = group_agg_in_batches(
        flat_keys, flat_values, specs, key_names, computed);
    if (!batched) return batched.error();
    if (*batched) return std::move(**batched);
    return group_agg_whole(flat_keys, flat_values, std::move(specs), key_names,
                           0, computed);
}

}  // namespace

dstd::Result<DataFrame> group_agg(dstd::Span<const Series* const> keys,
                                  dstd::Span<const Series* const> values,
                                  dstd::Vector<AggSpec> specs,
                                  dstd::Span<const dstd::String> key_names) {
    return group_agg_impl(keys, values, std::move(specs), key_names, nullptr);
}

dstd::Result<DataFrame> group_agg_computed(
    dstd::Span<const Series* const> keys,
    dstd::Span<const Series* const> values, dstd::Vector<AggSpec> specs,
    dstd::Span<const dstd::String> key_names, const AggComputed& computed) {
    return group_agg_impl(keys, values, std::move(specs), key_names, &computed);
}

dstd::Result<DataFrame> group_agg(const Series& key,
                                  dstd::Span<const Series* const> values,
                                  dstd::Vector<AggSpec> specs,
                                  dstd::StringView key_name) {
    const Series* const key_ptr = &key;
    const dstd::String name(key_name);
    return group_agg(dstd::Span<const Series* const>(&key_ptr, 1), values,
                     std::move(specs),
                     dstd::Span<const dstd::String>(&name, 1));
}

}  // namespace dftracer::utils::dataframe
