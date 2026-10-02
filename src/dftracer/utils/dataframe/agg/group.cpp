#include <concurrentqueue.h>
#include <dftracer/utils/core/common/hash/fnv1a.h>       // string key hashing
#include <dftracer/utils/core/common/hash/splitmix64.h>  // row hashing
#include <dftracer/utils/core/common/platform_compat.h>  // hardware_concurrency
#include <dftracer/utils/core/env.h>
#include <dftracer/utils/dataframe/agg/detail.h>
#include <dftracer/utils/dataframe/batch_ops.h>          // concat, take
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>  // dftu_series
#include <dftracer/utils/dataframe/internal/column_read.h>  // read_u64
#include <dftracer/utils/dataframe/kernels/sort.h>          // argsort
#include <dftracer/utils/dataframe/parallel.h>              // parallel_for

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
}  // namespace

namespace {
bool by_ref(const Series& c) {
    const Encoding e = c.encoding();
    return (e == Encoding::Dictionary || e == Encoding::View) &&
           (c.type() == TypeId::String || c.type() == TypeId::Binary);
}

Series flat_or_ref(const Series& c, bool key) {
    return c.encoding() == Encoding::Flat || (key && by_ref(c))
               ? c.share()
               : c.materialize();
}
}  // namespace

AggStatePtr group_agg_state(const std::vector<const Series*>& in_keys,
                            const std::vector<const Series*>& in_values,
                            std::vector<AggSpec> specs) {
    // The accumulator reads FLAT columns; a view (a filter's selection, a
    // dictionary) is materialized once here, for every caller.
    std::vector<Series> owned;
    owned.reserve(in_keys.size() + in_values.size());
    auto flat = [&](const std::vector<const Series*>& cols) {
        std::vector<const Series*> out;
        out.reserve(cols.size());
        for (const Series* c : cols) {
            if (c->encoding() == Encoding::Flat ||
                (&cols == &in_keys && by_ref(*c))) {
                out.push_back(c);
            } else {
                owned.push_back(c->materialize());
                out.push_back(&owned.back());
            }
        }
        return out;
    };
    const std::vector<const Series*> keys = flat(in_keys);
    const std::vector<const Series*> values = flat(in_values);
    const std::int64_t n = !keys.empty()     ? keys[0]->length()
                           : !values.empty() ? values[0]->length()
                                             : 0;
    if (n <= AGG_GRAIN) {
        auto st = agg_new(std::move(specs));
        agg_accumulate(*st, keys, values);
        return st;
    }
    // Fixed lanes, not a partial per thread: lane l folds chunks l, l+LANES,
    // ... in row order and the lanes merge in lane order, so the association
    // of a float sum is the same on every run and for every thread count.
    // Every lane re-discovers every group, as the per-thread partials did.
    constexpr std::int64_t LANES = 16;
    const std::int64_t chunks = (n + AGG_GRAIN - 1) / AGG_GRAIN;
    const std::int64_t nl = std::min(LANES, chunks);
    std::vector<AggStatePtr> lanes(static_cast<std::size_t>(nl));
    parallel_for(nl, 1, [&](std::int64_t l0, std::int64_t l1) {
        for (std::int64_t l = l0; l < l1; ++l) {
            AggStatePtr st = agg_new(specs);
            for (std::int64_t c = l; c < chunks; c += nl)
                agg_accumulate(*st, keys, values, c * AGG_GRAIN,
                               std::min(n, (c + 1) * AGG_GRAIN));
            lanes[static_cast<std::size_t>(l)] = std::move(st);
        }
    });
    // The merged groups go back into the order the frame first shows them.
    AggStatePtr merged = agg_in_first_seen_order(agg_merge_many(lanes));
    return merged ? std::move(merged) : agg_new(std::move(specs));
}

namespace {

// Partition results (disjoint groups) joined, the groups back in the order the
// frame first shows them; `first[p]` holds the original first row of each of
// partition p's groups. Nullopt when no partition has a group.
std::optional<DataFrame> stitch_in_first_seen_order(
    std::vector<DataFrame>& parts,
    std::vector<std::vector<std::int64_t>>& first, std::size_t drop_keys) {
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
    std::vector<std::size_t> live;
    std::vector<std::int64_t> first_all;
    for (std::size_t p = 0; p < parts.size(); ++p) {
        if (parts[p].columns.empty()) continue;
        live.push_back(p);
        first_all.insert(first_all.end(), first[p].begin(), first[p].end());
    }
    if (live.empty()) return std::nullopt;
    const Series first_col = Series::flat_i64(
        first_all.data(), static_cast<std::int64_t>(first_all.size()));
    const Series order = argsort(first_col, false);
    // One column at a time: the partitions' copies of it joined and put in
    // order, then dropped, so what is held beside the result is a column or
    // two and not a second and third copy of the whole output.
    DataFrame out;
    const std::size_t ncols = parts[live[0]].columns.size();
    for (std::size_t c = 0; c < ncols; ++c) {
        std::vector<DataFrame> ones;
        ones.reserve(live.size());
        for (const std::size_t p : live) {
            DataFrame one;
            one.names.push_back(parts[p].names[c]);
            one.columns.push_back(std::move(parts[p].columns[c]));
            ones.push_back(std::move(one));
        }
        std::vector<const DataFrame*> ptrs;
        ptrs.reserve(ones.size());
        for (const DataFrame& one : ones) ptrs.push_back(&one);
        DataFrame cat = concat(ptrs, ConcatHow::Vertical);
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
std::optional<DataFrame> finalize_in_place(
    std::vector<AggStatePtr>& states, const std::vector<std::string>& key_names,
    std::size_t drop_keys) {
    if constexpr (std::endian::native != std::endian::little)
        return std::nullopt;  // the validity words are written as words
    if (Env::get("DFTRACER_UTILS_GROUPBY_STITCH"))
        return std::nullopt;  // a test switch: the stitch path as the oracle

    std::vector<std::size_t> live;   // partitions that hold groups
    std::vector<std::int64_t> base;  // first group index of each live one
    std::int64_t G = 0;
    for (std::size_t p = 0; p < states.size(); ++p) {
        const AggState* st = states[p].get();
        if (st == nullptr || st->ngroups() == 0) continue;
        if (st->has_dyn || !agg_first_rows_known(*st)) return std::nullopt;
        live.push_back(p);
        base.push_back(G);
        G += st->ngroups();
    }
    if (live.empty() || G > INT32_MAX) return std::nullopt;
    const AggState& s0 = *states[live[0]];

    // Keys: the same kind in every partition, and a string column's bytes
    // must fit 32-bit offsets.
    std::vector<KeyKind> kinds;
    if (drop_keys == 0) {
        if (s0.nkeys != key_names.size()) return std::nullopt;
        for (std::size_t k = 0; k < s0.nkeys; ++k) {
            const KeyKind kk = key_kind(s0, k);
            if (kk == KeyKind::None) return std::nullopt;
            std::uint64_t bytes = 0;
            for (const std::size_t p : live) {
                const AggState& st = *states[p];
                if (st.nkeys != s0.nkeys || key_kind(st, k) != kk)
                    return std::nullopt;
                if (kk == KeyKind::Str)
                    for (const std::string& v : st.skey_cols[k])
                        bytes += v.size();
            }
            if (bytes > static_cast<std::uint64_t>(INT32_MAX))
                return std::nullopt;
            kinds.push_back(kk);
        }
    }

    // The aggregate columns' schema, from the first partition's cells.
    DataFrame piece0 = agg_finalize_values(s0);
    const std::size_t ncols = piece0.columns.size();
    std::vector<std::size_t> width(ncols);
    std::vector<TypeId> ctype(ncols);
    for (std::size_t c = 0; c < ncols; ++c) {
        const dftu_series* h = piece0.columns[c].handle();
        width[c] = scatter_width(h->type);
        if (width[c] == 0 || h->encoding != Encoding::Flat || h->offsets ||
            !h->nested.empty())
            return std::nullopt;
        ctype[c] = h->type;
    }

    // Each group's final row: the rank of its first row among all groups.
    std::vector<std::int64_t> first_all;
    first_all.reserve(static_cast<std::size_t>(G));
    std::vector<std::uint16_t> li_of(static_cast<std::size_t>(G));
    for (std::size_t li = 0; li < live.size(); ++li) {
        const AggState& st = *states[live[li]];
        first_all.insert(first_all.end(), st.group_first_row.begin(),
                         st.group_first_row.end());
        std::fill_n(li_of.begin() + base[li], st.ngroups(),
                    static_cast<std::uint16_t>(li));
    }
    std::vector<std::int32_t> order(static_cast<std::size_t>(G));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::int32_t a, std::int32_t b) {
        return first_all[static_cast<std::size_t>(a)] <
               first_all[static_cast<std::size_t>(b)];
    });
    std::vector<std::int32_t> rank(static_cast<std::size_t>(G));
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
        std::vector<std::uint8_t> null_row;  // output row is a null key
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
            h->offsets = Buffer::allocate(static_cast<std::size_t>(G + 1) *
                                          sizeof(std::int32_t));
            auto* off = reinterpret_cast<std::int32_t*>(h->offsets->data());
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
            h->data = Buffer::allocate(static_cast<std::size_t>(total));
            std::uint8_t* bytes = h->data->data();
            parallel_for(G, 1 << 14, [&](std::int64_t b, std::int64_t e) {
                for (std::int64_t i = b; i < e; ++i) {
                    const AggState* st;
                    std::int64_t g;
                    resolve(i, st, g);
                    const std::string& v =
                        st->skey_cols[k][static_cast<std::size_t>(g)];
                    if (!v.empty())
                        std::memcpy(bytes + off[i], v.data(), v.size());
                }
            });
        } else {
            h->type = kinds[k] == KeyKind::U64 ? TypeId::Uint64 : TypeId::Int64;
            h->data = Buffer::allocate(static_cast<std::size_t>(G) * 8);
            auto* dst = reinterpret_cast<std::int64_t*>(h->data->data());
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
            h->validity = Buffer::allocate(validity_bytes);
            std::memcpy(h->validity->data(), null_row.data(), validity_bytes);
            h->null_count = nulls;
        }
        out.names.push_back(key_names[k]);
        out.columns.push_back(std::move(col));
    }

    // Aggregate columns: allocated once, filled partition by partition.
    std::vector<dftu_series*> handle(ncols);
    std::vector<std::uint8_t*> dst(ncols);
    const auto words = static_cast<std::size_t>((G + 63) / 64);
    std::vector<std::atomic<std::uint64_t*>> vbits(ncols);
    std::vector<std::mutex> vmu(ncols);
    std::vector<std::atomic<std::int64_t>> nulls_of(ncols);
    for (std::size_t c = 0; c < ncols; ++c) {
        auto* h = new dftu_series();
        Series col{h};
        h->type = ctype[c];
        h->length = G;
        h->data = Buffer::allocate(static_cast<std::size_t>(G) * width[c]);
        handle[c] = h;
        dst[c] = h->data->data();
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
            handle[c]->validity = Buffer::allocate(words * 8);
            std::memset(handle[c]->validity->data(), 0xFF, words * 8);
            b = reinterpret_cast<std::uint64_t*>(handle[c]->validity->data());
            vbits[c].store(b, std::memory_order_release);
        }
        return b;
    };
    auto scatter = [&](std::size_t li, DataFrame& piece) {
        const std::int64_t at = base[li];
        const std::int64_t ng = states[live[li]]->ngroups();
        if (piece.columns.size() != ncols)
            throw std::logic_error("group_by: partition columns differ");
        for (std::size_t c = 0; c < ncols; ++c) {
            const dftu_series* ph = piece.columns[c].handle();
            if (ph->type != ctype[c] || ph->length != ng ||
                ph->encoding != Encoding::Flat || ph->offsets)
                throw std::logic_error("group_by: partition columns differ");
            const std::size_t w = width[c];
            const std::uint8_t* src = ph->data ? ph->data->data() : nullptr;
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
            if (ph->validity == nullptr) continue;
            std::uint64_t* bits = validity_of(c);
            const std::uint8_t* pv = ph->validity->data();
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
            handle[c]->validity.reset();  // a bitmap with no null in it
        handle[c]->null_count = nulls;
    }
    in_place_finalizes.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// Aggregates whose bits do not depend on the order rows reach a group: count,
// min, max and an integer sum. The packed driver's pages arrive in no fixed
// order, so it takes only these; a float sum or a mean goes to the by-key
// driver, which folds a group's rows in row order.
bool order_free(const std::vector<AggSpec>& specs,
                const std::vector<const Series*>& values) {
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
std::optional<DataFrame> group_agg_partitioned(
    const std::vector<const Series*>& in_keys,
    const std::vector<const Series*>& in_values,
    const std::vector<AggSpec>& specs,
    const std::vector<std::string>& key_names, std::size_t drop_keys = 0) {
    // Four partitions per thread: tables small enough for the first-level
    // cache, and the final drain's rounds divide the thread count.
    const auto threads = static_cast<std::int64_t>(
        std::max<std::size_t>(2, dftracer::utils::hardware_concurrency()));
    const auto PARTITIONS = static_cast<std::size_t>(4 * threads);
    const std::int64_t n = in_keys.empty() ? 0 : in_keys[0]->length();
    if (n <= 16 * AGG_GRAIN) return std::nullopt;
    if (!order_free(specs, in_values)) return std::nullopt;
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
        return std::nullopt;
    std::vector<Series> owned;
    owned.reserve(in_keys.size() + in_values.size());
    auto flat = [&](const std::vector<const Series*>& cols, bool key) {
        std::vector<const Series*> out;
        for (const Series* c : cols) {
            owned.push_back(flat_or_ref(*c, key));
            out.push_back(&owned.back());
        }
        return out;
    };
    const std::vector<const Series*> keys = flat(in_keys, true);
    const std::vector<const Series*> values = flat(in_values, false);
    AggPacked shape;
    AggStatePtr probe = agg_new(specs);
    // Integer keys have the direct table, which the scatter cannot beat.
    if (!agg_pack_shape(*probe, keys, values, shape) || !shape.strings)
        return std::nullopt;
    agg_accumulate(*probe, keys, values, 0, AGG_GRAIN / 4);
    if (probe->ngroups() < MANY_GROUPS) return std::nullopt;

    // No barriers: a thread packs its chunk into a page per partition and,
    // when a page fills, queues it on the partition and drains that queue
    // into the partition's state if no one else holds it (else the holder
    // will). The pages in flight are the ones filled while a holder was
    // busy, so memory is the partial pages plus little.
    struct Partition {
        std::atomic<bool> held{false};  // the state's owner
        moodycamel::ConcurrentQueue<std::unique_ptr<std::uint64_t[]>> queue;
        AggStatePtr state;
    };
    std::vector<Partition> partitions(PARTITIONS);
    for (Partition& part : partitions) part.state = agg_new(specs);
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
    using Pages = std::vector<AggPages>;
    std::vector<std::unique_ptr<Pages>> pool;
    std::mutex pool_mutex;
    std::atomic<bool> packed{true};
    parallel_for(n, AGG_GRAIN, [&](std::int64_t b, std::int64_t e) {
        if (!packed.load(std::memory_order_relaxed)) return;
        std::unique_ptr<Pages> pages;
        {
            const std::lock_guard<std::mutex> lock(pool_mutex);
            if (!pool.empty()) {
                pages = std::move(pool.back());
                pool.pop_back();
            }
        }
        if (!pages) pages = std::make_unique<Pages>(PARTITIONS);
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
    if (!packed.load()) return std::nullopt;
    // The partial pages and whatever is still queued, one partition per
    // task, no holder to wait for.
    std::vector<AggStatePtr> states(PARTITIONS);
    parallel_for(static_cast<std::int64_t>(PARTITIONS), 1,
                 [&](std::int64_t p0, std::int64_t p1) {
                     for (std::int64_t p = p0; p < p1; ++p) {
                         const auto pi = static_cast<std::size_t>(p);
                         drain(pi);
                         for (const std::unique_ptr<Pages>& pages : pool) {
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
    if (std::optional<DataFrame> out =
            finalize_in_place(states, key_names, drop_keys))
        return out;
    std::vector<DataFrame> parts(PARTITIONS);
    std::vector<std::vector<std::int64_t>> first(PARTITIONS);
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
std::optional<DataFrame> group_agg_by_key_hash(
    const std::vector<const Series*>& in_keys,
    const std::vector<const Series*>& in_values,
    const std::vector<AggSpec>& specs,
    const std::vector<std::string>& key_names, std::size_t drop_keys = 0) {
    // ponytail: threshold set from benchmarks/groupby_analyzer_bench.py; below
    // it the per-thread partials win, raise it if a mid-size case regresses.
    constexpr std::int64_t MIN_ROWS = 4 * AGG_GRAIN;
    const std::int64_t n = in_keys.empty() ? 0 : in_keys[0]->length();
    if (n <= MIN_ROWS) return std::nullopt;
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
                return std::nullopt;
        }
    }
    {
        // Order-dependent state (a tie, a bounded counter) would change with
        // the order the morsels reach a partition.
        AggStatePtr probe = agg_new(specs);
        if (probe->has_arg || probe->has_lst || probe->has_ss)
            return std::nullopt;
    }
    std::vector<Series> owned;
    owned.reserve(in_keys.size() + in_values.size());
    auto flat = [&](const std::vector<const Series*>& cols, bool key) {
        std::vector<const Series*> out;
        for (const Series* c : cols) {
            owned.push_back(flat_or_ref(*c, key));
            out.push_back(&owned.back());
        }
        return out;
    };
    const std::vector<const Series*> keys = flat(in_keys, true);
    const std::vector<const Series*> values = flat(in_values, false);
    std::size_t groups_hint = 0;
    {
        AggStatePtr probe = agg_new(specs);
        agg_accumulate(*probe, keys, values, 0, AGG_GRAIN / 4);
        if (probe->ngroups() < MANY_GROUPS) return std::nullopt;
        // How many groups the rows hold, from how often the probe saw each
        // one (Chao1: the groups seen plus those that singletons suggest).
        double f1 = 0, f2 = 0;
        for (const std::uint64_t c : probe->counts) {
            f1 += c == 1;
            f2 += c == 2;
        }
        const double seen = static_cast<double>(probe->ngroups());
        groups_hint = static_cast<std::size_t>(std::min(
            static_cast<double>(n), seen + f1 * f1 / (2.0 * (f2 + 1))));
    }

    const auto threads = static_cast<std::int64_t>(
        std::max<std::size_t>(2, dftracer::utils::hardware_concurrency()));
    const auto PARTITIONS = static_cast<std::size_t>(4 * threads);
    std::vector<bool> nullable(keys.size());
    for (std::size_t j = 0; j < keys.size(); ++j)
        nullable[j] = keys[j]->null_count() > 0;
    constexpr std::uint64_t NULL_KEY = 0x9E3779B97F4A7C15ULL;

    DataFrame src;  // the key and value columns, shared, not copied
    for (const Series* c : keys) {
        src.names.push_back("k" + std::to_string(src.columns.size()));
        src.columns.push_back(c->share());
    }
    for (const Series* c : values) {
        src.names.push_back("v" + std::to_string(src.columns.size()));
        src.columns.push_back(c->share());
    }
    std::vector<AggStatePtr> states(PARTITIONS);
    for (AggStatePtr& st : states) {
        st = agg_new(specs);
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
        std::vector<std::uint32_t> rows;  // row - morsel start, by partition
        std::vector<std::int64_t> off;    // PARTITIONS + 1 offsets into rows
    };
    std::vector<Bucketed> win(static_cast<std::size_t>(WINDOW));
    auto bucket_morsel = [&](std::int64_t m, Bucketed& out) {
        const std::int64_t b = m * AGG_GRAIN;
        const std::int64_t e = std::min(n, b + AGG_GRAIN);
        std::vector<std::int32_t> bucket(static_cast<std::size_t>(e - b));
        out.off.assign(PARTITIONS + 1, 0);
        for (std::int64_t i = b; i < e; ++i) {
            std::uint64_t h = 0;
            for (std::size_t j = 0; j < keys.size(); ++j) {
                const Series& c = *keys[j];
                std::uint64_t v;
                if (nullable[j] && c.is_null(i))
                    v = NULL_KEY;
                else if (c.type() == TypeId::String)
                    v = dftracer::utils::hash::fnv1a_hash(c.string_at(i));
                else
                    v = read_u64(c, i);
                h = dftracer::utils::hash::splitmix64(h ^ v);
            }
            const auto p =
                static_cast<std::int32_t>(((h >> 32) * PARTITIONS) >> 32);
            bucket[static_cast<std::size_t>(i - b)] = p;
            ++out.off[static_cast<std::size_t>(p) + 1];
        }
        for (std::size_t p = 0; p < PARTITIONS; ++p)
            out.off[p + 1] += out.off[p];
        out.rows.resize(static_cast<std::size_t>(e - b));
        std::vector<std::int64_t> cur(out.off.begin(), out.off.end() - 1);
        for (std::int64_t i = b; i < e; ++i)
            out.rows[static_cast<std::size_t>(cur[static_cast<std::size_t>(
                bucket[static_cast<std::size_t>(i - b)])]++)] =
                static_cast<std::uint32_t>(i - b);
    };
    for (std::int64_t w0 = 0; w0 < nmorsels; w0 += WINDOW) {
        const std::int64_t wn = std::min(WINDOW, nmorsels - w0);
        parallel_for(wn, 1, [&](std::int64_t m0, std::int64_t m1) {
            for (std::int64_t m = m0; m < m1; ++m)
                bucket_morsel(w0 + m, win[static_cast<std::size_t>(m)]);
        });
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
                        std::vector<std::int64_t> mine;
                        mine.reserve(static_cast<std::size_t>(bk.off[pi + 1] -
                                                              bk.off[pi]));
                        for (std::int64_t r = bk.off[pi]; r < bk.off[pi + 1];
                             ++r)
                            mine.push_back(
                                at + bk.rows[static_cast<std::size_t>(r)]);
                        const DataFrame sub = take(src, mine);
                        std::vector<const Series*> sk, sv;
                        for (std::size_t j = 0; j < keys.size(); ++j)
                            sk.push_back(&sub.columns[j]);
                        for (std::size_t j = 0; j < values.size(); ++j)
                            sv.push_back(&sub.columns[keys.size() + j]);
                        st.row_ids = mine.data();
                        agg_accumulate(st, sk, sv);
                        st.row_ids = nullptr;
                    }
                }
            });
    }
    if (std::optional<DataFrame> out =
            finalize_in_place(states, key_names, drop_keys))
        return out;
    std::vector<DataFrame> parts(PARTITIONS);
    std::vector<std::vector<std::int64_t>> first(PARTITIONS);
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
                         states[pi].reset();  // frees the settled moments
                     }
                 });
    if (!ok.load()) return std::nullopt;
    return stitch_in_first_seen_order(parts, first, drop_keys);
}

// `drop_keys` leaves the first columns (the keys) out of the result: a column
// batch after the first has them already.
DataFrame group_agg_whole(const std::vector<const Series*>& keys,
                          const std::vector<const Series*>& values,
                          std::vector<AggSpec> specs,
                          const std::vector<std::string>& key_names,
                          std::size_t drop_keys = 0) {
    if (std::optional<DataFrame> out =
            group_agg_partitioned(keys, values, specs, key_names, drop_keys))
        return std::move(*out);
    if (std::optional<DataFrame> out =
            group_agg_by_key_hash(keys, values, specs, key_names, drop_keys))
        return std::move(*out);
    DataFrame whole = agg_finalize(
        *group_agg_state(keys, values, std::move(specs)), key_names);
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
std::int64_t cell_bytes(const std::vector<AggSpec>& specs, std::int32_t col) {
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
std::optional<DataFrame> group_agg_in_batches(
    const std::vector<const Series*>& keys,
    const std::vector<const Series*>& values, const std::vector<AggSpec>& specs,
    const std::vector<std::string>& key_names) {
    // ponytail: the state one batch may hold; raise it for speed, lower it
    // for memory. The passes over the keys grow as columns / batch width.
    constexpr std::int64_t BATCH_STATE_BYTES = 8 << 20;
    constexpr std::size_t FIRST_BATCH = 1;
    const std::int64_t n = keys.empty() ? 0 : keys[0]->length();
    if (n <= 4 * AGG_GRAIN) return std::nullopt;
    for (const AggSpec& sp : specs)
        if (!one_column_state(sp.op)) return std::nullopt;
    // The value columns the aggregates read, in order of first use.
    std::vector<std::int32_t> used;
    for (const AggSpec& sp : specs)
        if (sp.value_col >= 0 &&
            std::find(used.begin(), used.end(), sp.value_col) == used.end())
            used.push_back(sp.value_col);
    if (used.size() <= FIRST_BATCH) return std::nullopt;
    // The count, sum, min and max cells are small next to the output; it is
    // the moments (a FieldStat a cell) that a batch bounds.
    if (std::none_of(used.begin(), used.end(),
                     [&](std::int32_t c) { return cell_bytes(specs, c) > 32; }))
        return std::nullopt;
    {
        std::vector<Series> owned;
        owned.reserve(keys.size());
        std::vector<const Series*> ks;
        for (const Series* c : keys) {
            owned.push_back(flat_or_ref(*c, true));
            ks.push_back(&owned.back());
        }
        std::vector<Series> owned_values;
        owned_values.reserve(values.size());
        std::vector<const Series*> vs;
        for (const Series* c : values) {
            owned_values.push_back(flat_or_ref(*c, false));
            vs.push_back(&owned_values.back());
        }
        AggStatePtr probe = agg_new(specs);
        agg_accumulate(*probe, ks, vs, 0, AGG_GRAIN / 4);
        if (probe->ngroups() < MANY_GROUPS) return std::nullopt;
    }
    // One batch: the aggregates over `cols`, reading a values list of those
    // columns alone. Count (no column) rides with the first batch.
    auto run = [&](std::size_t from, std::size_t count, bool with_counts) {
        const std::vector<std::int32_t> cols(used.begin() + from,
                                             used.begin() + from + count);
        std::vector<AggSpec> sub;
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
        std::vector<const Series*> vs;
        for (const std::int32_t c : cols)
            vs.push_back(values[static_cast<std::size_t>(c)]);
        return group_agg_whole(keys, vs, std::move(sub), key_names,
                               from == 0 ? 0 : keys.size());
    };
    std::vector<std::string> made_names;
    std::vector<Series> made;
    std::vector<Series> key_cols;
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
    DataFrame head = run(0, FIRST_BATCH, true);
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
        DataFrame next = run(at, count, false);
        keep(next, false);
        at += count;
    }
    // The columns back in the order the aggregates were asked for.
    DataFrame out;
    for (std::size_t k = 0; k < keys.size(); ++k) {
        out.names.push_back(key_names[k]);
        out.columns.push_back(std::move(key_cols[k]));
    }
    for (const AggSpec& sp : specs)
        for (std::size_t j = 0; j < made.size(); ++j)
            if (made_names[j] == sp.out) {
                out.names.push_back(made_names[j]);
                out.columns.push_back(std::move(made[j]));
                break;
            }
    return out;
}

}  // namespace

DataFrame group_agg(const std::vector<const Series*>& keys,
                    const std::vector<const Series*>& values,
                    std::vector<AggSpec> specs,
                    const std::vector<std::string>& key_names) {
    std::vector<Series> joined;
    joined.reserve(keys.size() + values.size());
    auto flat = [&](const std::vector<const Series*>& in) {
        std::vector<const Series*> out;
        out.reserve(in.size());
        for (const Series* c : in) {
            if (c && c->encoding() == Encoding::Chunked) {
                joined.push_back(join_chunks(*c));
                c = &joined.back();
            }
            out.push_back(c);
        }
        return out;
    };
    const std::vector<const Series*> flat_keys = flat(keys);
    const std::vector<const Series*> flat_values = flat(values);
    if (std::optional<DataFrame> out =
            group_agg_in_batches(flat_keys, flat_values, specs, key_names))
        return std::move(*out);
    return group_agg_whole(flat_keys, flat_values, std::move(specs), key_names);
}

DataFrame group_agg(const Series& key, const std::vector<const Series*>& values,
                    std::vector<AggSpec> specs, const std::string& key_name) {
    const std::vector<const Series*> keys{&key};
    return group_agg(keys, values, std::move(specs),
                     std::vector<std::string>{key_name});
}

}  // namespace dftracer::utils::dataframe
