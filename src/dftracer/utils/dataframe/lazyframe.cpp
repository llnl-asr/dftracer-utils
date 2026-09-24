#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/hash/hash.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>  // compute_memory_budget
#include <dftracer/utils/core/coro/task_abi.h>         // task_to_abi
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/dataframe/agg.h>        // streaming group-by state
#include <dftracer/utils/dataframe/batch_ops.h>  // concat_columns, take, concat
#include <dftracer/utils/dataframe/field_stat.h>         // FieldStat (describe)
#include <dftracer/utils/dataframe/internal/cell_ops.h>  // row_key, cell_to_string
#include <dftracer/utils/dataframe/internal/column_data.h>  // dftu_series (typed null templates)
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>  // dataframe_handle_wrap/take
#include <dftracer/utils/dataframe/internal/expr_handle.h>  // expr_handle_wrap/unwrap
#include <dftracer/utils/dataframe/internal/fingerprint.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dataframe/internal/node_registry.h>  // find_node
#include <dftracer/utils/dataframe/internal/reclaim_registry.h>
#include <dftracer/utils/dataframe/internal/schema_types.h>   // dftu_schema
#include <dftracer/utils/dataframe/internal/spill.h>  // external-merge spill
#include <dftracer/utils/dataframe/join.h>            // HashJoin (join cursor)
#include <dftracer/utils/dataframe/kernels/field_stat.h>  // field_stat_reduce (SIMD)
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/parallel.h>  // parallel_for (parallel sinks)
#include <dftracer/utils/dataframe/types.h>     // byte_width, buffer_bytes

#include <algorithm>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <variant>

namespace dftracer::utils::dataframe {

Cursor::~Cursor() {
    if (registry_) registry_->remove(this);
}

void Cursor::attach(std::shared_ptr<ReclaimRegistry> registry) {
    if (registry_) registry_->remove(this);
    registry_ = std::move(registry);
    if (registry_) registry_->add(this);
}

namespace {

std::vector<const Series*> column_ptrs(const std::vector<Series>& cols) {
    std::vector<const Series*> in;
    in.reserve(cols.size());
    for (const Series& c : cols) in.push_back(&c);
    return in;
}

int column_index_of(const std::vector<std::string>& names,
                    const std::string& name) {
    auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? -1 : static_cast<int>(it - names.begin());
}

// Default scan chunk when the caller does not set one (morsel_rows <= 0).
constexpr std::int64_t DEFAULT_MORSEL_ROWS = 65536;

// Build one standalone DataFrame from a morsel. Names come from the morsel's
// own schema (streaming, self-describing), else the cursor's data-dependent
// out_names(), else the static plan schema.
DataFrame frame_from_morsel(
    Morsel&& m, const std::vector<std::string>& static_names,
    const std::optional<std::vector<std::string>>& out_names) {
    DataFrame out;
    out.columns = std::move(m.columns);
    if (!m.name_ids.empty()) {
        out.names.reserve(m.name_ids.size());
        for (std::uint32_t id : m.name_ids)
            out.names.emplace_back(m.intern->resolve(id));
    } else if (out_names) {
        out.names = *out_names;
    } else {
        out.names = static_names;
    }
    // Fold the out-of-band dyn set back in as trailing named columns, so a
    // DataFrame-terminal consumer sees the per-morsel dyn columns by name.
    for (std::size_t i = 0; i < m.dyn_columns.size(); ++i) {
        out.names.push_back(std::move(m.dyn_names[i]));
        out.columns.push_back(std::move(m.dyn_columns[i]));
    }
    return out;
}

// Drain a chunk generator to a single DataFrame.
coro::CoroTask<DataFrame> drain_stream(coro::AsyncGenerator<DataFrame> gen) {
    std::vector<DataFrame> parts;
    while (auto df = co_await gen.next()) parts.push_back(std::move(*df));
    if (parts.empty()) co_return DataFrame{};
    // A single chunk needs no merge - concat cannot rejoin a nested
    // (List/Struct) column, which a single already-complete chunk (e.g. a
    // resident source's one morsel) may carry.
    if (parts.size() == 1) co_return std::move(parts[0]);

    bool uniform = true;
    for (std::size_t i = 1; i < parts.size() && uniform; ++i) {
        if (parts[i].names != parts[0].names ||
            parts[i].columns.size() != parts[0].columns.size()) {
            uniform = false;
            break;
        }
        for (std::size_t c = 0; c < parts[0].columns.size(); ++c) {
            if (parts[i].columns[c].type() != parts[0].columns[c].type()) {
                uniform = false;
                break;
            }
        }
    }
    std::vector<const DataFrame*> ptrs;
    ptrs.reserve(parts.size());
    for (const DataFrame& p : parts) ptrs.push_back(&p);
    co_return concat(ptrs, uniform ? ConcatHow::Vertical : ConcatHow::Diagonal);
}

// Drain a Cursor to a single DataFrame: for a pipeline breaker (take, reverse,
// sort_by_multi) whose eager op has no incremental form and needs every row at
// once.
coro::CoroTask<DataFrame> drain_cursor(Cursor& c, std::vector<std::string> sch,
                                       std::int64_t max_rows) {
    std::vector<DataFrame> parts;
    while (auto m = co_await c.next(max_rows)) {
        DataFrame df;
        df.names = sch;
        df.columns = std::move(m->columns);
        parts.push_back(std::move(df));
    }
    if (parts.empty()) {
        DataFrame e;
        e.names = std::move(sch);
        co_return e;
    }
    if (parts.size() == 1) co_return std::move(parts[0]);
    std::vector<const DataFrame*> ptrs;
    ptrs.reserve(parts.size());
    for (const DataFrame& p : parts) ptrs.push_back(&p);
    co_return concat(ptrs, ConcatHow::Vertical);
}

Morsel morsel_of(DataFrame&& f) {
    Morsel out;
    out.rows = f.num_rows();
    out.columns.reserve(f.columns.size());
    for (const Series& c : f.columns) out.columns.push_back(c.materialize());
    return out;
}

// Read a numeric cell as a double for key comparison (FLAT columns only).
double read_num(const Series& c, std::int64_t i) {
    switch (c.type()) {
        case TypeId::Bool:
            return (c.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
        case TypeId::Int8:
            return c.data<std::int8_t>()[i];
        case TypeId::Int16:
            return c.data<std::int16_t>()[i];
        case TypeId::Int32:
            return c.data<std::int32_t>()[i];
        case TypeId::Int64:
            return static_cast<double>(c.data<std::int64_t>()[i]);
        case TypeId::Uint8:
            return c.data<std::uint8_t>()[i];
        case TypeId::Uint16:
            return c.data<std::uint16_t>()[i];
        case TypeId::Uint32:
            return c.data<std::uint32_t>()[i];
        case TypeId::Uint64:
            return static_cast<double>(c.data<std::uint64_t>()[i]);
        case TypeId::Float32:
            return static_cast<double>(c.data<float>()[i]);
        case TypeId::Float64:
            return c.data<double>()[i];
        default:
            return 0.0;
    }
}

// Three-way compare of two key cells for a merge, honoring `descending`. Nulls
// always sort last (both directions), matching argsort.
int cmp_cell(const Series& a, std::int64_t ia, const Series& b, std::int64_t ib,
             bool descending) {
    const bool na = a.is_null(ia), nb = b.is_null(ib);
    if (na || nb) return na && nb ? 0 : (na ? 1 : -1);
    int c;
    if (narrow_varwidth_type(a.type()) == TypeId::String) {
        const std::string_view x = a.string_at(ia), y = b.string_at(ib);
        c = x < y ? -1 : (x > y ? 1 : 0);
    } else {
        const double x = read_num(a, ia), y = read_num(b, ib);
        c = x < y ? -1 : (x > y ? 1 : 0);
    }
    return descending ? -c : c;
}

// Approximate in-memory byte size of a set of FLAT columns (spill trigger).
std::size_t morsel_bytes(const std::vector<Series>& cols) {
    std::size_t total = 0;
    for (const Series& c : cols) {
        const std::int64_t n = c.length();
        const TypeId t = c.type();
        if (is_wide_offset_type(t)) {  // LargeString / LargeBinary
            const std::int64_t* offs = c.offsets64();
            total +=
                static_cast<std::size_t>(n + 1) * sizeof(std::int64_t) +
                (n > 0 && offs != nullptr ? static_cast<std::size_t>(offs[n])
                                          : 0);
        } else if (t == TypeId::String || t == TypeId::Binary) {
            const std::int32_t* offs = dftu_series_offsets(c.handle());
            total +=
                static_cast<std::size_t>(n + 1) * sizeof(std::int32_t) +
                (n > 0 && offs != nullptr ? static_cast<std::size_t>(offs[n])
                                          : 0);
        } else if (t == TypeId::FixedSizeBinary) {
            total +=
                static_cast<std::size_t>(n) *
                static_cast<std::size_t>(dftu_series_fixed_size(c.handle()));
        } else {
            total += buffer_bytes(t, n);
        }
    }
    return total;
}

// Fan-out width for the partitioned first-seen dedup below.
constexpr std::size_t DEDUP_PARTITIONS = 32;

// Radix-partition `keys` by hash into `seen_p.size()` buckets, each with its
// own running hash set, and mark keep[i] the first time each key is seen
// (order-preserving): buckets never overlap, so each is probed by exactly one
// worker with no lock. `seen_p` carries state across calls, bounded by the
// distinct-key count rather than the row count. An empty `seen_p` means no
// parallel backend is installed; the single `seen_serial` set is used instead
// so the partition bookkeeping is never paid for nothing.
std::vector<std::uint8_t> first_seen_mask(
    std::vector<std::string>& keys, std::int64_t n,
    ankerl::unordered_dense::set<std::string>& seen_serial,
    std::vector<ankerl::unordered_dense::set<std::string>>& seen_p) {
    std::vector<std::uint8_t> keep_mask(static_cast<std::size_t>(n), 0);
    if (seen_p.empty()) {
        for (std::int64_t i = 0; i < n; ++i)
            if (seen_serial.insert(std::move(keys[static_cast<std::size_t>(i)]))
                    .second)
                keep_mask[static_cast<std::size_t>(i)] = 1;
        return keep_mask;
    }
    const std::size_t p = seen_p.size();
    std::vector<std::vector<std::int64_t>> buckets(p);
    for (std::int64_t i = 0; i < n; ++i)
        buckets[std::hash<std::string>{}(keys[static_cast<std::size_t>(i)]) % p]
            .push_back(i);
    parallel_for(
        static_cast<std::int64_t>(p), 1, [&](std::int64_t pb, std::int64_t pe) {
            for (std::int64_t g = pb; g < pe; ++g)
                for (std::int64_t i : buckets[static_cast<std::size_t>(g)])
                    if (seen_p[static_cast<std::size_t>(g)]
                            .insert(
                                std::move(keys[static_cast<std::size_t>(i)]))
                            .second)
                        keep_mask[static_cast<std::size_t>(i)] = 1;
        });
    return keep_mask;
}

// Approximate per-entry overhead of one string in an unordered_dense::set
// (node + bucket bookkeeping), added to the key's own byte length when sizing
// the distinct-key state for the unique() spill trigger below.
constexpr std::size_t DEDUP_ENTRY_OVERHEAD = 48;

// Grace-hash-distinct spill fan-out and recursion bound (unique() below): a
// partition that still exceeds budget after fanning out is re-partitioned
// with a depth-salted hash, capped so a single hot key (which always lands in
// the same partition, however deep) cannot recurse forever.
constexpr int UNIQUE_SPILL_FANOUT = 16;
constexpr int UNIQUE_SPILL_MAX_DEPTH = 3;
constexpr std::int64_t UNIQUE_SPILL_MIN_LEAF_ROWS = 64;

// Depth-salted hash of a row key: depth 0 matches the plain hash so the first
// pass agrees with any caller hashing the same key; deeper passes mix in the
// depth so a key that collided at one depth spreads differently at the next.
std::size_t unique_spill_hash(const std::string& key, int depth) {
    const std::size_t h = std::hash<std::string>{}(key);
    if (depth == 0) return h;
    return static_cast<std::size_t>(hash::splitmix64(
        static_cast<std::uint64_t>(h) ^
        (static_cast<std::uint64_t>(depth) * hash::GOLDEN_RATIO)));
}

// All columns but the first (the row-id helper column prepended by the
// unique() spill path), as cheap shared views.
std::vector<Series> drop_first_column(const std::vector<Series>& cols) {
    std::vector<Series> out;
    out.reserve(cols.size() - 1);
    for (std::size_t i = 1; i < cols.size(); ++i)
        out.push_back(cols[i].share());
    return out;
}

// ---- cursors ----------------------------------------------------------------

// Reads contiguous chunks off an in-memory frame as zero-copy offset views.
class InMemoryCursor : public Cursor {
   public:
    explicit InMemoryCursor(std::shared_ptr<const DataFrame> frame)
        : frame_(std::move(frame)), n_(frame_->num_rows()) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        std::optional<Morsel> out;
        fill(max_rows, out);
        co_return out;
    }

    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        fill(max_rows, out);
        return true;
    }

   private:
    // Never suspends: the whole frame is already resident, so next() and
    // try_next() share this synchronous slice logic.
    void fill(std::int64_t max_rows, std::optional<Morsel>& out) {
        if (off_ >= n_) {
            out.reset();
            return;
        }
        const std::int64_t len =
            std::min(std::max<std::int64_t>(max_rows, 1), n_ - off_);
        DataFrame chunk = frame_->slice(off_, len);
        off_ += len;
        Morsel m;
        m.rows = len;
        m.columns.reserve(chunk.columns.size());
        for (Series& c : chunk.columns) m.columns.push_back(std::move(c));
        m.batch_index = next_index_++;
        m.ordering = Ordering::Sequence;
        out = std::move(m);
    }

    std::shared_ptr<const DataFrame> frame_;
    std::int64_t n_;
    std::int64_t off_ = 0;
    std::int64_t next_index_ = 0;
};

// Keeps rows where `pred` is true; pulls upstream until it has a non-empty
// morsel or the input ends.
class FilterCursor : public Cursor {
   public:
    FilterCursor(std::unique_ptr<Cursor> in, Expr pred)
        : in_(std::move(in)), pred_(std::move(pred)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        while (auto m = co_await in_->next(max_rows)) {
            std::optional<Morsel> filtered = apply(std::move(*m));
            if (filtered) co_return filtered;
        }
        co_return std::nullopt;
    }

    // Loops on the upstream's own try_next, exactly as next() loops on
    // next(): a morsel that filters to zero rows contributed nothing to the
    // output, so consuming it synchronously and moving on is observationally
    // identical to consuming it via next() - the row it produced (none)
    // does not depend on which path pulled it. Returns false the moment
    // upstream cannot answer synchronously, leaving nothing consumed-but-
    // unaccounted-for behind.
    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        for (;;) {
            std::optional<Morsel> m;
            if (!in_->try_next(max_rows, m)) return false;
            if (!m) {
                out.reset();
                return true;
            }
            std::optional<Morsel> filtered = apply(std::move(*m));
            if (filtered) {
                out = std::move(filtered);
                return true;
            }
        }
    }

   private:
    // Dropping rows can only shrink the surviving set of an already-ordered
    // column - it cannot move one kept row before another - so a ByColumn
    // claim on the input survives filtering unchanged.
    std::optional<Morsel> apply(Morsel&& m) {
        Series mask = eval(pred_, column_ptrs(m.columns));
        DataFrame tmp;
        tmp.names.assign(m.columns.size(), std::string());
        for (Series& c : m.columns) tmp.columns.push_back(c.share());
        DataFrame kept = tmp.filter(mask);
        Morsel out;
        out.columns.reserve(kept.columns.size());
        for (const Series& c : kept.columns)
            out.columns.push_back(c.materialize());
        out.rows = out.columns.empty() ? 0 : out.columns.front().length();
        if (out.rows == 0) return std::nullopt;
        out.ordering = m.ordering;
        out.ordered_column = m.ordered_column;
        out.ordered_descending = m.ordered_descending;
        return out;
    }

   public:
    // Same columns in and out, and dropping rows commutes with a row-wise
    // narrowing, so the predicate passes up unchanged.
    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        co_return co_await in_->narrow(predicate);
    }

   private:
    std::unique_ptr<Cursor> in_;
    Expr pred_;
};

// Keeps rows where a precomputed mask is true. The mask is fixed against this
// op's own input stream, one flag per row in order, so each morsel consumes
// the matching slice as it goes by - streams like FilterCursor.
class FilterMaskCursor : public Cursor {
   public:
    FilterMaskCursor(std::unique_ptr<Cursor> in, Series mask)
        : in_(std::move(in)), mask_(std::move(mask)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        while (auto m = co_await in_->next(max_rows)) {
            std::optional<Morsel> filtered = apply(std::move(*m));
            if (filtered) co_return filtered;
        }
        co_return std::nullopt;
    }

    // Same argument as FilterCursor::try_next: `off_` advances in lockstep
    // with whichever morsels were actually consumed, sync or async, and an
    // empty-after-mask morsel contributes nothing either way.
    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        for (;;) {
            std::optional<Morsel> m;
            if (!in_->try_next(max_rows, m)) return false;
            if (!m) {
                out.reset();
                return true;
            }
            std::optional<Morsel> filtered = apply(std::move(*m));
            if (filtered) {
                out = std::move(filtered);
                return true;
            }
        }
    }

   private:
    std::optional<Morsel> apply(Morsel&& m) {
        const std::int64_t n = m.rows;
        if (off_ + n > mask_.length())
            throw std::out_of_range("filter_mask: mask shorter than input");
        // Series::slice only supports byte-per-element FLAT types, not the
        // bit-packed Bool layout, so the sub-mask is re-packed by hand
        // instead (same bit math as IsDupCursor's mask build).
        const std::uint8_t* bits = mask_.data<std::uint8_t>();
        std::vector<std::uint8_t> sub(static_cast<std::size_t>((n + 7) / 8), 0);
        for (std::int64_t i = 0; i < n; ++i) {
            const std::int64_t p = off_ + i;
            if ((bits[p >> 3] >> (p & 7)) & 1u)
                sub[static_cast<std::size_t>(i >> 3)] |=
                    static_cast<std::uint8_t>(1u << (i & 7));
        }
        off_ += n;
        Series sub_mask = Series::flat(TypeId::Bool, sub.data(), n);
        DataFrame tmp;
        tmp.names.assign(m.columns.size(), std::string());
        for (Series& c : m.columns) tmp.columns.push_back(c.share());
        DataFrame kept = tmp.filter(sub_mask);
        Morsel out;
        out.columns.reserve(kept.columns.size());
        for (const Series& c : kept.columns)
            out.columns.push_back(c.materialize());
        out.rows = out.columns.empty() ? 0 : out.columns.front().length();
        if (out.rows == 0) return std::nullopt;
        // Same argument as FilterCursor::apply: dropping rows cannot break a
        // sortedness claim on the survivors.
        out.ordering = m.ordering;
        out.ordered_column = m.ordered_column;
        out.ordered_descending = m.ordered_descending;
        return out;
    }

    std::unique_ptr<Cursor> in_;
    Series mask_;
    std::int64_t off_ = 0;
};

// Projects columns by index (share, no copy).
class SelectCursor : public Cursor {
   public:
    SelectCursor(std::unique_ptr<Cursor> in, std::vector<int> idx)
        : in_(std::move(in)), idx_(std::move(idx)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return std::nullopt;
        co_return apply(std::move(*m));
    }

    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        std::optional<Morsel> m;
        if (!in_->try_next(max_rows, m)) return false;
        out = m ? std::optional<Morsel>(apply(std::move(*m))) : std::nullopt;
        return true;
    }

   private:
    // Selection keeps rows and their order, but renumbers columns; a
    // ByColumn claim survives only if the ordered column is still present,
    // and must be remapped to its new position.
    Morsel apply(Morsel&& m) {
        Morsel out;
        out.rows = m.rows;
        out.batch_index = m.batch_index;
        out.ordering = m.ordering;
        out.ordered_descending = m.ordered_descending;
        if (m.ordering == Ordering::ByColumn) {
            auto it = std::find(idx_.begin(), idx_.end(), m.ordered_column);
            if (it != idx_.end())
                out.ordered_column =
                    static_cast<std::int32_t>(it - idx_.begin());
            else
                out.ordering = Ordering::Unordered;
        }
        out.columns.reserve(idx_.size());
        for (int i : idx_) out.columns.push_back(m.columns[i].share());
        return out;
    }

   public:
    // Output column i is input column idx_[i]; a reference past the
    // selection has no input column and stops the forwarding.
    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        if (expr_max_col(predicate) >= static_cast<std::int32_t>(idx_.size()))
            co_return false;
        std::vector<std::int32_t> map(idx_.begin(), idx_.end());
        co_return co_await in_->narrow(expr_remap_cols(predicate, map));
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::vector<int> idx_;
};

// Adds or replaces one column from an expr.
class WithColumnCursor : public Cursor {
   public:
    WithColumnCursor(std::unique_ptr<Cursor> in, Expr e, int replace,
                     std::size_t width)
        : in_(std::move(in)),
          expr_(std::move(e)),
          replace_(replace),
          width_(width) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return std::nullopt;
        co_return apply(std::move(*m));
    }

    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        std::optional<Morsel> m;
        if (!in_->try_next(max_rows, m)) return false;
        out = m ? std::optional<Morsel>(apply(std::move(*m))) : std::nullopt;
        return true;
    }

   private:
    // Adding a column, or replacing one other than the ordered column,
    // leaves row order and every other column's position untouched. Replacing
    // the ordered column itself invalidates the claim: the new values have no
    // known relation to it.
    Morsel apply(Morsel&& m) {
        Series nc = eval(expr_, column_ptrs(m.columns));
        Morsel out;
        out.rows = m.rows;
        out.batch_index = m.batch_index;
        out.ordering = m.ordering;
        out.ordered_column = m.ordered_column;
        out.ordered_descending = m.ordered_descending;
        if (m.ordering == Ordering::ByColumn && replace_ == m.ordered_column)
            out.ordering = Ordering::Unordered;
        out.columns = std::move(m.columns);
        out.dyn_names = std::move(m.dyn_names);
        out.dyn_columns = std::move(m.dyn_columns);
        if (replace_ >= 0)
            out.columns[static_cast<std::size_t>(replace_)] = std::move(nc);
        else
            out.columns.push_back(std::move(nc));
        return out;
    }

   public:
    // Every input column keeps its position; only a predicate reading the
    // produced column (which the input does not carry, or carries with
    // other values) cannot go up.
    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        if (expr_max_col(predicate) >= static_cast<std::int32_t>(width_))
            co_return false;
        if (replace_ >= 0 && expr_references(predicate, replace_))
            co_return false;
        co_return co_await in_->narrow(predicate);
    }

   private:
    std::unique_ptr<Cursor> in_;
    Expr expr_;
    int replace_;
    std::size_t width_;
};

// Slices a morsel's rows [off, off+len) as a fresh FLAT morsel.
Morsel slice_morsel(const Morsel& m, std::int64_t off, std::int64_t len) {
    DataFrame tmp;
    tmp.names.assign(m.columns.size(), std::string());
    for (const Series& c : m.columns) tmp.columns.push_back(c.share());
    DataFrame s = tmp.slice(off, len);
    Morsel out;
    out.rows = len;
    out.columns.reserve(s.columns.size());
    for (const Series& c : s.columns) out.columns.push_back(c.materialize());
    return out;
}

// Emits the global row window [offset, offset+len); stops once len rows are out
// (so head() short-circuits the scan).
class SliceCursor : public Cursor {
   public:
    SliceCursor(std::unique_ptr<Cursor> in, std::int64_t offset,
                std::int64_t len)
        : in_(std::move(in)), offset_(offset), len_(len) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        while (emitted_ < len_) {
            auto m = co_await in_->next(max_rows);
            if (!m) co_return std::nullopt;
            std::optional<Morsel> windowed = apply(*m);
            if (windowed) co_return windowed;
        }
        co_return std::nullopt;
    }

    // seen_/emitted_ track total rows consumed and rows emitted so far, both
    // of which advance identically whether a morsel arrived via try_next or
    // next(); a morsel entirely outside [offset_, offset_+len_) is skipped
    // (loop continues) with no output either way, exactly like FilterCursor.
    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        while (emitted_ < len_) {
            std::optional<Morsel> m;
            if (!in_->try_next(max_rows, m)) return false;
            if (!m) {
                out.reset();
                return true;
            }
            std::optional<Morsel> windowed = apply(*m);
            if (windowed) {
                out = std::move(windowed);
                return true;
            }
        }
        out.reset();
        return true;
    }

   private:
    std::optional<Morsel> apply(const Morsel& m) {
        const std::int64_t start = seen_;
        seen_ += m.rows;
        const std::int64_t w_start = std::max(offset_, start);
        const std::int64_t w_end = std::min(offset_ + len_, seen_);
        if (w_end <= w_start) return std::nullopt;
        emitted_ += w_end - w_start;
        return slice_morsel(m, w_start - start, w_end - w_start);
    }

    std::unique_ptr<Cursor> in_;
    std::int64_t offset_, len_;
    std::int64_t seen_ = 0, emitted_ = 0;
};

// Keeps the last n rows: consumes the input streaming, holding at most n rows
// (plus one morsel) in a ring, then emits the tail once.
class TailCursor : public Cursor {
   public:
    TailCursor(std::unique_ptr<Cursor> in, std::int64_t n)
        : in_(std::move(in)), n_(n) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        if (n_ <= 0) {
            done_ = true;
            co_return std::nullopt;
        }
        while (auto m = co_await in_->next(max_rows)) accumulate(std::move(*m));
        done_ = true;
        co_return finish();
    }

    // The ring-buffer fold below (accumulate) is a pure function of the
    // ORDERED sequence of upstream morsels: it does not care whether a given
    // morsel arrived via try_next or next(), only the order they arrived in.
    // buf_/total_ are members, so a partial synchronous drain here leaves
    // exactly the state next() would have reached pulling the same prefix via
    // co_await, and next() (or another try_next call) resumes the same fold
    // from there - never a re-drain, never a skipped morsel.
    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        if (done_) {
            out.reset();
            return true;
        }
        if (n_ <= 0) {
            done_ = true;
            out.reset();
            return true;
        }
        for (;;) {
            std::optional<Morsel> m;
            if (!in_->try_next(max_rows, m)) return false;
            if (!m) break;
            accumulate(std::move(*m));
        }
        done_ = true;
        out = finish();
        return true;
    }

   private:
    void accumulate(Morsel&& m) {
        total_ += m.rows;
        buf_.push_back(std::move(m));
        while (!buf_.empty() && total_ - buf_.front().rows >= n_) {
            total_ -= buf_.front().rows;
            buf_.pop_front();
        }
    }

    std::optional<Morsel> finish() {
        if (buf_.empty()) return std::nullopt;
        const std::size_t ncols = buf_.front().columns.size();
        Morsel out;
        out.rows = total_;
        out.columns.reserve(ncols);
        for (std::size_t c = 0; c < ncols; ++c) {
            std::vector<const Series*> parts;
            parts.reserve(buf_.size());
            for (Morsel& m : buf_) parts.push_back(&m.columns[c]);
            out.columns.push_back(concat_columns(parts));
        }
        if (total_ > n_) return slice_morsel(out, total_ - n_, n_);
        return out;
    }

    std::unique_ptr<Cursor> in_;
    std::int64_t n_;
    bool done_ = false;
    std::deque<Morsel> buf_;
    std::int64_t total_ = 0;
};

// Wrap a morsel's columns in a DataFrame (dummy names), apply `fn`, and return
// the result materialized FLAT.
template <class Fn>
Morsel map_frame(Morsel&& m, Fn&& fn) {
    DataFrame tmp;
    tmp.names.assign(m.columns.size(), std::string());
    for (Series& c : m.columns) tmp.columns.push_back(std::move(c));
    DataFrame r = fn(std::move(tmp));
    Morsel out;
    out.columns.reserve(r.columns.size());
    for (const Series& c : r.columns) out.columns.push_back(c.materialize());
    out.rows = out.columns.empty() ? 0 : out.columns.front().length();
    return out;
}

// Drops rows null in any column; skips fully-dropped morsels.
class DropNullsCursor : public Cursor {
   public:
    explicit DropNullsCursor(std::unique_ptr<Cursor> in) : in_(std::move(in)) {}
    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        while (auto m = co_await in_->next(max_rows)) {
            Morsel out = map_frame(std::move(*m),
                                   [](DataFrame f) { return f.drop_nulls(); });
            if (out.rows > 0) co_return out;
        }
        co_return std::nullopt;
    }

    // A morsel dropped to zero rows contributes nothing, same argument as
    // FilterCursor::try_next.
    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        for (;;) {
            std::optional<Morsel> m;
            if (!in_->try_next(max_rows, m)) return false;
            if (!m) {
                out.reset();
                return true;
            }
            Morsel filtered = map_frame(
                std::move(*m), [](DataFrame f) { return f.drop_nulls(); });
            if (filtered.rows > 0) {
                out = std::move(filtered);
                return true;
            }
        }
    }

    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        co_return co_await in_->narrow(predicate);
    }

   private:
    std::unique_ptr<Cursor> in_;
};

// Fills nulls per morsel.
class FillNullCursor : public Cursor {
   public:
    FillNullCursor(std::unique_ptr<Cursor> in, dftu_scalar value)
        : in_(std::move(in)), value_(value) {}
    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return std::nullopt;
        co_return apply(std::move(*m));
    }

    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        std::optional<Morsel> m;
        if (!in_->try_next(max_rows, m)) return false;
        out = m ? std::optional<Morsel>(apply(std::move(*m))) : std::nullopt;
        return true;
    }

   private:
    // Filling nulls keeps rows and columns in place, but nulls sort last
    // (cmp_cell); replacing them with a concrete value can move what was the
    // tail of the ordered column anywhere, so the claim only survives when
    // that column had no nulls to fill.
    Morsel apply(Morsel&& m) {
        const std::int64_t bi = m.batch_index;
        const Ordering ord = m.ordering;
        const std::int32_t ordered_column = m.ordered_column;
        const bool ordered_descending = m.ordered_descending;
        const bool ordered_col_had_nulls =
            ord == Ordering::ByColumn &&
            m.columns[static_cast<std::size_t>(ordered_column)].null_count() >
                0;
        Morsel out = map_frame(
            std::move(m), [&](DataFrame f) { return f.fill_null(value_); });
        out.batch_index = bi;
        out.ordering = ordered_col_had_nulls ? Ordering::Unordered : ord;
        out.ordered_column = ordered_column;
        out.ordered_descending = ordered_descending;
        return out;
    }

    std::unique_ptr<Cursor> in_;
    dftu_scalar value_;
};

// Prepends a global Int64 row-index column.
class WithRowIndexCursor : public Cursor {
   public:
    explicit WithRowIndexCursor(std::unique_ptr<Cursor> in)
        : in_(std::move(in)) {}
    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return std::nullopt;
        co_return apply(std::move(*m));
    }

    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        std::optional<Morsel> m;
        if (!in_->try_next(max_rows, m)) return false;
        out = m ? std::optional<Morsel>(apply(std::move(*m))) : std::nullopt;
        return true;
    }

   private:
    // This cursor pulls upstream strictly sequentially (one next() at a
    // time, no fan-out), so the counter it prepends is genuinely
    // monotonically increasing across the whole output regardless of what
    // upstream claimed - always the stronger, always-true claim.
    Morsel apply(Morsel&& m) {
        std::vector<std::int64_t> idx(static_cast<std::size_t>(m.rows));
        for (std::int64_t i = 0; i < m.rows; ++i) idx[i] = pos_ + i;
        pos_ += m.rows;
        Morsel out;
        out.rows = m.rows;
        out.batch_index = m.batch_index;
        out.ordering = Ordering::ByColumn;
        out.ordered_column = 0;
        out.ordered_descending = false;
        out.columns.reserve(m.columns.size() + 1);
        out.columns.push_back(Series::flat_i64(idx.data(), m.rows));
        for (Series& c : m.columns) out.columns.push_back(std::move(c));
        return out;
    }

   public:
    // The index column is prepended, so input column i sits at i + 1; a
    // predicate on the index itself cannot go up (pruning renumbers it).
    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        if (expr_references(predicate, 0)) co_return false;
        const std::int32_t top = expr_max_col(predicate);
        if (top < 1) co_return co_await in_->narrow(predicate);
        std::vector<std::int32_t> map(static_cast<std::size_t>(top) + 1, 0);
        for (std::int32_t i = 1; i <= top; ++i)
            map[static_cast<std::size_t>(i)] = i - 1;
        co_return co_await in_->narrow(expr_remap_cols(predicate, map));
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::int64_t pos_ = 0;
};

// Per-column null counts, accumulated streaming, emitted as one row.
class NullCountCursor : public Cursor {
   public:
    explicit NullCountCursor(std::unique_ptr<Cursor> in) : in_(std::move(in)) {}
    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        done_ = true;
        std::vector<std::int64_t> counts;
        while (auto m = co_await in_->next(max_rows)) {
            if (counts.empty()) counts.assign(m->columns.size(), 0);
            for (std::size_t i = 0; i < m->columns.size(); ++i)
                counts[i] += m->columns[i].null_count();
        }
        if (counts.empty()) co_return std::nullopt;
        Morsel out;
        out.rows = 1;
        out.columns.reserve(counts.size());
        for (std::int64_t& c : counts)
            out.columns.push_back(Series::flat_i64(&c, 1));
        co_return out;
    }

   private:
    std::unique_ptr<Cursor> in_;
    bool done_ = false;
};

// Wrap a morsel's columns in a DataFrame with real `names`, apply `fn`, return
// the result materialized FLAT.
template <class Fn>
Morsel frame_op(Morsel&& m, const std::vector<std::string>& names, Fn&& fn) {
    DataFrame tmp;
    tmp.names = names;
    for (Series& c : m.columns) tmp.columns.push_back(std::move(c));
    DataFrame r = fn(std::move(tmp));
    Morsel out;
    out.rows = r.num_rows();
    out.columns.reserve(r.columns.size());
    for (const Series& c : r.columns) out.columns.push_back(c.materialize());
    return out;
}

// Unnests a List column per morsel (streaming): DataFrame::unnest, with the
// flattened names reported for a plan whose schema could not say them.
class UnnestCursor : public Cursor {
   public:
    UnnestCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                 std::string column, bool keep_empty)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          column_(std::move(column)),
          keep_empty_(keep_empty) {}
    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return std::nullopt;
        co_return apply(std::move(*m));
    }
    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        std::optional<Morsel> m;
        if (!in_->try_next(max_rows, m)) return false;
        out = m ? std::optional<Morsel>(apply(std::move(*m))) : std::nullopt;
        return true;
    }
    std::optional<std::vector<std::string>> out_names() const override {
        return out_names_;
    }

   private:
    Morsel apply(Morsel m) {
        DataFrame f;
        f.names = sch_;
        f.columns = std::move(m.columns);
        DataFrame r = f.unnest(column_, keep_empty_);
        out_names_ = r.names;
        return morsel_of(std::move(r));
    }
    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::string column_;
    bool keep_empty_;
    std::optional<std::vector<std::string>> out_names_;
};

// Explodes a List column per morsel (streaming).
class ExplodeCursor : public Cursor {
   public:
    ExplodeCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                  std::string column)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          column_(std::move(column)) {}
    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return std::nullopt;
        co_return frame_op(std::move(*m), sch_,
                           [&](DataFrame f) { return f.explode(column_); });
    }

    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        std::optional<Morsel> m;
        if (!in_->try_next(max_rows, m)) return false;
        out = m ? std::optional<Morsel>(
                      frame_op(std::move(*m), sch_,
                               [&](DataFrame f) { return f.explode(column_); }))
                : std::nullopt;
        return true;
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::string column_;
};

// Reshapes wide->long per morsel (streaming).
class UnpivotCursor : public Cursor {
   public:
    UnpivotCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                  std::vector<std::string> id, std::vector<std::string> val)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          id_(std::move(id)),
          val_(std::move(val)) {}
    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return std::nullopt;
        co_return frame_op(std::move(*m), sch_,
                           [&](DataFrame f) { return f.unpivot(id_, val_); });
    }

    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        std::optional<Morsel> m;
        if (!in_->try_next(max_rows, m)) return false;
        out = m ? std::optional<Morsel>(frame_op(
                      std::move(*m), sch_,
                      [&](DataFrame f) { return f.unpivot(id_, val_); }))
                : std::nullopt;
        return true;
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_, id_, val_;
};

// The k rows with the largest/smallest `name`: keep a running best of <= k
// rows, re-topk after each morsel (bounded state), emit once. Streaming
// ingestion.
class TopkCursor : public Cursor {
   public:
    TopkCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
               std::string name, std::int64_t k, bool largest)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          name_(std::move(name)),
          k_(k),
          largest_(largest) {}
    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        done_ = true;
        DataFrame best;
        bool has = false;
        while (auto m = co_await in_->next(max_rows)) {
            DataFrame cur;
            cur.names = sch_;
            for (Series& c : m->columns) cur.columns.push_back(std::move(c));
            if (!has) {
                best = cur.topk(name_, k_, largest_);
                has = true;
            } else {
                DataFrame u = concat({&best, &cur});
                best = u.topk(name_, k_, largest_);
            }
        }
        if (!has) co_return std::nullopt;
        Morsel out;
        out.rows = best.num_rows();
        out.columns.reserve(best.columns.size());
        for (const Series& c : best.columns)
            out.columns.push_back(c.materialize());
        co_return out;
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::string name_;
    std::int64_t k_;
    bool largest_;
    bool done_ = false;
};

// One run: single-group AggState blobs (agg_extract_group + agg_serialize),
// length-prefixed, in ascending composite-key order (agg_sort_groups). The
// on-disk unit a bounded k-way merge reads back one group at a time.
// Streaming group-by: fold every morsel into one mergeable AggState. When the
// accumulated state exceeds `budget`, flush it to a sorted-by-key run on disk
// and start a fresh state (mirrors SortMergeCursor's external merge sort). No
// spill needed: finalize the single in-memory state directly (unchanged
// behavior). Spilled: k-way merge the runs, combining equal composite keys,
// emitting rows bounded by max_rows per call.
class GroupByCursor : public Cursor {
   public:
    GroupByCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                  std::vector<std::string> keys, std::vector<GroupAgg> aggs,
                  std::uint64_t budget, std::vector<AggDynSpec> dyn = {},
                  std::string dyn_prefix = {})
        : in_(std::move(in)),
          sch_(std::move(sch)),
          keys_(std::move(keys)),
          aggs_(std::move(aggs)),
          budget_(budget),
          dyn_specs_(std::move(dyn)),
          dyn_prefix_(std::move(dyn_prefix)) {}

    std::optional<std::vector<std::string>> out_names() const override {
        return out_names_;
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!built_) co_await build(max_rows);
        if (!spilled_) {
            if (done_) co_return std::nullopt;
            done_ = true;
            co_return std::move(result_);
        }
        co_return merge_next(max_rows);
    }

   private:
    static int index_in(const std::vector<std::string>& s,
                        const std::string& n) {
        auto it = std::find(s.begin(), s.end(), n);
        return it == s.end() ? -1 : static_cast<int>(it - s.begin());
    }

    static Morsel to_morsel(const DataFrame& r) {
        Morsel out;
        out.rows = r.num_rows();
        out.columns.reserve(r.columns.size());
        for (const Series& c : r.columns)
            out.columns.push_back(c.materialize());
        return out;
    }

    coro::CoroTask<void> build(std::int64_t max_rows) {
        std::vector<int> key_idx;
        key_idx.reserve(keys_.size());
        for (const std::string& k : keys_) key_idx.push_back(index_in(sch_, k));
        std::vector<int> value_idx;  // sch indices of the deduped value columns
        ankerl::unordered_dense::map<std::string, std::int32_t> dedup;
        auto resolve = [&](const std::string& name) -> std::int32_t {
            auto it = dedup.find(name);
            if (it != dedup.end()) return it->second;
            const std::int32_t idx =
                static_cast<std::int32_t>(value_idx.size());
            value_idx.push_back(index_in(sch_, name));
            dedup.emplace(name, idx);
            return idx;
        };
        specs_.reserve(aggs_.size());
        for (const GroupAgg& a : aggs_) {
            AggSpec sp;
            sp.op = to_agg_op(a.op);
            sp.out = a.out;
            sp.param = a.param;
            sp.value_col = sp.op == AggOp::Count ? -1 : resolve(a.column);
            if (agg_uses_by_col(sp.op)) sp.by_col = resolve(a.by);
            specs_.push_back(std::move(sp));
        }

        // A resident source carries dyn columns in-band (prefix-tagged in
        // sch_); a streaming source carries them out of band (Morsel::dyn_*).
        std::vector<std::pair<int, std::string>> sch_dyn;
        if (!dyn_specs_.empty() && !dyn_prefix_.empty())
            for (std::size_t i = 0; i < sch_.size(); ++i)
                if (sch_[i].rfind(dyn_prefix_, 0) == 0)
                    sch_dyn.emplace_back(static_cast<int>(i),
                                         sch_[i].substr(dyn_prefix_.size()));

        AggStatePtr state = agg_new(specs_, dyn_specs_);
        // Bounded parallel sink: pull a batch of morsels, accumulate each into
        // its own partial AggState in parallel (the mergeable agg IR), then
        // merge the partials into the running state. Memory stays bounded to
        // one batch; a serial-pull single morsel skips the fan-out.
        constexpr std::size_t BATCH = 32;
        std::vector<Morsel> batch;
        batch.reserve(BATCH);
        bool eof = false;
        int run_id = 0;
        while (!eof) {
            batch.clear();
            for (std::size_t b = 0; b < BATCH; ++b) {
                auto m = co_await in_->next(max_rows);
                if (!m) {
                    eof = true;
                    break;
                }
                batch.push_back(std::move(*m));
            }
            if (batch.empty()) break;
            auto accumulate = [&](AggState& st, const Morsel& m) {
                std::vector<const Series*> keys;
                keys.reserve(key_idx.size());
                for (int ki : key_idx) keys.push_back(&m.columns[ki]);
                std::vector<const Series*> values;
                values.reserve(value_idx.size());
                for (int vi : value_idx) values.push_back(&m.columns[vi]);
                if (dyn_specs_.empty()) {
                    agg_accumulate(st, keys, values);
                    return;
                }
                std::vector<AggDynInput> dyn;
                dyn.reserve(sch_dyn.size() + m.dyn_columns.size());
                for (const auto& [ci, name] : sch_dyn)
                    dyn.push_back(
                        {name, &m.columns[static_cast<std::size_t>(ci)]});
                for (std::size_t i = 0; i < m.dyn_columns.size(); ++i) {
                    const std::string& raw = m.dyn_names[i];
                    std::string name = raw.rfind(dyn_prefix_, 0) == 0
                                           ? raw.substr(dyn_prefix_.size())
                                           : raw;
                    dyn.push_back({std::move(name), &m.dyn_columns[i]});
                }
                agg_accumulate(st, keys, values, dyn);
            };
            if (batch.size() == 1) {
                accumulate(*state, batch[0]);
            } else {
                std::vector<AggStatePtr> partials(batch.size());
                parallel_for(
                    static_cast<std::int64_t>(batch.size()), 1,
                    [&](std::int64_t bi, std::int64_t ei) {
                        for (std::int64_t j = bi; j < ei; ++j) {
                            auto st = agg_new(specs_, dyn_specs_);
                            accumulate(*st, batch[static_cast<std::size_t>(j)]);
                            partials[static_cast<std::size_t>(j)] =
                                std::move(st);
                        }
                    });
                for (auto& p : partials)
                    if (p) agg_merge(*state, *p);
            }
            if (budget_ > 0 && agg_approx_bytes(*state) > budget_) {
                spill::write_agg_run(*state, dir_.run_path(run_id++));
                state = agg_new(specs_, dyn_specs_);
            }
        }

        if (run_id == 0) {
            DataFrame r = agg_finalize(*state, keys_);
            out_names_ = r.names;
            result_ = to_morsel(r);
            spilled_ = false;
        } else {
            if (agg_num_groups(*state) > 0)
                spill::write_agg_run(*state, dir_.run_path(run_id++));
            runs_.reserve(static_cast<std::size_t>(run_id));
            for (int i = 0; i < run_id; ++i)
                runs_.push_back(
                    std::make_unique<spill::AggRunReader>(dir_.run_path(i)));
            spilled_ = true;
        }
        built_ = true;
    }

    // Merge every run sharing the smallest composite key into `merged`;
    // distinct keys arrive ascending, so groups append in order. One
    // agg_finalize at the end (not per group + concat_columns) lets a nested
    // Hist column, which concat_columns cannot rejoin, survive spill.
    std::optional<Morsel> merge_next(std::int64_t max_rows) {
        // The dyn column set is the global name union, so a k-way streamed
        // emission would give per-batch-varying dyn columns that cannot
        // vertically concat. Merge all runs into one state, finalize once.
        if (!dyn_specs_.empty()) {
            if (dyn_drained_) return std::nullopt;
            dyn_drained_ = true;
            AggStatePtr merged = agg_new(specs_, dyn_specs_);
            for (auto& run : runs_)
                while (run->valid()) {
                    agg_merge(*merged, run->state());
                    run->advance();
                }
            if (agg_num_groups(*merged) == 0) return std::nullopt;
            DataFrame r = agg_finalize(*merged, keys_);
            out_names_ = r.names;
            return to_morsel(r);
        }
        AggStatePtr merged = agg_new(specs_, dyn_specs_);
        std::int64_t produced = 0;
        while (produced < max_rows) {
            int best = -1;
            for (std::size_t i = 0; i < runs_.size(); ++i) {
                if (!runs_[i]->valid()) continue;
                if (best < 0 ||
                    agg_key_cmp(runs_[i]->state(), 0,
                                runs_[static_cast<std::size_t>(best)]->state(),
                                0) < 0)
                    best = static_cast<int>(i);
            }
            if (best < 0) break;
            // Snapshot the winning key before merging: advancing `best`'s own
            // reader mid-loop would otherwise mutate the very state later
            // iterations compare against.
            const AggStatePtr win_key = agg_extract_group(
                runs_[static_cast<std::size_t>(best)]->state(), 0);
            for (auto& run : runs_) {
                if (!run->valid()) continue;
                if (agg_key_cmp(run->state(), 0, *win_key, 0) != 0) continue;
                agg_merge(*merged, run->state());
                run->advance();
            }
            ++produced;
        }
        if (produced == 0) return std::nullopt;
        DataFrame r = agg_finalize(*merged, keys_);
        out_names_ = r.names;
        return to_morsel(r);
    }

    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::vector<std::string> keys_;
    std::vector<GroupAgg> aggs_;
    std::uint64_t budget_;
    std::vector<AggDynSpec> dyn_specs_;
    std::string dyn_prefix_;
    std::vector<AggSpec> specs_;
    std::optional<std::vector<std::string>> out_names_;
    bool built_ = false;
    bool done_ = false;
    bool spilled_ = false;
    bool dyn_drained_ = false;
    Morsel result_;
    spill::Dir dir_;
    std::vector<std::unique_ptr<spill::AggRunReader>> runs_;
};

// Streaming tumbling/sliding time-window aggregation over an ascending Int64
// time column. Explodes each event into the windows it falls in and feeds one
// mergeable agg state, so state is bounded by the window count (the output) not
// the input. Requires ascending time: the grid is anchored on the first event
// (= the minimum), matching DataFrame::group_by_dynamic.
class GroupByDynamicCursor : public Cursor {
   public:
    GroupByDynamicCursor(std::unique_ptr<Cursor> in,
                         std::vector<std::string> sch, std::string time_col,
                         std::int64_t every, std::int64_t period,
                         std::vector<GroupAgg> aggs, std::int64_t origin,
                         bool origin_min)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          time_col_(std::move(time_col)),
          every_(every),
          period_(period),
          aggs_(std::move(aggs)),
          origin_(origin),
          origin_min_(origin_min) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        done_ = true;
        if (every_ <= 0)
            throw std::invalid_argument("group_by_dynamic: every must be > 0");
        const std::int64_t period = period_ <= 0 ? every_ : period_;

        const int ti = index_in(sch_, time_col_);
        if (ti < 0)
            throw std::out_of_range("group_by_dynamic: no column named " +
                                    time_col_);

        std::vector<AggSpec> specs;
        std::vector<int> value_idx;
        ankerl::unordered_dense::map<std::string, std::int32_t> dedup;
        auto resolve = [&](const std::string& name) -> std::int32_t {
            auto it = dedup.find(name);
            if (it != dedup.end()) return it->second;
            const std::int32_t idx =
                static_cast<std::int32_t>(value_idx.size());
            value_idx.push_back(index_in(sch_, name));
            dedup.emplace(name, idx);
            return idx;
        };
        specs.reserve(aggs_.size());
        for (const GroupAgg& a : aggs_) {
            AggSpec sp;
            sp.op = to_agg_op(a.op);
            sp.out = a.out;
            sp.param = a.param;
            sp.value_col = sp.op == AggOp::Count ? -1 : resolve(a.column);
            if (agg_uses_by_col(sp.op)) sp.by_col = resolve(a.by);
            specs.push_back(std::move(sp));
        }

        AggStatePtr state = agg_new(specs);
        bool anchored = false;
        std::int64_t start0 = 0, origin = origin_;
        while (auto m = co_await in_->next(max_rows)) {
            const Series& tc = m->columns[static_cast<std::size_t>(ti)];
            if (tc.type() != TypeId::Int64)
                throw std::invalid_argument("group_by_dynamic: " + time_col_ +
                                            " must be an Int64 column");
            const std::int64_t n = m->rows;
            const std::int64_t* t = tc.data<std::int64_t>();
            if (!anchored) {
                for (std::int64_t i = 0; i < n; ++i)
                    if (!tc.is_null(i)) {
                        if (origin_min_) origin = t[i];
                        start0 =
                            origin + floor_to_multiple(t[i] - origin, every_);
                        anchored = true;
                        break;
                    }
                if (!anchored) continue;
            }
            std::vector<std::int64_t> keyv, rowsv;
            for (std::int64_t i = 0; i < n; ++i) {
                if (tc.is_null(i)) continue;
                const std::int64_t ts = t[i];
                std::int64_t k = (ts - start0) / every_;
                for (; k >= 0; --k) {
                    const std::int64_t s = start0 + k * every_;
                    if (s <= ts - period) break;
                    keyv.push_back(s);
                    rowsv.push_back(i);
                }
            }
            if (keyv.empty()) continue;
            Series keyc = Series::flat_i64(
                keyv.data(), static_cast<std::int64_t>(keyv.size()));
            std::vector<Series> gathered;
            gathered.reserve(value_idx.size());
            for (int vi : value_idx)
                gathered.push_back(
                    m->columns[static_cast<std::size_t>(vi)].take(rowsv));
            std::vector<const Series*> values;
            values.reserve(gathered.size());
            for (const Series& g : gathered) values.push_back(&g);
            agg_accumulate(*state, keyc, values);
        }
        DataFrame r = agg_finalize(*state, time_col_).sort_by(time_col_, false);
        co_return morsel_of(std::move(r));
    }

   private:
    static int index_in(const std::vector<std::string>& s,
                        const std::string& n) {
        auto it = std::find(s.begin(), s.end(), n);
        return it == s.end() ? -1 : static_cast<int>(it - s.begin());
    }
    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::string time_col_;
    std::int64_t every_, period_;
    std::vector<GroupAgg> aggs_;
    std::int64_t origin_;
    bool origin_min_;
    bool done_ = false;
};

// Streaming min-hash reservoir: keep the n rows with the smallest
// mix64(global_row_index + seed) keys, matching DataFrame::sample. Bounded to n
// rows (plus one morsel) regardless of input size; emits them in original row
// order.
class SampleCursor : public Cursor {
   public:
    SampleCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                 std::int64_t n, std::uint64_t seed)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          n_(std::max<std::int64_t>(n, 0)),
          seed_(seed) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        done_ = true;
        DataFrame best;                   // <= n_ rows
        std::vector<std::uint64_t> keys;  // parallel to best's rows
        std::vector<std::int64_t> idx;    // original global row indices
        std::int64_t off = 0;
        while (auto m = co_await in_->next(max_rows)) {
            const std::int64_t mrows = m->rows;
            DataFrame mf;
            mf.names = sch_;
            mf.columns = std::move(m->columns);
            DataFrame combined =
                best.num_rows() == 0
                    ? std::move(mf)
                    : concat({&best, &mf}, ConcatHow::Vertical);
            keys.reserve(keys.size() + static_cast<std::size_t>(mrows));
            idx.reserve(idx.size() + static_cast<std::size_t>(mrows));
            for (std::int64_t i = 0; i < mrows; ++i) {
                keys.push_back(hash::splitmix64(
                    static_cast<std::uint64_t>(off + i) + seed_));
                idx.push_back(off + i);
            }
            off += mrows;
            const std::int64_t total = combined.num_rows();
            const std::int64_t keep = std::min(n_, total);
            std::vector<std::int64_t> sel(static_cast<std::size_t>(total));
            std::iota(sel.begin(), sel.end(), std::int64_t{0});
            if (keep < total)
                std::nth_element(sel.begin(), sel.begin() + keep, sel.end(),
                                 [&](std::int64_t a, std::int64_t b) {
                                     return keys[static_cast<std::size_t>(a)] <
                                            keys[static_cast<std::size_t>(b)];
                                 });
            sel.resize(static_cast<std::size_t>(keep));
            best = take(combined, sel);
            std::vector<std::uint64_t> nk(static_cast<std::size_t>(keep));
            std::vector<std::int64_t> ni(static_cast<std::size_t>(keep));
            for (std::int64_t j = 0; j < keep; ++j) {
                nk[static_cast<std::size_t>(j)] = keys[static_cast<std::size_t>(
                    sel[static_cast<std::size_t>(j)])];
                ni[static_cast<std::size_t>(j)] = idx[static_cast<std::size_t>(
                    sel[static_cast<std::size_t>(j)])];
            }
            keys = std::move(nk);
            idx = std::move(ni);
        }
        // DataFrame::sample returns survivors in original row order.
        std::vector<std::int64_t> ord(
            static_cast<std::size_t>(best.num_rows()));
        std::iota(ord.begin(), ord.end(), std::int64_t{0});
        std::sort(ord.begin(), ord.end(), [&](std::int64_t a, std::int64_t b) {
            return idx[static_cast<std::size_t>(a)] <
                   idx[static_cast<std::size_t>(b)];
        });
        co_return morsel_of(take(best, ord));
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::int64_t n_;
    std::uint64_t seed_;
    bool done_ = false;
};

// External merge sort. Generates sorted runs bounded by `budget` bytes (spilled
// to disk; budget 0 keeps one in-memory run), then k-way range-merges them into
// a sorted stream. Peak memory is O(budget + one output morsel) when spilling.
class SortMergeCursor : public Cursor {
   public:
    SortMergeCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                    std::string key, bool descending, std::uint64_t budget)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          key_(std::move(key)),
          descending_(descending),
          budget_(budget) {}

    // Sorting commutes with a row-wise narrowing, and the input is untouched
    // until the first next(), which is when a join sends one.
    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        if (built_) co_return false;
        co_return co_await in_->narrow(predicate);
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!built_) co_await build(max_rows);

        std::vector<std::vector<Series>> pieces;
        std::int64_t out_rows = 0;
        while (out_rows < max_rows) {
            const int winner = pick(-1, false);
            if (winner < 0) break;
            const int bnd = pick(winner, true);
            Morsel& wm = *cur_[static_cast<std::size_t>(winner)];
            const Series& kw = wm.columns[static_cast<std::size_t>(key_idx_)];
            std::int64_t pos = pos_[static_cast<std::size_t>(winner)];
            const std::int64_t end =
                std::min(wm.rows, pos + (max_rows - out_rows));
            std::int64_t limit;
            if (bnd < 0) {
                limit = end;
            } else {
                const Morsel& bm = *cur_[static_cast<std::size_t>(bnd)];
                const Series& kb =
                    bm.columns[static_cast<std::size_t>(key_idx_)];
                const std::int64_t bpos = pos_[static_cast<std::size_t>(bnd)];
                limit = pos;
                while (limit < end &&
                       cmp_cell(kw, limit, kb, bpos, descending_) <= 0)
                    ++limit;
            }
            Morsel piece = slice_morsel(wm, pos, limit - pos);
            pieces.push_back(std::move(piece.columns));
            out_rows += limit - pos;
            pos_[static_cast<std::size_t>(winner)] = limit;
            if (limit >= wm.rows) co_await advance(winner, max_rows);
        }
        if (pieces.empty()) co_return std::nullopt;
        Morsel out;
        out.rows = out_rows;
        const std::size_t ncols = pieces.front().size();
        out.columns.reserve(ncols);
        for (std::size_t c = 0; c < ncols; ++c) {
            std::vector<const Series*> parts;
            parts.reserve(pieces.size());
            for (auto& pc : pieces) parts.push_back(&pc[c]);
            out.columns.push_back(concat_columns(parts));
        }
        // The k-way merge always picks the globally smallest key across the
        // whole call sequence (pos_/cur_ persist between next() calls), so
        // the concatenation of every morsel this cursor ever returns is
        // sorted by key_, not just each one in isolation.
        out.ordering = Ordering::ByColumn;
        out.ordered_column = key_idx_;
        out.ordered_descending = descending_;
        co_return out;
    }

   private:
    // Index of the active run whose current key is most "before" the rest;
    // `exclude` skips one run (for the boundary), returns -1 if none active.
    int pick(int exclude, bool /*is_boundary*/) const {
        int best = -1;
        for (std::size_t k = 0; k < cur_.size(); ++k) {
            if (static_cast<int>(k) == exclude) continue;
            if (!cur_[k] || pos_[k] >= cur_[k]->rows) continue;
            if (best < 0) {
                best = static_cast<int>(k);
                continue;
            }
            const Series& kk =
                cur_[k]->columns[static_cast<std::size_t>(key_idx_)];
            const Series& kb =
                cur_[static_cast<std::size_t>(best)]
                    ->columns[static_cast<std::size_t>(key_idx_)];
            if (cmp_cell(kk, pos_[k], kb, pos_[static_cast<std::size_t>(best)],
                         descending_) < 0)
                best = static_cast<int>(k);
        }
        return best;
    }

    coro::CoroTask<void> advance(int k, std::int64_t max_rows) {
        cur_[static_cast<std::size_t>(k)] =
            co_await runs_[static_cast<std::size_t>(k)]->next(max_rows);
        pos_[static_cast<std::size_t>(k)] = 0;
    }

    DataFrame concat_pending(std::vector<std::vector<Series>>& pending) const {
        DataFrame buf;
        buf.names = sch_;
        buf.columns.reserve(sch_.size());
        for (std::size_t c = 0; c < sch_.size(); ++c) {
            std::vector<const Series*> parts;
            parts.reserve(pending.size());
            for (auto& ch : pending) parts.push_back(&ch[c]);
            buf.columns.push_back(concat_columns(parts));
        }
        return buf;
    }

    void spill_run(std::vector<std::vector<Series>>& pending, int id,
                   std::int64_t chunk) {
        DataFrame sorted = concat_pending(pending).sort_by(key_, descending_);
        spill::Writer w(dir_.run_path(id));
        const std::int64_t total = sorted.num_rows();
        for (std::int64_t off = 0; off < total; off += chunk) {
            const std::int64_t len = std::min(chunk, total - off);
            DataFrame s = sorted.slice(off, len);
            std::vector<Series> cols;
            cols.reserve(s.columns.size());
            for (const Series& c : s.columns) cols.push_back(c.materialize());
            w.write(cols, len);
        }
        w.close();
    }

    coro::CoroTask<void> build(std::int64_t max_rows) {
        key_idx_ = static_cast<int>(std::distance(
            sch_.begin(), std::find(sch_.begin(), sch_.end(), key_)));
        if (key_idx_ >= static_cast<int>(sch_.size()))
            throw std::out_of_range("sort_by: no column named " + key_);

        std::vector<std::vector<Series>> pending;
        std::size_t pend_bytes = 0;
        int run_id = 0;
        while (auto m = co_await in_->next(max_rows)) {
            pend_bytes += morsel_bytes(m->columns);
            pending.push_back(std::move(m->columns));
            if (budget_ > 0 && pend_bytes > budget_) {
                spill_run(pending, run_id++, max_rows);
                pending.clear();
                pend_bytes = 0;
            }
        }

        if (run_id == 0) {  // everything fits in memory: one sorted run
            DataFrame buf =
                pending.empty() ? DataFrame{} : concat_pending(pending);
            if (pending.empty()) buf.names = sch_;
            DataFrame sorted = buf.num_rows() ? buf.sort_by(key_, descending_)
                                              : std::move(buf);
            runs_.push_back(std::make_unique<InMemoryCursor>(
                std::make_shared<const DataFrame>(std::move(sorted))));
        } else {
            if (!pending.empty()) spill_run(pending, run_id++, max_rows);
            for (int id = 0; id < run_id; ++id)
                runs_.push_back(
                    std::make_unique<spill::Reader>(dir_.run_path(id)));
        }
        cur_.resize(runs_.size());
        pos_.assign(runs_.size(), 0);
        for (std::size_t k = 0; k < runs_.size(); ++k)
            cur_[k] = co_await runs_[k]->next(max_rows);
        built_ = true;
    }

    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::string key_;
    bool descending_;
    std::uint64_t budget_;
    bool built_ = false;
    int key_idx_ = 0;
    spill::Dir dir_;
    std::vector<std::unique_ptr<Cursor>> runs_;
    std::vector<std::optional<Morsel>> cur_;
    std::vector<std::int64_t> pos_;
};

// Streaming distinct (keep first occurrence, original order). Fast path holds
// only the set of distinct row keys - which is the result itself, materialized
// by collect anyway - and streams input and output morsel by morsel. When that
// key state would exceed `budget_`, switches to a grace-hash-distinct spill:
// every row still to come gets a global row-id, is hash-partitioned by key to
// disk (skipping any key already resolved by the fast path), each partition is
// deduped independently keeping the row with the minimum row-id (recursing
// with a depth-salted hash if a partition itself does not fit budget), and the
// survivors are k-way merged back into row-id order so the fast-emitted prefix
// and the spilled remainder together reproduce one globally first-occurrence,
// input-order stream.
class UniqueCursor : public Cursor {
   public:
    UniqueCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                 std::vector<std::int64_t> key_idx, std::uint64_t budget)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          key_idx_(std::move(key_idx)),
          budget_(budget) {
        if (parallel_backend_installed()) seen_p_.resize(DEDUP_PARTITIONS);
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!spilling_) {
            while (auto m = co_await in_->next(max_rows)) {
                const std::int64_t n = m->rows;
                // Build the exact row keys in parallel (scalar string work,
                // one per row, independent), then dedupe (radix-partitioned
                // when a parallel backend is installed, else one serial set).
                std::vector<std::string> keys(static_cast<std::size_t>(n));
                const std::vector<Series> kc = key_cols(m->columns);
                parallel_for(n, std::int64_t{1} << 13,
                             [&](std::int64_t b, std::int64_t e) {
                                 for (std::int64_t i = b; i < e; ++i)
                                     keys[static_cast<std::size_t>(i)] =
                                         row_key(kc, i);
                             });
                std::vector<std::uint32_t> key_len(static_cast<std::size_t>(n));
                for (std::int64_t i = 0; i < n; ++i)
                    key_len[static_cast<std::size_t>(i)] =
                        static_cast<std::uint32_t>(
                            keys[static_cast<std::size_t>(i)].size());
                std::vector<std::uint8_t> keep_mask =
                    first_seen_mask(keys, n, seen_, seen_p_);
                std::vector<std::int64_t> keep;
                keep.reserve(static_cast<std::size_t>(n));
                for (std::int64_t i = 0; i < n; ++i) {
                    if (!keep_mask[static_cast<std::size_t>(i)]) continue;
                    keep.push_back(i);
                    fast_bytes_ += key_len[static_cast<std::size_t>(i)] +
                                   DEDUP_ENTRY_OVERHEAD;
                }
                next_row_id_ += n;
                if (budget_ > 0 && fast_bytes_ > budget_) spilling_ = true;
                if (!keep.empty()) {
                    DataFrame mf;
                    mf.names = sch_;
                    mf.columns = std::move(m->columns);
                    co_return morsel_of(take(mf, keep));
                }
                if (spilling_) break;
            }
            if (!spilling_) co_return std::nullopt;
        }
        if (!drained_) co_await drain_and_finalize(max_rows);
        co_return co_await merge_next(max_rows);
    }

   private:
    // The columns the row key hashes: every data column, or the subset the
    // op names (positions against the plan schema).
    std::vector<Series> key_cols(const std::vector<Series>& data) const {
        std::vector<Series> out;
        if (key_idx_.empty()) {
            out.reserve(data.size());
            for (const Series& c : data) out.push_back(c.share());
        } else {
            out.reserve(key_idx_.size());
            for (std::int64_t k : key_idx_)
                out.push_back(data[static_cast<std::size_t>(k)].share());
        }
        return out;
    }

    bool already_seen(const std::string& key) const {
        if (!seen_p_.empty())
            return seen_p_[std::hash<std::string>{}(key) % seen_p_.size()]
                .contains(key);
        return seen_.contains(key);
    }

    std::vector<std::string> rowid_schema() const {
        std::vector<std::string> out;
        out.reserve(sch_.size() + 1);
        out.emplace_back("__row_id");
        for (const std::string& n : sch_) out.push_back(n);
        return out;
    }

    // Hash-partitions `cols` (row-id column first, then `data_cols` for the
    // key) into `fanout` on-disk runs by `unique_spill_hash(key, depth)`,
    // tracking each partition's approximate bytes and row count for the
    // recurse-or-leaf decision at finalize.
    void partition_rows(const std::vector<Series>& tagged_cols,
                        const std::vector<Series>& data_cols, std::int64_t n,
                        int depth, int fanout,
                        std::vector<spill::Writer>& writers,
                        std::vector<std::size_t>& bytes,
                        std::vector<std::int64_t>& rows) {
        std::vector<std::string> keys(static_cast<std::size_t>(n));
        const std::vector<Series> kc = key_cols(data_cols);
        parallel_for(n, std::int64_t{1} << 13,
                     [&](std::int64_t b, std::int64_t e) {
                         for (std::int64_t i = b; i < e; ++i)
                             keys[static_cast<std::size_t>(i)] = row_key(kc, i);
                     });
        std::vector<std::vector<std::int64_t>> buckets(
            static_cast<std::size_t>(fanout));
        for (std::int64_t i = 0; i < n; ++i) {
            if (depth == 0 && already_seen(keys[static_cast<std::size_t>(i)]))
                continue;  // already emitted by the fast phase
            const std::size_t p =
                unique_spill_hash(keys[static_cast<std::size_t>(i)], depth) %
                static_cast<std::size_t>(fanout);
            buckets[p].push_back(i);
        }
        DataFrame mf;
        mf.names = rowid_schema();
        mf.columns.reserve(tagged_cols.size());
        for (const Series& c : tagged_cols) mf.columns.push_back(c.share());
        for (int p = 0; p < fanout; ++p) {
            if (buckets[static_cast<std::size_t>(p)].empty()) continue;
            DataFrame sel = take(mf, buckets[static_cast<std::size_t>(p)]);
            bytes[static_cast<std::size_t>(p)] += morsel_bytes(sel.columns);
            rows[static_cast<std::size_t>(p)] += sel.num_rows();
            writers[static_cast<std::size_t>(p)].write(sel.columns,
                                                       sel.num_rows());
        }
    }

    // Dedups one spilled partition (rows whose key was not already resolved
    // by the fast phase, hash-partitioned to land here), keeping the row with
    // the minimum row-id per key. If the partition itself would still exceed
    // budget and is large enough that splitting helps, re-partitions it with
    // a depth-salted hash instead of loading it whole (bounded recursion: a
    // single hot key always lands in the same sub-partition however deep, so
    // depth alone cannot force it smaller - the row-count floor stops the
    // recursion once further splitting cannot shrink it).
    coro::CoroTask<void> finalize_partition(const std::string& path,
                                            std::size_t bytes,
                                            std::int64_t rows, int depth) {
        if (depth < UNIQUE_SPILL_MAX_DEPTH && budget_ > 0 && bytes > budget_ &&
            rows > UNIQUE_SPILL_MIN_LEAF_ROWS) {
            constexpr int FANOUT = UNIQUE_SPILL_FANOUT;
            std::vector<spill::Writer> writers;
            writers.reserve(static_cast<std::size_t>(FANOUT));
            std::vector<int> ids(static_cast<std::size_t>(FANOUT));
            for (int p = 0; p < FANOUT; ++p) {
                ids[static_cast<std::size_t>(p)] = next_run_id_++;
                writers.emplace_back(
                    dir_.run_path(ids[static_cast<std::size_t>(p)]));
            }
            std::vector<std::size_t> sub_bytes(static_cast<std::size_t>(FANOUT),
                                               0);
            std::vector<std::int64_t> sub_rows(static_cast<std::size_t>(FANOUT),
                                               0);
            spill::Reader reader(path);
            while (auto m = co_await reader.next(DEFAULT_MORSEL_ROWS)) {
                std::vector<Series> data_cols = drop_first_column(m->columns);
                partition_rows(m->columns, data_cols, m->rows, depth + 1,
                               FANOUT, writers, sub_bytes, sub_rows);
            }
            for (spill::Writer& w : writers) w.close();
            for (int p = 0; p < FANOUT; ++p)
                co_await finalize_partition(
                    dir_.run_path(ids[static_cast<std::size_t>(p)]),
                    sub_bytes[static_cast<std::size_t>(p)],
                    sub_rows[static_cast<std::size_t>(p)], depth + 1);
            co_return;
        }

        // Leaf: small enough to fit budget (or recursion bottomed out) - load
        // fully, dedupe by minimum row-id, sort survivors by row-id, and write
        // one run for the final k-way merge.
        std::vector<std::vector<Series>> parts;
        std::int64_t total = 0;
        {
            spill::Reader reader(path);
            while (auto m = co_await reader.next(DEFAULT_MORSEL_ROWS)) {
                total += m->rows;
                parts.push_back(std::move(m->columns));
            }
        }
        if (total == 0) co_return;

        const std::size_t ncols = parts.front().size();
        std::vector<Series> whole;
        whole.reserve(ncols);
        for (std::size_t c = 0; c < ncols; ++c) {
            std::vector<const Series*> pcs;
            pcs.reserve(parts.size());
            for (auto& pc : parts) pcs.push_back(&pc[c]);
            whole.push_back(concat_columns(pcs));
        }
        const std::vector<Series> data_cols =
            key_cols(drop_first_column(whole));
        const std::int64_t* rowid = whole[0].data<std::int64_t>();

        ankerl::unordered_dense::map<std::string, std::int64_t> best;
        for (std::int64_t i = 0; i < total; ++i) {
            std::string k = row_key(data_cols, i);
            auto it = best.find(k);
            if (it == best.end())
                best.emplace(std::move(k), i);
            else if (rowid[i] < rowid[it->second])
                it->second = i;
        }
        std::vector<std::int64_t> survivors;
        survivors.reserve(best.size());
        for (const auto& kv : best) survivors.push_back(kv.second);
        std::sort(survivors.begin(), survivors.end(),
                  [&](std::int64_t a, std::int64_t b) {
                      return rowid[a] < rowid[b];
                  });

        DataFrame mf;
        mf.names = rowid_schema();
        mf.columns = std::move(whole);
        DataFrame sorted = take(mf, survivors);

        const std::string run_path = dir_.run_path(next_run_id_++);
        spill::Writer w(run_path);
        w.write(sorted.columns, sorted.num_rows());
        w.close();
        survivor_runs_.push_back(std::make_unique<spill::Reader>(run_path));
    }

    // Fully drains the remaining input into UNIQUE_SPILL_FANOUT partitions
    // (skipping rows whose key the fast phase already resolved), then dedupes
    // and orders every partition's survivors for the k-way merge in
    // merge_next.
    coro::CoroTask<void> drain_and_finalize(std::int64_t max_rows) {
        constexpr int FANOUT = UNIQUE_SPILL_FANOUT;
        std::vector<spill::Writer> writers;
        writers.reserve(static_cast<std::size_t>(FANOUT));
        std::vector<int> ids(static_cast<std::size_t>(FANOUT));
        for (int p = 0; p < FANOUT; ++p) {
            ids[static_cast<std::size_t>(p)] = next_run_id_++;
            writers.emplace_back(
                dir_.run_path(ids[static_cast<std::size_t>(p)]));
        }
        std::vector<std::size_t> bytes(static_cast<std::size_t>(FANOUT), 0);
        std::vector<std::int64_t> rows(static_cast<std::size_t>(FANOUT), 0);

        while (auto m = co_await in_->next(max_rows)) {
            const std::int64_t n = m->rows;
            std::vector<std::int64_t> rowid(static_cast<std::size_t>(n));
            for (std::int64_t i = 0; i < n; ++i)
                rowid[static_cast<std::size_t>(i)] = next_row_id_ + i;
            next_row_id_ += n;
            std::vector<Series> tagged;
            tagged.reserve(m->columns.size() + 1);
            tagged.push_back(Series::flat_i64(rowid.data(), n));
            for (Series& c : m->columns) tagged.push_back(std::move(c));
            partition_rows(tagged, drop_first_column(tagged), n, 0, FANOUT,
                           writers, bytes, rows);
        }
        for (spill::Writer& w : writers) w.close();

        for (int p = 0; p < FANOUT; ++p)
            co_await finalize_partition(
                dir_.run_path(ids[static_cast<std::size_t>(p)]),
                bytes[static_cast<std::size_t>(p)],
                rows[static_cast<std::size_t>(p)], 0);

        cur_.resize(survivor_runs_.size());
        pos_.assign(survivor_runs_.size(), 0);
        for (std::size_t k = 0; k < survivor_runs_.size(); ++k)
            cur_[k] = co_await survivor_runs_[k]->next(max_rows);
        drained_ = true;
    }

    // K-way merges the row-id-sorted survivor runs into ascending row-id
    // order (rows are globally unique row-ids, so a one-row-at-a-time pick is
    // fine here - this spill-finalize path is rare, not the streaming fast
    // path), dropping the row-id helper column before emitting.
    coro::CoroTask<std::optional<Morsel>> merge_next(std::int64_t max_rows) {
        std::vector<std::vector<Series>> pieces;
        std::int64_t out_rows = 0;
        while (out_rows < max_rows) {
            int winner = -1;
            for (std::size_t k = 0; k < cur_.size(); ++k) {
                if (!cur_[k] || pos_[k] >= cur_[k]->rows) continue;
                if (winner < 0) {
                    winner = static_cast<int>(k);
                    continue;
                }
                const std::size_t wk = static_cast<std::size_t>(winner);
                const std::int64_t cand =
                    cur_[k]->columns[0].data<std::int64_t>()[pos_[k]];
                const std::int64_t best =
                    cur_[wk]->columns[0].data<std::int64_t>()[pos_[wk]];
                if (cand < best) winner = static_cast<int>(k);
            }
            if (winner < 0) break;
            const std::size_t wk = static_cast<std::size_t>(winner);
            Morsel piece = slice_morsel(*cur_[wk], pos_[wk], 1);
            piece.columns.erase(piece.columns.begin());
            pieces.push_back(std::move(piece.columns));
            ++out_rows;
            ++pos_[wk];
            if (pos_[wk] >= cur_[wk]->rows)
                cur_[wk] = co_await survivor_runs_[wk]->next(max_rows);
        }
        if (pieces.empty()) co_return std::nullopt;
        Morsel out;
        out.rows = out_rows;
        const std::size_t ncols = pieces.front().size();
        out.columns.reserve(ncols);
        for (std::size_t c = 0; c < ncols; ++c) {
            std::vector<const Series*> parts;
            parts.reserve(pieces.size());
            for (auto& pc : pieces) parts.push_back(&pc[c]);
            out.columns.push_back(concat_columns(parts));
        }
        co_return out;
    }

    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::vector<std::int64_t> key_idx_;
    std::uint64_t budget_;
    ankerl::unordered_dense::set<std::string> seen_;
    std::vector<ankerl::unordered_dense::set<std::string>> seen_p_;
    std::size_t fast_bytes_ = 0;
    std::int64_t next_row_id_ = 0;
    bool spilling_ = false;
    bool drained_ = false;
    spill::Dir dir_;
    int next_run_id_ = 0;
    std::vector<std::unique_ptr<Cursor>> survivor_runs_;
    std::vector<std::optional<Morsel>> cur_;
    std::vector<std::int64_t> pos_;
};

// A plugin-registered plan node (dftu_node_register): vt/self captured at the
// .op() call, args copied by value (see LazyFrame::op's Doxygen for the
// pointer-operand borrow contract).
struct NodeOp {
    std::string name;
    ::dftu_node_vt vt;
    void* self;
    ::dftu_op_arg args;
};

// Streaming per-column summary statistics, matching DataFrame::describe. One
// pass with O(numeric columns) state: count/null_count and running min/max plus
// Welford (mean, M2) for mean/sample-std. Output columns are data-dependent
// (one per numeric input column), reported via out_names().
class DescribeCursor : public Cursor {
   public:
    DescribeCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch)
        : in_(std::move(in)), sch_(std::move(sch)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        done_ = true;
        // Per-morsel SIMD reduction into one mergeable FieldStat per numeric
        // column (the engine's shared aggregation atom), so the numeric work is
        // vectorized and bounded by the column count.
        std::vector<int> num_idx;
        std::vector<FieldStat> acc;
        std::int64_t total_rows = 0;
        bool first = true;
        while (auto m = co_await in_->next(max_rows)) {
            if (first) {
                first = false;
                for (std::size_t c = 0; c < m->columns.size(); ++c)
                    if (is_numeric(m->columns[c].type()))
                        num_idx.push_back(static_cast<int>(c));
                acc.resize(num_idx.size());
            }
            total_rows += m->rows;
            for (std::size_t j = 0; j < num_idx.size(); ++j)
                acc[j].merge(field_stat_reduce(
                    m->columns[static_cast<std::size_t>(num_idx[j])]));
        }
        DataFrame out;
        out.names.push_back("statistic");
        out.columns.push_back(Series::strings(
            {"count", "null_count", "mean", "std", "min", "max"}));
        for (std::size_t j = 0; j < num_idx.size(); ++j) {
            const FieldStat& fs = acc[j];
            const double vals[6] = {
                static_cast<double>(fs.n),
                static_cast<double>(total_rows -
                                    static_cast<std::int64_t>(fs.n)),
                fs.mean(),
                fs.stddev(),
                fs.n ? fs.min : 0.0,
                fs.n ? fs.max : 0.0};
            out.columns.push_back(Series::flat_f64(vals, 6));
            out.names.push_back(sch_[static_cast<std::size_t>(num_idx[j])]);
        }
        produced_ = out.names;
        co_return morsel_of(std::move(out));
    }

    std::optional<std::vector<std::string>> out_names() const override {
        return produced_;
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    bool done_ = false;
    std::vector<std::string> produced_;
};

std::string describe_dftu_error(const ::dftu_error& err) {
    return err.message && *err.message ? err.message : "plan node failed";
}

// Adapts a host-native Cursor as a dftu_cursor_vt, the reverse of
// ProviderCursor (provider_registry.cpp): a registered node's open() pulls its
// input through this bridge exactly as it would pull from any other node's
// input, so the host and a plugin node speak the same cursor protocol in
// either direction. On success, ownership of `cursor_` passes to whichever
// side ends up calling destroy() (see dftu_node_vt::open's Doxygen); on a
// failed open() the caller still holds the owning unique_ptr and this object
// is destroyed normally.
class HostCursorBridge {
   public:
    HostCursorBridge(std::unique_ptr<Cursor> cursor,
                     std::vector<std::string> schema)
        : cursor_(std::move(cursor)), schema_(std::move(schema)) {}

    static const ::dftu_cursor_vt* vt() {
        static const ::dftu_cursor_vt v{
            &HostCursorBridge::next_thunk, &HostCursorBridge::destroy_thunk,
            &HostCursorBridge::narrow_thunk, &HostCursorBridge::bytes_thunk,
            &HostCursorBridge::reclaim_thunk};
        return &v;
    }
    void* self() { return this; }

   private:
    coro::CoroTask<void> next_task(std::int64_t max_rows,
                                   ::dftu_result_frame* out) {
        try {
            std::optional<Morsel> m = co_await cursor_->next(max_rows);
            if (!m) {
                out->ok = 1;
                out->u.value = nullptr;
                co_return;
            }
            DataFrame df =
                frame_from_morsel(std::move(*m), schema_, cursor_->out_names());
            out->ok = 1;
            out->u.value = dataframe_handle_wrap(std::move(df));
        } catch (const std::exception& e) {
            last_error_ = e.what();
            out->ok = 0;
            out->u.err =
                ::dftu_error{0, 0, DFTU_COND_INTERNAL, last_error_.c_str()};
        }
    }

    // Fills *out and returns true when cursor_->try_next answered without
    // suspending; false leaves *out untouched so the caller falls back to
    // next_task. Mirrors next_task's success/error/end-of-stream shapes
    // exactly, so a plugin node cannot tell which path answered it apart
    // from the NULL return.
    bool try_next_sync(std::int64_t max_rows, ::dftu_result_frame* out) {
        std::optional<Morsel> m;
        try {
            if (!cursor_->try_next(max_rows, m)) return false;
        } catch (const std::exception& e) {
            last_error_ = e.what();
            out->ok = 0;
            out->u.err =
                ::dftu_error{0, 0, DFTU_COND_INTERNAL, last_error_.c_str()};
            return true;
        }
        if (!m) {
            out->ok = 1;
            out->u.value = nullptr;
            return true;
        }
        DataFrame df =
            frame_from_morsel(std::move(*m), schema_, cursor_->out_names());
        out->ok = 1;
        out->u.value = dataframe_handle_wrap(std::move(df));
        return true;
    }

    static ::dftu_task* next_thunk(void* self, std::int64_t max_rows,
                                   ::dftu_result_frame* out) {
        auto* self_ = static_cast<HostCursorBridge*>(self);
        if (self_->try_next_sync(max_rows, out)) return nullptr;
        return task_to_abi(self_->next_task(max_rows, out));
    }
    static void destroy_thunk(void* self) {
        delete static_cast<HostCursorBridge*>(self);
    }

    coro::CoroTask<void> narrow_task(Expr predicate, std::int32_t* out) {
        bool applied = false;
        try {
            applied = co_await cursor_->narrow(predicate);
        } catch (const std::exception&) {
            applied = false;
        }
        *out = applied ? 1 : 0;
    }
    static ::dftu_task* narrow_thunk(void* self, const ::dftu_expr* predicate,
                                     std::int32_t* out_applied) {
        auto* self_ = static_cast<HostCursorBridge*>(self);
        if (out_applied) *out_applied = 0;
        if (!predicate || !out_applied) return nullptr;
        return task_to_abi(
            self_->narrow_task(expr_handle_unwrap(predicate), out_applied));
    }
    static std::uint64_t bytes_thunk(void* self) {
        return static_cast<HostCursorBridge*>(self)->cursor_->resident_bytes();
    }
    static std::uint64_t reclaim_thunk(void* self, std::uint64_t want) {
        auto* self_ = static_cast<HostCursorBridge*>(self);
        try {
            return self_->cursor_->reclaim(want).get();
        } catch (const std::exception&) {
            return 0;
        }
    }

    std::unique_ptr<Cursor> cursor_;
    std::vector<std::string> schema_;
    std::string last_error_;
};

// Drives a registered plugin node as one stage of the pull chain: opens the
// node's output cursor over `in` at construction (eagerly, like
// ProviderSource::scan), then pulls it exactly like any built-in cursor.
// Verifies every produced frame's column count against the node's own
// declared schema, so a node that lies about what it produces is caught here
// rather than corrupting whatever the optimizer built on the declaration.
class NodeCursor final : public Cursor {
   public:
    NodeCursor(const NodeOp& node, std::unique_ptr<Cursor> in,
               std::vector<std::string> in_schema, Schema declared_out)
        : name_(node.name), declared_(std::move(declared_out)) {
        auto bridge = std::make_unique<HostCursorBridge>(std::move(in),
                                                         std::move(in_schema));
        void* out_self = nullptr;
        const ::dftu_cursor_vt* out_vt = nullptr;
        void* ok =
            node.vt.open(node.self, bridge->self(), HostCursorBridge::vt(),
                         &node.args, &out_self, &out_vt);
        if (!ok || !out_vt)
            throw std::runtime_error("lazy op '" + name_ + "': open() failed");
        // The node took ownership of the input cursor on success: it (not us)
        // now owns destroying it, via out_vt_->destroy eventually reaching
        // HostCursorBridge::destroy_thunk.
        bridge.release();
        out_self_ = out_self;
        out_vt_ = out_vt;
    }

    ~NodeCursor() override {
        if (out_vt_ && out_vt_->destroy) out_vt_->destroy(out_self_);
    }

    NodeCursor(const NodeCursor&) = delete;
    NodeCursor& operator=(const NodeCursor&) = delete;

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        ::dftu_result_frame out{};
        if (::dftu_task* t = out_vt_->next(out_self_, max_rows, &out)) {
            auto* task = reinterpret_cast<coro::CoroTask<void>*>(t);
            co_await *task;
            delete task;
        }
        if (!DFTU_RESULT_OK(out))
            throw std::runtime_error(
                "lazy op '" + name_ +
                "': " + describe_dftu_error(DFTU_RESULT_ERROR(out)));

        ::dftu_dataframe* frame = DFTU_RESULT_VALUE(out);
        if (!frame) co_return std::nullopt;

        DataFrame df = dataframe_handle_take(frame);
        if (!declared_.fields.empty() &&
            df.columns.size() != declared_.fields.size())
            throw std::runtime_error("lazy op '" + name_ + "': produced " +
                                     std::to_string(df.columns.size()) +
                                     " columns but output_schema declared " +
                                     std::to_string(declared_.fields.size()));

        Morsel m;
        m.rows = df.num_rows();
        m.columns = std::move(df.columns);
        co_return m;
    }

    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        if (!out_vt_->narrow) co_return false;
        std::int32_t applied = 0;
        ::dftu_expr* h = expr_handle_wrap(predicate);
        ::dftu_task* t = out_vt_->narrow(out_self_, h, &applied);
        if (t) {
            auto* task = reinterpret_cast<coro::CoroTask<void>*>(t);
            co_await *task;
            delete task;
        }
        ::dftu_expr_free(h);
        co_return applied != 0;
    }
    std::uint64_t resident_bytes() const override {
        return out_vt_->bytes ? out_vt_->bytes(out_self_) : 0;
    }
    coro::CoroTask<std::uint64_t> reclaim(std::uint64_t want) override {
        co_return out_vt_->reclaim ? out_vt_->reclaim(out_self_, want) : 0;
    }

   private:
    std::string name_;
    Schema declared_;
    void* out_self_ = nullptr;
    const ::dftu_cursor_vt* out_vt_ = nullptr;
};

// Two-pass per-row mask: pass 1 counts each row key while spooling the input
// (RAM up to the budget, overflow to disk); pass 2 replays the spool in order,
// emitting count>1 (is_duplicated) or count==1 (is_unique) as one Bool column
// per morsel. Reads the upstream once; order-preserving; state is the count map
// plus the spool.
//
// Pass 1 is DISTINCT == GROUP BY all columns: it reuses the mergeable agg IR
// (AggOp::Count keyed by the row-key column) through the same bounded parallel
// sink as GroupByCursor - a batch of morsels, one partial AggState per morsel
// in parallel, merged serially - instead of a single hash map fed one row at a
// time. Pass 2's per-row lookup against the finalized (key, count) map is
// independent per row, so it fans out too; the output Bool column is bit-
// packed, so each parallel task owns whole bytes (8 rows) to avoid a shared-
// byte write race.
class IsDupCursor : public Cursor {
   public:
    IsDupCursor(std::unique_ptr<Cursor> in, std::uint64_t budget, bool unique)
        : first_(std::move(in)), spool_(budget), unique_(unique) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!counted_) co_await count(max_rows);
        while (auto m = co_await pass2_->next(max_rows)) {
            const std::int64_t n = m->rows;
            const std::int64_t nbytes = (n + 7) / 8;
            std::vector<std::uint8_t> bits(static_cast<std::size_t>(nbytes), 0);
            const std::vector<Series>& cols = m->columns;
            parallel_for(nbytes, std::int64_t{1} << 10,
                         [&](std::int64_t bb, std::int64_t be) {
                             for (std::int64_t byte = bb; byte < be; ++byte) {
                                 const std::int64_t base = byte * 8;
                                 const std::int64_t lim = std::min(base + 8, n);
                                 std::uint8_t v = 0;
                                 for (std::int64_t i = base; i < lim; ++i) {
                                     auto it = counts_.find(row_key(cols, i));
                                     const std::int64_t c =
                                         it != counts_.end() ? it->second : 0;
                                     if (unique_ ? c == 1 : c > 1)
                                         v |= static_cast<std::uint8_t>(
                                             1u << (i - base));
                                 }
                                 bits[static_cast<std::size_t>(byte)] = v;
                             }
                         });
            Morsel out;
            out.rows = n;
            out.columns.push_back(Series::flat(TypeId::Bool, bits.data(), n));
            co_return out;
        }
        co_return std::nullopt;
    }

   private:
    coro::CoroTask<void> count(std::int64_t max_rows) {
        std::vector<AggSpec> specs(1);
        specs[0].op = AggOp::Count;
        specs[0].out = "count";
        AggStatePtr state = agg_new(specs);
        constexpr std::size_t BATCH = 32;
        std::vector<Morsel> batch;
        batch.reserve(BATCH);
        auto key_series = [](const Morsel& m) {
            std::vector<std::string> keys(static_cast<std::size_t>(m.rows));
            for (std::int64_t i = 0; i < m.rows; ++i)
                keys[static_cast<std::size_t>(i)] = row_key(m.columns, i);
            return Series::strings(keys);
        };
        bool eof = false;
        while (!eof) {
            batch.clear();
            for (std::size_t b = 0; b < BATCH; ++b) {
                auto m = co_await first_->next(max_rows);
                if (!m) {
                    eof = true;
                    break;
                }
                batch.push_back(std::move(*m));
            }
            if (batch.empty()) break;
            if (batch.size() == 1) {
                Series key = key_series(batch[0]);
                agg_accumulate(*state, key, {});
            } else {
                std::vector<AggStatePtr> partials(batch.size());
                parallel_for(static_cast<std::int64_t>(batch.size()), 1,
                             [&](std::int64_t bi, std::int64_t ei) {
                                 for (std::int64_t j = bi; j < ei; ++j) {
                                     auto st = agg_new(specs);
                                     Series key = key_series(
                                         batch[static_cast<std::size_t>(j)]);
                                     agg_accumulate(*st, key, {});
                                     partials[static_cast<std::size_t>(j)] =
                                         std::move(st);
                                 }
                             });
                for (auto& p : partials)
                    if (p) agg_merge(*state, *p);
            }
            for (auto& m : batch) spool_.add(std::move(m.columns), m.rows);
        }
        first_.reset();
        DataFrame r = agg_finalize(*state, "key");
        const Series& kc = r.columns[0];
        const Series& cc = r.columns[1];
        const std::int64_t d = r.num_rows();
        counts_.reserve(static_cast<std::size_t>(d));
        for (std::int64_t i = 0; i < d; ++i)
            counts_.emplace(std::string(kc.string_at(i)),
                            cc.data<std::int64_t>()[i]);
        pass2_ = spool_.reader();
        counted_ = true;
    }

    std::unique_ptr<Cursor> first_, pass2_;
    spill::Spool spool_;
    bool unique_;
    bool counted_ = false;
    ankerl::unordered_dense::map<std::string, std::int64_t> counts_;
};

// Two-pass one-hot encode. Pass 1 collects the distinct non-null values of
// `column` (bounded by cardinality); pass 2 re-scans, replacing that column in
// place with one Int8 column per distinct value (ascending, named
// "<column>_<value>"), matching DataFrame::to_dummies. Output columns are
// data-dependent, reported via out_names().
class ToDummiesCursor : public Cursor {
   public:
    ToDummiesCursor(std::unique_ptr<Cursor> in, std::uint64_t budget,
                    std::vector<std::string> sch, std::string column)
        : first_(std::move(in)),
          spool_(budget),
          sch_(std::move(sch)),
          column_(std::move(column)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!built_) co_await build(max_rows);
        auto m = co_await pass2_->next(max_rows);
        if (!m) co_return std::nullopt;
        const Series& col = m->columns[static_cast<std::size_t>(ci_)];
        const std::int64_t n = m->rows;
        std::vector<Series> one;
        one.push_back(col.share());
        std::vector<std::vector<std::int8_t>> dummies(
            uniq_idx_.size(),
            std::vector<std::int8_t>(static_cast<std::size_t>(n), 0));
        for (std::int64_t i = 0; i < n; ++i) {
            if (col.is_null(i)) continue;
            auto it = uniq_idx_.find(row_key(one, i));
            if (it != uniq_idx_.end())
                dummies[static_cast<std::size_t>(it->second)]
                       [static_cast<std::size_t>(i)] = 1;
        }
        Morsel out;
        out.rows = n;
        for (std::size_t k = 0; k < m->columns.size(); ++k) {
            if (static_cast<int>(k) != ci_) {
                out.columns.push_back(m->columns[k].share());
                continue;
            }
            for (auto& col_bits : dummies)
                out.columns.push_back(
                    Series::flat(TypeId::Int8, col_bits.data(), n));
        }
        co_return out;
    }

    std::optional<std::vector<std::string>> out_names() const override {
        return produced_;
    }

   private:
    coro::CoroTask<void> build(std::int64_t max_rows) {
        ci_ = static_cast<int>(std::distance(
            sch_.begin(), std::find(sch_.begin(), sch_.end(), column_)));
        if (ci_ >= static_cast<int>(sch_.size()))
            throw std::out_of_range("to_dummies: no column named " + column_);

        ankerl::unordered_dense::set<std::string> seen;
        std::vector<Series> chunks;
        while (auto m = co_await first_->next(max_rows)) {
            const Series& col = m->columns[static_cast<std::size_t>(ci_)];
            std::vector<Series> one;
            one.push_back(col.share());
            std::vector<std::int64_t> keep;
            for (std::int64_t i = 0; i < m->rows; ++i)
                if (!col.is_null(i) && seen.insert(row_key(one, i)).second)
                    keep.push_back(i);
            if (!keep.empty()) chunks.push_back(col.take(keep));
            spool_.add(std::move(m->columns), m->rows);
        }
        first_.reset();
        // Distinct values, ascending (Series::unique sorts), matching eager.
        Series uniq;
        if (!chunks.empty())
            uniq = concat_columns(column_ptrs(chunks)).unique().materialize();
        const std::int64_t d = uniq.length();
        std::vector<Series> one;
        one.push_back(uniq.share());
        for (std::int64_t u = 0; u < d; ++u)
            uniq_idx_.emplace(row_key(one, u), static_cast<int>(u));
        for (std::size_t k = 0; k < sch_.size(); ++k) {
            if (static_cast<int>(k) != ci_) {
                produced_.push_back(sch_[k]);
                continue;
            }
            for (std::int64_t u = 0; u < d; ++u)
                produced_.push_back(column_ + "_" + cell_to_string(uniq, u));
        }
        pass2_ = spool_.reader();
        built_ = true;
    }

    std::unique_ptr<Cursor> first_, pass2_;
    spill::Spool spool_;
    std::vector<std::string> sch_;
    std::string column_;
    bool built_ = false;
    int ci_ = 0;
    ankerl::unordered_dense::map<std::string, int> uniq_idx_;
    std::vector<std::string> produced_;
};

// Two-pass long->wide pivot, matching DataFrame::pivot. Pass 1 discovers the
// distinct index rows and `on` columns (both ascending via Series::unique);
// pass 2 aggregates each (index, on) cell through the mergeable agg IR keyed by
// row*C+col (first/last/sum/min/max/mean all map to an AggOp), so state is
// bounded by the output (R*C) not the input. Output columns are data-dependent,
// reported via out_names().
class PivotCursor : public Cursor {
   public:
    PivotCursor(std::unique_ptr<Cursor> in, std::uint64_t budget,
                std::vector<std::string> sch, std::string index, std::string on,
                std::string values, std::string agg)
        : first_(std::move(in)),
          spool_(budget),
          sch_(std::move(sch)),
          index_(std::move(index)),
          on_(std::move(on)),
          values_(std::move(values)),
          agg_(std::move(agg)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        done_ = true;
        const int ii = idx_of(index_), ci = idx_of(on_), vi = idx_of(values_);
        if (ii < 0) throw std::out_of_range("pivot: no column named " + index_);
        if (ci < 0) throw std::out_of_range("pivot: no column named " + on_);
        if (vi < 0)
            throw std::out_of_range("pivot: no column named " + values_);

        // Pass 1: distinct index and `on` values (ascending, matching eager).
        ankerl::unordered_dense::set<std::string> seen_i, seen_c;
        std::vector<Series> ich, cch;
        while (auto m = co_await first_->next(max_rows)) {
            distinct_into(m->columns[static_cast<std::size_t>(ii)], seen_i,
                          ich);
            distinct_into(m->columns[static_cast<std::size_t>(ci)], seen_c,
                          cch);
            spool_.add(std::move(m->columns), m->rows);
        }
        first_.reset();
        Series uniq_idx =
            ich.empty()
                ? Series{}
                : concat_columns(column_ptrs(ich)).unique().materialize();
        Series uniq_col =
            cch.empty()
                ? Series{}
                : concat_columns(column_ptrs(cch)).unique().materialize();
        const std::int64_t R = uniq_idx.length(), C = uniq_col.length();
        ankerl::unordered_dense::map<std::string, std::int64_t> row_of, col_of;
        key_index(uniq_idx, R, row_of);
        key_index(uniq_col, C, col_of);
        const std::int64_t sentinel = R * C;

        // Pass 2: aggregate each cell through the agg IR keyed by row*C+col.
        std::vector<AggSpec> specs;
        AggSpec sp;
        sp.op = to_agg_op(agg_from_string(agg_));
        sp.value_col = 0;
        sp.out = "v";
        specs.push_back(std::move(sp));
        AggStatePtr state = agg_new(std::move(specs));
        auto p2 = spool_.reader();
        while (auto m = co_await p2->next(max_rows)) {
            const Series& ic = m->columns[static_cast<std::size_t>(ii)];
            const Series& cc = m->columns[static_cast<std::size_t>(ci)];
            const Series& vc = m->columns[static_cast<std::size_t>(vi)];
            const std::int64_t n = m->rows;
            std::vector<Series> oi, oc;
            oi.push_back(ic.share());
            oc.push_back(cc.share());
            std::vector<std::int64_t> keyv(static_cast<std::size_t>(n),
                                           sentinel);
            for (std::int64_t i = 0; i < n; ++i) {
                if (ic.is_null(i) || cc.is_null(i)) continue;
                auto ri = row_of.find(row_key(oi, i));
                auto rc = col_of.find(row_key(oc, i));
                if (ri != row_of.end() && rc != col_of.end())
                    keyv[static_cast<std::size_t>(i)] =
                        ri->second * C + rc->second;
            }
            Series keyc = Series::flat_i64(keyv.data(), n);
            std::vector<const Series*> values{&vc};
            agg_accumulate(*state, keyc, values);
        }
        DataFrame agg_res = agg_finalize(*state, "cell");
        Series source = agg_res.column("v");
        Series cells_col = agg_res.column("cell");
        const std::int64_t* cells = cells_col.data<std::int64_t>();
        std::vector<std::int64_t> cell_src(static_cast<std::size_t>(R * C), -1);
        for (std::int64_t p = 0; p < cells_col.length(); ++p) {
            const std::int64_t cell = cells[p];
            if (cell != sentinel) cell_src[static_cast<std::size_t>(cell)] = p;
        }

        DataFrame out;
        out.names.push_back(index_);
        out.columns.push_back(uniq_idx.share());
        for (std::int64_t c = 0; c < C; ++c) {
            std::vector<std::int64_t> ti(static_cast<std::size_t>(R));
            for (std::int64_t r = 0; r < R; ++r)
                ti[static_cast<std::size_t>(r)] =
                    cell_src[static_cast<std::size_t>(r * C + c)];
            out.names.push_back(cell_to_string(uniq_col, c));
            out.columns.push_back(source.take(ti));
        }
        produced_ = out.names;
        co_return morsel_of(std::move(out));
    }

    std::optional<std::vector<std::string>> out_names() const override {
        return produced_;
    }

   private:
    int idx_of(const std::string& name) const {
        auto it = std::find(sch_.begin(), sch_.end(), name);
        return it == sch_.end() ? -1 : static_cast<int>(it - sch_.begin());
    }
    // Append the first-occurrence non-null cells of `col` to `chunks`.
    static void distinct_into(const Series& col,
                              ankerl::unordered_dense::set<std::string>& seen,
                              std::vector<Series>& chunks) {
        std::vector<Series> one;
        one.push_back(col.share());
        std::vector<std::int64_t> keep;
        for (std::int64_t i = 0; i < col.length(); ++i)
            if (!col.is_null(i) && seen.insert(row_key(one, i)).second)
                keep.push_back(i);
        if (!keep.empty()) chunks.push_back(col.take(keep));
    }
    static void key_index(
        const Series& uniq, std::int64_t n,
        ankerl::unordered_dense::map<std::string, std::int64_t>& out) {
        std::vector<Series> one;
        one.push_back(uniq.share());
        for (std::int64_t i = 0; i < n; ++i) out.emplace(row_key(one, i), i);
    }

    std::unique_ptr<Cursor> first_;
    spill::Spool spool_;
    std::vector<std::string> sch_;
    std::string index_, on_, values_, agg_;
    bool done_ = false;
    std::vector<std::string> produced_;
};

// Reverses row order. Buffers the whole input through a Spool (bounded memory,
// spills past budget), then applies DataFrame::reverse once and emits one
// morsel - there is no incremental reverse.
class ReverseCursor : public Cursor {
   public:
    ReverseCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                  std::uint64_t budget)
        : in_(std::move(in)), spool_(budget), sch_(std::move(sch)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        done_ = true;
        while (auto m = co_await in_->next(max_rows))
            spool_.add(std::move(m->columns), m->rows);
        in_.reset();
        std::unique_ptr<Cursor> reader = spool_.reader();
        DataFrame df = co_await drain_cursor(*reader, sch_, max_rows);
        co_return morsel_of(df.reverse());
    }

   private:
    std::unique_ptr<Cursor> in_;
    spill::Spool spool_;
    std::vector<std::string> sch_;
    bool done_ = false;
};

// Keeps the rows at `indices_` (arbitrary order, repeats allowed). Buffers the
// whole input as ReverseCursor does: the requested indices are positions
// against the fully assembled frame, so every row must be resident first.
class TakeCursor : public Cursor {
   public:
    TakeCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
               std::vector<std::int64_t> indices, std::uint64_t budget)
        : in_(std::move(in)),
          spool_(budget),
          sch_(std::move(sch)),
          indices_(std::move(indices)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        done_ = true;
        while (auto m = co_await in_->next(max_rows))
            spool_.add(std::move(m->columns), m->rows);
        in_.reset();
        std::unique_ptr<Cursor> reader = spool_.reader();
        DataFrame df = co_await drain_cursor(*reader, sch_, max_rows);
        co_return morsel_of(df.take(indices_));
    }

   private:
    std::unique_ptr<Cursor> in_;
    spill::Spool spool_;
    std::vector<std::string> sch_;
    std::vector<std::int64_t> indices_;
    bool done_ = false;
};

// Stable lexicographic sort by several key columns. Buffers the whole input as
// ReverseCursor does: DataFrame::sort_by_multi has no external-merge form
// (unlike the single-key SortMergeCursor), so this holds the full frame rather
// than reimplementing one.
class SortByMultiCursor : public Cursor {
   public:
    SortByMultiCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                      std::vector<std::string> by, std::vector<bool> descending,
                      std::uint64_t budget)
        : in_(std::move(in)),
          spool_(budget),
          sch_(std::move(sch)),
          by_(std::move(by)),
          descending_(std::move(descending)) {}

    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        if (done_) co_return false;
        co_return co_await in_->narrow(predicate);
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        done_ = true;
        while (auto m = co_await in_->next(max_rows))
            spool_.add(std::move(m->columns), m->rows);
        in_.reset();
        std::unique_ptr<Cursor> reader = spool_.reader();
        DataFrame df = co_await drain_cursor(*reader, sch_, max_rows);
        Morsel out = morsel_of(df.sort_by_multi(by_, descending_));
        // A stable sort keeps the leading key's values non-decreasing (or
        // non-increasing) across the whole output regardless of how ties are
        // broken by the trailing keys, so that alone is a true claim.
        // Recording every key would need a per-morsel vector instead of one
        // int - real weight on the hot struct for a multi-column claim no
        // consumer reads yet.
        if (!by_.empty()) {
            const int key_idx = column_index_of(sch_, by_.front());
            if (key_idx >= 0) {
                out.ordering = Ordering::ByColumn;
                out.ordered_column = key_idx;
                out.ordered_descending = descending_.front();
            }
        }
        co_return out;
    }

   private:
    std::unique_ptr<Cursor> in_;
    spill::Spool spool_;
    std::vector<std::string> sch_;
    std::vector<std::string> by_;
    std::vector<bool> descending_;
    bool done_ = false;
};

// Hash join: collects the right plan in full on the first pull (the build
// side), then streams each left morsel through it. A Right / Outer join emits
// the unmatched right rows as one final morsel once the left is drained.
class JoinCursor : public Cursor {
   public:
    JoinCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
               std::vector<Field> left_fields, LazyFrame other,
               std::vector<std::string> left_on,
               std::vector<std::string> right_on, JoinHow how,
               std::string suffix)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          left_fields_(std::move(left_fields)),
          other_(std::move(other)),
          left_on_(std::move(left_on)),
          right_on_(std::move(right_on)),
          how_(how),
          suffix_(std::move(suffix)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!join_) {
            DataFrame right = co_await other_.collect();
            if (std::optional<Expr> pred = build_side_predicate(right))
                co_await in_->narrow(*pred);
            join_.emplace(std::move(right), left_on_, right_on_, how_, suffix_);
        }
        while (in_) {
            auto m = co_await in_->next(max_rows);
            if (!m) {
                in_.reset();
                break;
            }
            DataFrame left;
            left.names = sch_;
            left.columns = std::move(m->columns);
            if (templates_.empty())
                for (const Series& c : left.columns)
                    templates_.push_back(c.share());
            DataFrame out = join_->probe(left);
            if (out.num_rows() > 0) co_return morsel_of(std::move(out));
        }
        if (flushed_) co_return std::nullopt;
        flushed_ = true;
        if (templates_.empty()) templates_ = null_templates();
        DataFrame rest = join_->flush(sch_, templates_);
        if (rest.num_rows() == 0) co_return std::nullopt;
        co_return morsel_of(std::move(rest));
    }

   private:
    // A left row whose key is not on the build side is dropped by an Inner,
    // Right or Semi join, so the build keys narrow the left input: the set
    // itself when it is small, its range for a large numeric key, nothing
    // for a large key of another type. Positional against sch_, the join's
    // input; a key column absent from either side is left to probe(), which
    // names it in its error.
    std::optional<Expr> build_side_predicate(const DataFrame& right) const {
        if (how_ != JoinHow::Inner && how_ != JoinHow::Right &&
            how_ != JoinHow::Semi)
            return std::nullopt;
        std::optional<Expr> pred;
        for (std::size_t i = 0; i < left_on_.size() && i < right_on_.size();
             ++i) {
            const auto l = std::find(sch_.begin(), sch_.end(), left_on_[i]);
            const auto r =
                std::find(right.names.begin(), right.names.end(), right_on_[i]);
            if (l == sch_.end() || r == right.names.end()) continue;
            const Series& keys =
                right
                    .columns[static_cast<std::size_t>(r - right.names.begin())];
            const Expr col =
                expr_col(static_cast<std::int32_t>(l - sch_.begin()));
            Series values = keys.drop_nulls().unique();
            std::optional<Expr> one;
            if (values.length() <= NARROW_MAX_KEYS) {
                one = expr_is_in(col, std::move(values));
            } else if (is_numeric_dispatchable(keys.type())) {
                one = expr_logical(LogicalOp::And,
                                   expr_cmp(CmpOp::Ge, col, values.min()),
                                   expr_cmp(CmpOp::Le, col, values.max()));
            } else {
                continue;
            }
            pred = pred ? expr_logical(LogicalOp::And, *pred, *one) : one;
        }
        return pred;
    }
    static constexpr std::int64_t NARROW_MAX_KEYS = 1 << 16;

    // A typed empty column per left field, for a flush with no left morsel to
    // borrow types from. Refuses a type it cannot rebuild rather than emit a
    // column that has silently lost its parameters.
    std::vector<Series> null_templates() const {
        std::vector<Series> out;
        out.reserve(left_fields_.size());
        for (const Field& f : left_fields_) {
            const DataType& dt = f.type;
            const bool rebuildable =
                dt.fields.empty() &&
                (byte_width(dt.id).has_value() || dt.id == TypeId::String ||
                 dt.id == TypeId::Binary);
            if (!rebuildable)
                throw std::runtime_error(
                    "join: cannot build a null column of type " +
                    std::string(type_name(dt.id)) + " for left column '" +
                    f.name + "' when the left side produced no rows");
            Series s = Series::nulls(dt.id, 1);
            dftu_series* h = s.handle();
            h->time_unit = dt.time_unit;
            h->timezone = dt.timezone;
            h->decimal_precision = dt.decimal_precision;
            h->decimal_scale = dt.decimal_scale;
            h->fixed_size = dt.fixed_size;
            out.push_back(std::move(s));
        }
        return out;
    }

    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::vector<Field> left_fields_;
    LazyFrame other_;
    std::vector<std::string> left_on_;
    std::vector<std::string> right_on_;
    JoinHow how_;
    std::string suffix_;
    std::optional<HashJoin> join_;
    std::vector<Series> templates_;
    bool flushed_ = false;
};

// Vertical concatenation: every morsel of the left plan, then every morsel
// of the right one, which is opened only when the left is drained. Both plans
// share a schema (checked when the op is built), so morsels pass through.
class ConcatCursor : public Cursor {
   public:
    ConcatCursor(std::unique_ptr<Cursor> in, LazyFrame other)
        : in_(std::move(in)), other_(std::move(other)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (in_) {
            if (auto m = co_await in_->next(max_rows)) {
                m->ordering = Ordering::Unordered;
                co_return m;
            }
            in_.reset();
            right_ = other_.open_cursor();
        }
        if (!right_) co_return std::nullopt;
        auto m = co_await right_->next(max_rows);
        if (!m) {
            right_.reset();
            co_return std::nullopt;
        }
        m->ordering = Ordering::Unordered;
        co_return m;
    }

   private:
    std::unique_ptr<Cursor> in_;
    LazyFrame other_;
    std::unique_ptr<Cursor> right_;
};

// The operands of a registry frame op, deep-copied so the plan owns them for
// as long as it lives: a LazyOp is shared and re-run, so it cannot borrow the
// caller's strings and lists the way a one-shot dftu_op_run_frame call does.
// A FRAME operand after the primary one is a whole LazyFrame, collected when
// the op runs; SERIES shares the column; EXPR / QUERY / LAZY operands are
// refused (no owned form).
class OwnedFrameOpArgs {
   public:
    OwnedFrameOpArgs(const dftu_op_desc& op, const OpArgs& args,
                     std::vector<LazyFrame> frames)
        : frames_(std::move(frames)) {
        const dftu_op_arg& raw = args.raw();
        std::size_t next_frame = 0;
        for (int i = 0; i < DFTU_OP_MAX_ARGS; ++i) {
            const dftu_op_tok t = DFTU_OP_SIG_ARG(op.sig, i);
            const dftu_op_val& v = raw.args[i];
            Slot s;
            s.tok = t;
            switch (t) {
                case DFTU_TOK_NONE:
                    break;
                case DFTU_TOK_FRAME:
                    if (i == 0) break;
                    if (next_frame >= frames_.size())
                        throw std::invalid_argument(
                            std::string("lazy frame op '") + op.name +
                            "': operand " + std::to_string(i) +
                            " is a frame but no plan was given for it");
                    s.frame_index = next_frame++;
                    break;
                case DFTU_TOK_SERIES:
                    if (!v.series)
                        throw std::invalid_argument(
                            std::string("lazy frame op '") + op.name +
                            "': operand " + std::to_string(i) +
                            " is a column but none was given");
                    s.series = Series{dftu_series_share(v.series)};
                    break;
                case DFTU_TOK_SCALAR:
                    s.val.scalar = v.scalar;
                    break;
                case DFTU_TOK_I64:
                    s.val.i64 = v.i64;
                    break;
                case DFTU_TOK_U64:
                    s.val.u64 = v.u64;
                    break;
                case DFTU_TOK_F64:
                    s.val.f64 = v.f64;
                    break;
                case DFTU_TOK_CHAR:
                    s.val.ch = v.ch;
                    break;
                case DFTU_TOK_BOOL:
                case DFTU_TOK_CMP:
                case DFTU_TOK_PRIM:
                case DFTU_TOK_LOGICAL:
                case DFTU_TOK_DTYPE:
                case DFTU_TOK_REDUCE:
                case DFTU_TOK_I32:
                case DFTU_TOK_RANK:
                case DFTU_TOK_ROLLING:
                    s.val.i32 = v.i32;
                    break;
                case DFTU_TOK_STR:
                    s.strings.emplace_back(v.str.ptr ? std::string(v.str.ptr)
                                                     : std::string());
                    break;
                case DFTU_TOK_STRLIST:
                    for (int32_t k = 0; k < v.list.n; ++k)
                        s.strings.emplace_back(v.list.items[k]);
                    break;
                case DFTU_TOK_I32LIST:
                    s.i32s.assign(v.i32list.items,
                                  v.i32list.items + v.i32list.n);
                    break;
                case DFTU_TOK_I64LIST:
                    s.i64s.assign(v.i64list.items,
                                  v.i64list.items + v.i64list.n);
                    break;
                case DFTU_TOK_AGGLIST:
                    for (int32_t k = 0; k < v.agglist.n; ++k) {
                        const dftu_group_agg& a = v.agglist.items[k];
                        s.strings.emplace_back(a.op ? a.op : "");
                        s.strings.emplace_back(a.column ? a.column : "");
                        s.strings.emplace_back(a.out ? a.out : "");
                        s.aggs.push_back(a);
                    }
                    break;
                case DFTU_TOK_WINLIST:
                    for (int32_t k = 0; k < v.winlist.n; ++k) {
                        const dftu_window_spec& w = v.winlist.items[k];
                        s.strings.emplace_back(w.value ? w.value : "");
                        s.has_value.push_back(w.value != nullptr);
                        s.strings.emplace_back(w.time ? w.time : "");
                        s.has_time.push_back(w.time != nullptr);
                        s.strings.emplace_back(w.out ? w.out : "");
                        s.wins.push_back(w);
                    }
                    break;
                case DFTU_TOK_EXPR:
                case DFTU_TOK_QUERY:
                case DFTU_TOK_LAZY:
                    throw std::invalid_argument(std::string("lazy frame op '") +
                                                op.name + "': operand " +
                                                std::to_string(i) +
                                                " has no owned form in a plan");
            }
            slots_.push_back(std::move(s));
        }
        if (next_frame != frames_.size())
            throw std::invalid_argument(
                std::string("lazy frame op '") + op.name +
                "': " + std::to_string(frames_.size()) + " plan(s) given for " +
                std::to_string(next_frame) + " frame operand(s)");
    }

    const std::vector<LazyFrame>& frames() const noexcept { return frames_; }

    // The same operands over other frame plans; `frames` must match the
    // frame operand count.
    std::shared_ptr<const OwnedFrameOpArgs> with_frames(
        std::vector<LazyFrame> frames) const {
        auto out = std::shared_ptr<OwnedFrameOpArgs>(new OwnedFrameOpArgs());
        out->slots_.reserve(slots_.size());
        for (const Slot& s : slots_) {
            Slot c;
            c.tok = s.tok;
            c.val = s.val;
            c.series = s.series.valid() ? s.series.share() : Series{};
            c.strings = s.strings;
            c.i32s = s.i32s;
            c.i64s = s.i64s;
            c.aggs = s.aggs;
            c.wins = s.wins;
            c.has_value = s.has_value;
            c.has_time = s.has_time;
            c.frame_index = s.frame_index;
            out->slots_.push_back(std::move(c));
        }
        out->frames_ = std::move(frames);
        return out;
    }

    // The C operand bag, pointing into this object's storage; `cstrs` and
    // `wins` are scratch the bag points into and must outlive the run.
    dftu_op_arg bind(std::vector<std::vector<const char*>>& cstrs,
                     std::vector<std::vector<dftu_window_spec>>& wins,
                     std::vector<std::vector<dftu_group_agg>>& aggs) const {
        dftu_op_arg out{};
        cstrs.assign(slots_.size(), {});
        wins.assign(slots_.size(), {});
        aggs.assign(slots_.size(), {});
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const Slot& s = slots_[i];
            dftu_op_val& v = out.args[i];
            switch (s.tok) {
                case DFTU_TOK_NONE:
                case DFTU_TOK_FRAME:
                case DFTU_TOK_EXPR:
                case DFTU_TOK_QUERY:
                case DFTU_TOK_LAZY:
                    break;
                case DFTU_TOK_SERIES:
                    v.series = s.series.handle();
                    break;
                case DFTU_TOK_SCALAR:
                case DFTU_TOK_I64:
                case DFTU_TOK_U64:
                case DFTU_TOK_F64:
                case DFTU_TOK_CHAR:
                case DFTU_TOK_BOOL:
                case DFTU_TOK_CMP:
                case DFTU_TOK_PRIM:
                case DFTU_TOK_LOGICAL:
                case DFTU_TOK_DTYPE:
                case DFTU_TOK_REDUCE:
                case DFTU_TOK_I32:
                case DFTU_TOK_RANK:
                case DFTU_TOK_ROLLING:
                    v = s.val;
                    break;
                case DFTU_TOK_STR:
                    v.str.ptr = s.strings.front().c_str();
                    v.str.len = static_cast<int32_t>(s.strings.front().size());
                    break;
                case DFTU_TOK_STRLIST:
                    for (const std::string& str : s.strings)
                        cstrs[i].push_back(str.c_str());
                    v.list.items = cstrs[i].data();
                    v.list.n = static_cast<int32_t>(cstrs[i].size());
                    break;
                case DFTU_TOK_I32LIST:
                    v.i32list.items = s.i32s.data();
                    v.i32list.n = static_cast<int32_t>(s.i32s.size());
                    break;
                case DFTU_TOK_I64LIST:
                    v.i64list.items = s.i64s.data();
                    v.i64list.n = static_cast<int32_t>(s.i64s.size());
                    break;
                case DFTU_TOK_AGGLIST:
                    for (std::size_t k = 0; k < s.aggs.size(); ++k) {
                        dftu_group_agg a = s.aggs[k];
                        a.op = s.strings[3 * k].c_str();
                        a.column = s.strings[3 * k + 1].c_str();
                        a.out = s.strings[3 * k + 2].c_str();
                        aggs[i].push_back(a);
                    }
                    v.agglist.items = aggs[i].data();
                    v.agglist.n = static_cast<int32_t>(aggs[i].size());
                    break;
                case DFTU_TOK_WINLIST:
                    for (std::size_t k = 0; k < s.wins.size(); ++k) {
                        dftu_window_spec w = s.wins[k];
                        w.value =
                            s.has_value[k] ? s.strings[3 * k].c_str() : nullptr;
                        w.time = s.has_time[k] ? s.strings[3 * k + 1].c_str()
                                               : nullptr;
                        w.out = s.strings[3 * k + 2].c_str();
                        wins[i].push_back(w);
                    }
                    v.winlist.items = wins[i].data();
                    v.winlist.n = static_cast<int32_t>(wins[i].size());
                    break;
            }
        }
        return out;
    }

   private:
    OwnedFrameOpArgs() = default;

    struct Slot {
        dftu_op_tok tok = DFTU_TOK_NONE;
        dftu_op_val val{};
        Series series;
        std::vector<std::string> strings;
        std::vector<std::int32_t> i32s;
        std::vector<std::int64_t> i64s;
        std::vector<dftu_group_agg> aggs;
        std::vector<dftu_window_spec> wins;
        std::vector<bool> has_value;
        std::vector<bool> has_time;
        std::size_t frame_index = 0;
    };
    std::vector<Slot> slots_;
    std::vector<LazyFrame> frames_;
};

// Runs a registry frame op (dftu.frame.*) over the whole input: buffers every
// morsel through a Spool, collects each further frame operand's plan, runs
// the op once through dftu_op_run_frame and emits the result as one morsel.
// A pipeline breaker; the output names come from the result.
class FrameOpCursor : public Cursor {
   public:
    FrameOpCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                  std::string name, const dftu_op_desc* op,
                  std::shared_ptr<const OwnedFrameOpArgs> args,
                  std::uint64_t budget)
        : in_(std::move(in)),
          spool_(budget),
          sch_(std::move(sch)),
          name_(std::move(name)),
          op_(op),
          args_(std::move(args)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        done_ = true;
        while (auto m = co_await in_->next(max_rows))
            spool_.add(std::move(m->columns), m->rows);
        in_.reset();
        std::unique_ptr<Cursor> reader = spool_.reader();
        DataFrame primary = co_await drain_cursor(*reader, sch_, max_rows);

        std::vector<dftu_dataframe*> handles;
        struct Free {
            std::vector<dftu_dataframe*>& h;
            ~Free() {
                for (dftu_dataframe* p : h) dftu_dataframe_free(p);
            }
        } guard{handles};
        handles.push_back(dataframe_handle_wrap(std::move(primary)));
        for (const LazyFrame& other : args_->frames()) {
            DataFrame f = co_await other.collect();
            handles.push_back(dataframe_handle_wrap(std::move(f)));
        }
        std::vector<std::vector<const char*>> cstrs;
        std::vector<std::vector<dftu_window_spec>> wins;
        std::vector<std::vector<dftu_group_agg>> aggs;
        const dftu_op_arg bag = args_->bind(cstrs, wins, aggs);
        std::vector<const dftu_dataframe*> frames(handles.begin(),
                                                  handles.end());
        dftu_dataframe* result = dftu_op_run_frame(
            op_, frames.data(), static_cast<uint32_t>(frames.size()), &bag);
        if (!result)
            throw std::runtime_error("lazy frame op '" + name_ +
                                     "' failed: the op returned no frame");
        DataFrame out = dataframe_handle_take(result);
        out_names_ = out.names;
        co_return morsel_of(std::move(out));
    }

    std::optional<std::vector<std::string>> out_names() const override {
        return out_names_;
    }

   private:
    std::unique_ptr<Cursor> in_;
    spill::Spool spool_;
    std::vector<std::string> sch_;
    std::string name_;
    const dftu_op_desc* op_;
    std::shared_ptr<const OwnedFrameOpArgs> args_;
    std::optional<std::vector<std::string>> out_names_;
    bool done_ = false;
};

// ---- plan ops (tagged union) ------------------------------------------------

struct FilterOp {
    Expr pred;
};
struct SelectOp {
    std::vector<std::string> names;
};
struct WithColumnOp {
    std::string name;
    Expr expr;
};
struct RenameOp {
    std::vector<std::string> names;
};
struct SliceOp {
    std::int64_t offset;
    std::int64_t len;
};
struct TailOp {
    std::int64_t n;
};
struct DropNullsOp {};
struct FillNullOp {
    dftu_scalar value;
};
struct WithRowIndexOp {
    std::string name;
};
struct NullCountOp {};
struct ExplodeOp {
    std::string column;
};
struct UnnestOp {
    std::string column;
    bool keep_empty;
    // The output names when the input's typed schema settles them (a known
    // List<Struct> flattens to its fields, any other List keeps the name);
    // empty when the column's type is unknown at build time.
    std::vector<std::string> out_names;
};
struct FrameOp {
    std::string name;
    const dftu_op_desc* op;
    std::shared_ptr<const OwnedFrameOpArgs> args;
    // The output names when the caller can say them from the op's contract
    // (a window keeps its input and appends its specs); empty means
    // data-dependent, known at collect.
    std::vector<std::string> out_names;
};
struct UnpivotOp {
    std::vector<std::string> id_vars;
    std::vector<std::string> value_vars;
};
struct TopkOp {
    std::string name;
    std::int64_t k;
    bool largest;
};
struct GroupByOp {
    std::vector<std::string> keys;
    std::vector<GroupAgg> aggs;
    std::vector<AggDynSpec> dyn;
    std::string dyn_prefix;
};
struct SortByOp {
    std::string name;
    bool descending;
};
struct UniqueOp {
    std::vector<std::string> subset;
};
struct SampleOp {
    std::int64_t n;
    std::uint64_t seed;
};
struct IsDupOp {
    bool unique;  // true = is_unique, false = is_duplicated
};
struct GroupByDynamicOp {
    std::string time_col;
    std::int64_t every;
    std::int64_t period;
    std::vector<GroupAgg> aggs;
    std::int64_t origin;
    bool origin_min;
};
struct PivotOp {
    std::string index, on, values, agg;
};
struct ToDummiesOp {
    std::string column;
};
struct DescribeOp {};
struct ReverseOp {};
struct TakeOp {
    std::vector<std::int64_t> indices;
};
struct FilterMaskOp {
    Series mask;
};
struct SortByMultiOp {
    std::vector<std::string> by;
    std::vector<bool> descending;
};
struct JoinOp {
    LazyFrame other;
    std::vector<std::string> left_on;
    std::vector<std::string> right_on;
    JoinHow how;
    std::string suffix;
    std::vector<Field> left_fields;
};
struct ConcatOp {
    LazyFrame other;
};

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace

AggOp to_agg_op(Agg a) {
    switch (a) {
#define DFTU_AGG_OP(id, code, name) \
    case Agg::id:                   \
        return AggOp::id;
#include <dftracer/utils/dataframe/agg_ops.def>
#undef DFTU_AGG_OP
    }
    return AggOp::Count;
}

Agg from_agg_op(AggOp a) {
    switch (a) {
#define DFTU_AGG_OP(id, code, name) \
    case AggOp::id:                 \
        return Agg::id;
#include <dftracer/utils/dataframe/agg_ops.def>
#undef DFTU_AGG_OP
    }
    return Agg::Count;
}

// A plan node: exactly the data its op needs (no fat struct). Held by value in
// the LazyFrame plan.
class LazyOp {
   public:
    std::variant<FilterOp, SelectOp, WithColumnOp, RenameOp, SliceOp, TailOp,
                 DropNullsOp, FillNullOp, WithRowIndexOp, NullCountOp,
                 ExplodeOp, UnpivotOp, TopkOp, GroupByOp, SortByOp, UniqueOp,
                 SampleOp, IsDupOp, GroupByDynamicOp, PivotOp, ToDummiesOp,
                 DescribeOp, ReverseOp, TakeOp, FilterMaskOp, SortByMultiOp,
                 JoinOp, ConcatOp, UnnestOp, FrameOp, NodeOp>
        node;
};

namespace detail {

struct PlanAccess {
    static const Source& source(const LazyFrame& lf) { return *lf.source_; }
    static const std::shared_ptr<const Source>& source_ptr(
        const LazyFrame& lf) {
        return lf.source_;
    }
    static const std::vector<std::shared_ptr<const LazyOp>>& ops(
        const LazyFrame& lf) {
        return lf.ops_;
    }
    static std::uint64_t memory_budget(const LazyFrame& lf) {
        return lf.memory_budget_;
    }
    static LazyFrame make(std::shared_ptr<const Source> source,
                          std::vector<std::shared_ptr<const LazyOp>> ops,
                          std::uint64_t memory_budget) {
        return LazyFrame(std::move(source), std::move(ops), memory_budget);
    }
    static coro::CoroTask<DataFrame> run_in_memory(
        DataFrame df, std::vector<std::shared_ptr<const LazyOp>> ops) {
        return LazyFrame::run_ops_in_memory(std::move(df), std::move(ops));
    }
};

}  // namespace detail

namespace {

// Runs a node's mandatory output_schema over `in`, returning exactly the
// Schema it declares. dftu_schema::top mirrors dataframe::Schema::fields, so
// the call is a plain copy in and out, no per-field translation needed.
Schema call_node_output_schema(const NodeOp& node, const Schema& in) {
    ::dftu_schema in_built;
    in_built.top = in.fields;
    ::dftu_schema out_built;
    node.vt.output_schema(node.self, &in_built, &node.args, &out_built);
    Schema out;
    out.fields = std::move(out_built.top);
    return out;
}

// Names-only callers (out_schema below, and lower_cursor_chain at drive time)
// have no typed input Schema to hand a node - only the column names already
// resolved for this point in the plan - so every input field reports Unknown.
// A node whose output_schema logic needs real input types should be driven
// through LazyFrame::output_schema() instead, which always calls it with the
// typed Schema.
Schema call_node_output_schema(const NodeOp& node,
                               const std::vector<std::string>& names) {
    Schema in;
    in.fields.reserve(names.size());
    for (const std::string& n : names)
        in.fields.push_back(Field{n, scalar(TypeId::Unknown), true});
    return call_node_output_schema(node, in);
}

const char* join_how_name(JoinHow how) {
    switch (how) {
        case JoinHow::Inner:
            return "inner";
        case JoinHow::Left:
            return "left";
        case JoinHow::Right:
            return "right";
        case JoinHow::Outer:
            return "outer";
        case JoinHow::Semi:
            return "semi";
        case JoinHow::Anti:
            return "anti";
        case JoinHow::Cross:
            return "cross";
    }
    return "?";
}

std::string join_names(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += v[i];
    }
    return s;
}

std::vector<std::string> out_schema(const LazyOp& op,
                                    std::vector<std::string> in) {
    return std::visit(
        overloaded{
            [&](const FilterOp&) { return in; },
            [&](const SliceOp&) { return in; },
            [&](const TailOp&) { return in; },
            [&](const DropNullsOp&) { return in; },
            [&](const FillNullOp&) { return in; },
            [&](const SelectOp& o) { return o.names; },
            [&](const RenameOp& o) { return o.names; },
            [&](const WithColumnOp& o) {
                if (std::find(in.begin(), in.end(), o.name) == in.end())
                    in.push_back(o.name);
                return in;
            },
            [&](const WithRowIndexOp& o) {
                in.insert(in.begin(), o.name);
                return in;
            },
            [&](const NullCountOp&) { return in; },
            [&](const ExplodeOp&) { return in; },
            [&](const TopkOp&) { return in; },
            [&](const UnpivotOp& o) {
                std::vector<std::string> s = o.id_vars;
                s.push_back("variable");
                s.push_back("value");
                return s;
            },
            [&](const GroupByOp& o) {
                // Dyn column names are discovered at run time: data-dependent
                // schema, signalled empty like pivot (collect() relabels).
                if (!o.dyn.empty()) return std::vector<std::string>{};
                std::vector<std::string> s = o.keys;
                for (const GroupAgg& a : o.aggs) s.push_back(a.out);
                return s;
            },
            [&](const SortByOp&) { return in; },
            [&](const UniqueOp&) { return in; },
            [&](const SampleOp&) { return in; },
            [&](const IsDupOp& o) {
                return std::vector<std::string>{o.unique ? "is_unique"
                                                         : "is_duplicated"};
            },
            [&](const GroupByDynamicOp& o) {
                std::vector<std::string> s{o.time_col};
                for (const GroupAgg& a : o.aggs) s.push_back(a.out);
                return s;
            },
            // Data-dependent schema: known only after running; collect()
            // relabels from the cursor's out_names().
            [&](const PivotOp&) { return std::vector<std::string>{}; },
            [&](const ToDummiesOp&) { return std::vector<std::string>{}; },
            [&](const DescribeOp&) { return std::vector<std::string>{}; },
            [&](const ReverseOp&) { return in; },
            [&](const TakeOp&) { return in; },
            [&](const FilterMaskOp&) { return in; },
            [&](const SortByMultiOp&) { return in; },
            [&](const JoinOp& o) {
                return join_out_names(in, o.other.schema(), o.left_on,
                                      o.right_on, o.how, o.suffix);
            },
            [&](const ConcatOp&) { return in; },
            [&](const UnnestOp& o) { return o.out_names; },
            [&](const FrameOp& o) { return o.out_names; },
            [&](const NodeOp& o) {
                Schema s = call_node_output_schema(o, in);
                std::vector<std::string> names;
                names.reserve(s.fields.size());
                for (const Field& f : s.fields) names.push_back(f.name);
                return names;
            }},
        op.node);
}

const Field* find_field(const Schema& s, const std::string& name) {
    for (const Field& f : s.fields)
        if (f.name == name) return &f;
    return nullptr;
}

TypeId field_type_or_unknown(const Schema& s, const std::string& name) {
    const Field* f = find_field(s, name);
    return f ? f->type.id : TypeId::Unknown;
}

// Aggregate output types for one GroupByOp/GroupByDynamicOp: each spec's
// value column type feeds agg_output_type (shared with agg_finalize's own
// dispatch, so this can never disagree with collect()).
std::vector<Field> group_agg_fields(const Schema& in,
                                    const std::vector<GroupAgg>& aggs) {
    std::vector<Field> out;
    out.reserve(aggs.size());
    for (const GroupAgg& a : aggs) {
        TypeId vt = a.column.empty() ? TypeId::Unknown
                                     : field_type_or_unknown(in, a.column);
        out.push_back(Field{a.out, agg_output_type(to_agg_op(a.op), vt), true});
    }
    return out;
}

// Mirrors dfops::unpivot's value-column type resolution (batch_ops.cpp):
// value_vars sharing one type keep it exactly (params included); otherwise,
// if every value_var is numeric, the common type is Int64 unless any is a
// float, else the plan is invalid and collect() will throw (report Unknown).
DataType unpivot_value_type(const Schema& in,
                            const std::vector<std::string>& value_vars) {
    if (value_vars.empty()) return scalar(TypeId::Unknown);
    const Field* first = find_field(in, value_vars.front());
    if (!first) return scalar(TypeId::Unknown);
    bool all_same = true, any_float = false, all_numeric = true;
    for (const std::string& name : value_vars) {
        const Field* f = find_field(in, name);
        if (!f) return scalar(TypeId::Unknown);
        if (f->type != first->type) all_same = false;
        if (f->type.id == TypeId::Float32 || f->type.id == TypeId::Float64)
            any_float = true;
        if (!is_numeric_dispatchable(f->type.id)) all_numeric = false;
    }
    if (all_same) return first->type;
    if (all_numeric) return scalar(any_float ? TypeId::Float64 : TypeId::Int64);
    return scalar(TypeId::Unknown);
}

Schema out_types(const LazyOp& op, Schema in) {
    return std::visit(
        overloaded{
            [&](const FilterOp&) { return in; },
            [&](const SliceOp&) { return in; },
            [&](const TailOp&) { return in; },
            [&](const DropNullsOp&) { return in; },
            // dftu_series_fillna only runs on a numeric-dispatchable column
            // (Int8-64/Uint8-64/Float32/64, none of which carry nested
            // parameters) and leaves every other column untouched, so
            // fill_null never changes a field's type.
            [&](const FillNullOp&) { return in; },
            [&](const SelectOp& o) {
                Schema out;
                out.fields.reserve(o.names.size());
                for (const std::string& name : o.names) {
                    const Field* f = find_field(in, name);
                    out.fields.push_back(
                        f ? *f : Field{name, scalar(TypeId::Unknown), true});
                }
                return out;
            },
            [&](const RenameOp& o) {
                Schema out;
                out.fields.reserve(o.names.size());
                for (std::size_t i = 0; i < o.names.size(); ++i) {
                    DataType t = i < in.fields.size() ? in.fields[i].type
                                                      : scalar(TypeId::Unknown);
                    out.fields.push_back(Field{o.names[i], t, true});
                }
                return out;
            },
            [&](const WithColumnOp& o) {
                std::vector<DataType> types;
                types.reserve(in.fields.size());
                for (const Field& f : in.fields) types.push_back(f.type);
                DataType t = infer_type(o.expr, types);
                auto it = std::find_if(
                    in.fields.begin(), in.fields.end(),
                    [&](const Field& f) { return f.name == o.name; });
                if (it != in.fields.end())
                    it->type = t;
                else
                    in.fields.push_back(Field{o.name, t, true});
                return in;
            },
            [&](const WithRowIndexOp& o) {
                in.fields.insert(in.fields.begin(),
                                 Field{o.name, scalar(TypeId::Int64), true});
                return in;
            },
            [&](const NullCountOp&) {
                for (Field& f : in.fields) f.type = scalar(TypeId::Int64);
                return in;
            },
            [&](const ExplodeOp& o) {
                for (Field& f : in.fields) {
                    if (f.name != o.column) continue;
                    const bool is_list = f.type.id == TypeId::List ||
                                         f.type.id == TypeId::LargeList ||
                                         f.type.id == TypeId::FixedSizeList;
                    f.type = is_list && !f.type.fields.empty()
                                 ? f.type.fields.front().type
                                 : scalar(TypeId::Unknown);
                }
                return in;
            },
            [&](const TopkOp&) { return in; },
            [&](const UnpivotOp& o) {
                Schema out;
                for (const std::string& name : o.id_vars) {
                    const Field* f = find_field(in, name);
                    out.fields.push_back(
                        f ? *f : Field{name, scalar(TypeId::Unknown), true});
                }
                out.fields.push_back(
                    Field{"variable", scalar(TypeId::String), true});
                out.fields.push_back(
                    Field{"value", unpivot_value_type(in, o.value_vars), true});
                return out;
            },
            [&](const GroupByOp& o) {
                if (!o.dyn.empty()) return Schema{};
                Schema out;
                for (const std::string& key : o.keys) {
                    const Field* f = find_field(in, key);
                    out.fields.push_back(
                        f ? *f : Field{key, scalar(TypeId::Unknown), true});
                }
                std::vector<Field> aggf = group_agg_fields(in, o.aggs);
                out.fields.insert(out.fields.end(), aggf.begin(), aggf.end());
                return out;
            },
            [&](const SortByOp&) { return in; },
            [&](const UniqueOp&) { return in; },
            [&](const SampleOp&) { return in; },
            [&](const IsDupOp& o) {
                return Schema{{Field{o.unique ? "is_unique" : "is_duplicated",
                                     scalar(TypeId::Bool), true}}};
            },
            [&](const GroupByDynamicOp& o) {
                Schema out;
                const Field* tf = find_field(in, o.time_col);
                out.fields.push_back(
                    tf ? *tf
                       : Field{o.time_col, scalar(TypeId::Unknown), true});
                std::vector<Field> aggf = group_agg_fields(in, o.aggs);
                out.fields.insert(out.fields.end(), aggf.begin(), aggf.end());
                return out;
            },
            [&](const PivotOp&) { return Schema{}; },
            [&](const ToDummiesOp&) { return Schema{}; },
            [&](const DescribeOp&) { return Schema{}; },
            [&](const ReverseOp&) { return in; },
            [&](const TakeOp&) { return in; },
            [&](const FilterMaskOp&) { return in; },
            [&](const SortByMultiOp&) { return in; },
            [&](const JoinOp& o) {
                Schema out;
                out.fields =
                    join_out_fields(in.fields, o.other.output_schema().fields,
                                    o.left_on, o.right_on, o.how, o.suffix);
                return out;
            },
            [&](const ConcatOp&) { return in; },
            [&](const UnnestOp& o) {
                Schema out;
                for (const Field& f : in.fields) {
                    if (f.name != o.column) {
                        out.fields.push_back(f);
                        continue;
                    }
                    const bool is_list = f.type.id == TypeId::List ||
                                         f.type.id == TypeId::LargeList ||
                                         f.type.id == TypeId::FixedSizeList;
                    if (!is_list || f.type.fields.empty()) {
                        out.fields.push_back(
                            Field{f.name, scalar(TypeId::Unknown), true});
                        continue;
                    }
                    const DataType& elem = f.type.fields.front().type;
                    if (elem.id == TypeId::Struct) {
                        for (const Field& sf : elem.fields)
                            out.fields.push_back(Field{sf.name, sf.type, true});
                    } else {
                        out.fields.push_back(Field{f.name, elem, true});
                    }
                }
                return out;
            },
            [&](const FrameOp& o) {
                Schema out;
                for (const std::string& name : o.out_names) {
                    const Field* f = find_field(in, name);
                    out.fields.push_back(
                        f ? *f : Field{name, scalar(TypeId::Unknown), true});
                }
                return out;
            },
            [&](const NodeOp& o) { return call_node_output_schema(o, in); }},
        op.node);
}

std::string describe_op(const LazyOp& op) {
    return std::visit(
        overloaded{
            [](const FilterOp&) { return std::string("filter"); },
            [](const SelectOp& o) {
                return "select [" + join_names(o.names) + "]";
            },
            [](const WithColumnOp& o) { return "with_column " + o.name; },
            [](const RenameOp& o) {
                return "rename [" + join_names(o.names) + "]";
            },
            [](const SliceOp&) { return std::string("slice"); },
            [](const TailOp&) { return std::string("tail"); },
            [](const DropNullsOp&) { return std::string("drop_nulls"); },
            [](const FillNullOp&) { return std::string("fill_null"); },
            [](const WithRowIndexOp& o) { return "with_row_index " + o.name; },
            [](const NullCountOp&) { return std::string("null_count"); },
            [](const ExplodeOp& o) { return "explode " + o.column; },
            [](const UnpivotOp&) { return std::string("unpivot"); },
            [](const TopkOp& o) { return "topk " + o.name; },
            [](const GroupByOp& o) { return "group_by " + join_names(o.keys); },
            [](const SortByOp& o) { return "sort_by " + o.name; },
            [](const UniqueOp& o) {
                return o.subset.empty()
                           ? std::string("unique")
                           : "unique [" + join_names(o.subset) + "]";
            },
            [](const SampleOp&) { return std::string("sample"); },
            [](const IsDupOp& o) {
                return std::string(o.unique ? "is_unique" : "is_duplicated");
            },
            [](const GroupByDynamicOp& o) {
                return "group_by_dynamic " + o.time_col;
            },
            [](const PivotOp& o) { return "pivot on " + o.on; },
            [](const ToDummiesOp& o) { return "to_dummies " + o.column; },
            [](const DescribeOp&) { return std::string("describe"); },
            [](const ReverseOp&) { return std::string("reverse"); },
            [](const TakeOp& o) {
                return "take [" + std::to_string(o.indices.size()) + "]";
            },
            [](const FilterMaskOp&) { return std::string("filter_mask"); },
            [](const SortByMultiOp& o) {
                return "sort_by_multi [" + join_names(o.by) + "]";
            },
            [](const JoinOp& o) {
                return "join " + std::string(join_how_name(o.how)) + " [" +
                       join_names(o.left_on) + "] = [" +
                       join_names(o.right_on) + "]";
            },
            [](const ConcatOp&) { return std::string("concat"); },
            [](const UnnestOp& o) {
                return "unnest " + o.column +
                       (o.keep_empty ? " keep_empty" : "");
            },
            [](const FrameOp& o) { return "frame_op " + o.name; },
            [](const NodeOp& o) { return "op " + o.name; }},
        op.node);
}

int col_index(const std::vector<std::string>& sch, const std::string& name) {
    auto it = std::find(sch.begin(), sch.end(), name);
    return it == sch.end() ? -1 : static_cast<int>(it - sch.begin());
}

// Predicate pushdown: bubble each Filter left past any WithColumn whose output
// column it does not read, so the filter shrinks the with_column's input. Safe
// on column indices because WithColumn appends or replaces in place; filters do
// not cross other ops (Select/Rename reindex, Slice/Tail change row counts).
std::vector<std::shared_ptr<const LazyOp>> pushdown_predicates(
    const std::vector<std::string>& source_names,
    const std::vector<std::shared_ptr<const LazyOp>>& ops) {
    struct Node {
        std::shared_ptr<const LazyOp> op;
        int write_idx;  // WithColumn output column index; -1 otherwise
    };
    std::vector<Node> nodes;
    nodes.reserve(ops.size());
    std::vector<std::string> sch = source_names;
    for (const auto& op : ops) {
        int w = -1;
        if (const auto* wc = std::get_if<WithColumnOp>(&op->node)) {
            w = col_index(sch, wc->name);
            if (w < 0) w = static_cast<int>(sch.size());
        }
        nodes.push_back({op, w});
        sch = out_schema(*op, std::move(sch));
    }

    // Hoist a filter past a preceding op that keeps its columns' positions and
    // rows: with_column (unless the predicate reads the written column),
    // sort_by, sort_by_multi, rename, reverse. A value-based filter selects
    // the same rows in the same relative order on either side of a reorder,
    // so sort_by_multi/reverse commute with it exactly as sort_by/rename do.
    // take and filter_mask get no branch: both are keyed by row position
    // against whatever stream reaches them, so hoisting a filter above them
    // would shift those positions and change which rows they act on. A
    // plugin NodeOp gets no branch either, for a stronger reason: it is
    // opaque, so the engine cannot know whether reordering around it is
    // safe and must always treat it as a barrier.
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 1; i < nodes.size(); ++i) {
            const auto* filt = std::get_if<FilterOp>(&nodes[i].op->node);
            if (!filt) continue;
            const LazyOp& prev = *nodes[i - 1].op;
            bool hoist = false;
            if (std::holds_alternative<WithColumnOp>(prev.node)) {
                hoist = !expr_references(filt->pred, nodes[i - 1].write_idx);
            } else if (std::holds_alternative<SortByOp>(prev.node) ||
                       std::holds_alternative<RenameOp>(prev.node) ||
                       std::holds_alternative<SortByMultiOp>(prev.node) ||
                       std::holds_alternative<ReverseOp>(prev.node)) {
                hoist = true;
            }
            if (hoist) {
                std::swap(nodes[i - 1], nodes[i]);
                changed = true;
            }
        }
    }

    std::vector<std::shared_ptr<const LazyOp>> out;
    out.reserve(nodes.size());
    for (Node& n : nodes) out.push_back(std::move(n.op));
    return out;
}

// Projection pushdown: for a schema-preserving plan (filter/sort_by/slice/tail/
// topk/sample/reverse/take/filter_mask/sort_by_multi) that ends in a select,
// insert a projection after the source
// keeping only the columns the output and ops read, renumbering the filters
// into it. Any other op - including a plugin NodeOp, deliberately absent from
// the whitelist below since it is opaque - leaves the plan unchanged; pushdown
// is an optimization, so bailing is always correct. Biggest payoff is a scan
// source that then reads only the kept columns.
std::vector<std::shared_ptr<const LazyOp>> pushdown_projections(
    const std::vector<std::string>& source_names,
    const std::vector<std::shared_ptr<const LazyOp>>& ops) {
    // The first op that names what it reads: a select, or a group-by (its
    // keys and the aggregates' columns; a dyn aggregate reads every arg
    // column, so it keeps the whole row). Whatever follows it reads its
    // output, which the projection does not change, and stays as it is.
    std::size_t last = ops.size();
    for (std::size_t i = 0; i < ops.size(); ++i)
        if (std::holds_alternative<SelectOp>(ops[i]->node) ||
            std::holds_alternative<GroupByOp>(ops[i]->node)) {
            last = i;
            break;
        }
    if (last == ops.size()) return ops;
    const auto* sel = std::get_if<SelectOp>(&ops[last]->node);
    const auto* gb = std::get_if<GroupByOp>(&ops[last]->node);
    if (sel && last == 0) return ops;  // a leading select is the projection
    if (gb && !gb->dyn.empty()) return ops;
    for (std::size_t i = 0; i < last; ++i) {
        const auto& n = ops[i]->node;
        if (!(std::holds_alternative<FilterOp>(n) ||
              std::holds_alternative<SortByOp>(n) ||
              std::holds_alternative<SliceOp>(n) ||
              std::holds_alternative<TailOp>(n) ||
              std::holds_alternative<TopkOp>(n) ||
              std::holds_alternative<SampleOp>(n) ||
              std::holds_alternative<ReverseOp>(n) ||
              std::holds_alternative<TakeOp>(n) ||
              std::holds_alternative<FilterMaskOp>(n) ||
              std::holds_alternative<SortByMultiOp>(n)))
            return ops;  // changes the schema or reads whole rows: bail
    }

    const int nsrc = static_cast<int>(source_names.size());
    std::vector<char> need(static_cast<std::size_t>(nsrc), 0);
    std::vector<std::string> wanted;
    if (sel) {
        wanted = sel->names;
    } else {
        wanted = gb->keys;
        for (const GroupAgg& a : gb->aggs) {
            if (!a.column.empty()) wanted.push_back(a.column);
            if (!a.by.empty()) wanted.push_back(a.by);
        }
    }
    for (const std::string& nm : wanted) {
        int c = col_index(source_names, nm);
        if (c < 0) return ops;
        need[static_cast<std::size_t>(c)] = 1;
    }
    // These ops preserve the source schema by position, so predicate indices
    // are source indices.
    for (std::size_t i = 0; i < last; ++i) {
        const auto& n = ops[i]->node;
        if (const auto* f = std::get_if<FilterOp>(&n)) {
            for (int c = 0; c < nsrc; ++c)
                if (expr_references(f->pred, c))
                    need[static_cast<std::size_t>(c)] = 1;
        } else if (const auto* s = std::get_if<SortByOp>(&n)) {
            int c = col_index(source_names, s->name);
            if (c >= 0) need[static_cast<std::size_t>(c)] = 1;
        } else if (const auto* t = std::get_if<TopkOp>(&n)) {
            int c = col_index(source_names, t->name);
            if (c >= 0) need[static_cast<std::size_t>(c)] = 1;
        } else if (const auto* sm = std::get_if<SortByMultiOp>(&n)) {
            for (const std::string& nm : sm->by) {
                int c = col_index(source_names, nm);
                if (c >= 0) need[static_cast<std::size_t>(c)] = 1;
            }
        }
        // reverse/take/filter_mask read no column of their own: nothing to add.
    }

    std::vector<std::string> live;
    std::vector<std::int32_t> old_to_new(static_cast<std::size_t>(nsrc), -1);
    for (int c = 0; c < nsrc; ++c)
        if (need[static_cast<std::size_t>(c)]) {
            old_to_new[static_cast<std::size_t>(c)] =
                static_cast<std::int32_t>(live.size());
            live.push_back(source_names[static_cast<std::size_t>(c)]);
        }
    if (static_cast<int>(live.size()) == nsrc) return ops;  // nothing to prune

    std::vector<std::shared_ptr<const LazyOp>> out;
    out.reserve(ops.size() + 1);
    out.push_back(std::make_shared<LazyOp>(LazyOp{SelectOp{live}}));
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const auto& op = ops[i];
        const auto* f = i < last ? std::get_if<FilterOp>(&op->node) : nullptr;
        if (f) {
            out.push_back(std::make_shared<LazyOp>(
                LazyOp{FilterOp{expr_remap_cols(f->pred, old_to_new)}}));
        } else {
            // sort/slice/tail/topk/sample and the terminal read by name; an
            // op after the terminal reads the terminal's output.
            out.push_back(op);
        }
    }
    return out;
}

struct PlanParts {
    std::shared_ptr<const Source> source;
    std::vector<std::shared_ptr<const LazyOp>> ops;
};

PlanParts optimize_parts(std::shared_ptr<const Source> source,
                         std::vector<std::shared_ptr<const LazyOp>> ops);

// `g` as an AggregateSpec: each named column is the expression `by` gives it
// (`names[i]` is `by[i]`), positional against the offering source.
std::optional<AggregateSpec> aggregate_spec(
    const GroupByOp& g, const std::vector<std::string>& names,
    const std::vector<Expr>& by) {
    if (!g.dyn.empty()) return std::nullopt;
    auto expr_of = [&](const std::string& name) -> std::optional<Expr> {
        const int c = col_index(names, name);
        if (c < 0) return std::nullopt;
        return by[static_cast<std::size_t>(c)];
    };
    AggregateSpec spec;
    spec.keys.reserve(g.keys.size());
    for (const std::string& key : g.keys) {
        std::optional<Expr> e = expr_of(key);
        if (!e) return std::nullopt;
        spec.keys.push_back({key, std::move(*e)});
    }
    spec.aggs.reserve(g.aggs.size());
    for (const GroupAgg& a : g.aggs) {
        AggregateExpr e;
        e.op = a.op;
        e.param = a.param;
        e.out = a.out;
        if (a.op != Agg::Count) {
            std::optional<Expr> in = expr_of(a.column);
            if (!in) return std::nullopt;
            e.input = std::move(*in);
        }
        if (!a.by.empty()) {
            std::optional<Expr> b = expr_of(a.by);
            if (!b) return std::nullopt;
            e.by = std::move(*b);
        }
        spec.aggs.push_back(std::move(e));
    }
    return spec;
}

// The aggregate-projection merge: with_column steps directly before a
// group-by, offered as one aggregation whose keys and inputs are their
// expressions over the source columns. The number of ops it covers, or 0.
std::size_t offer_merged_aggregation(
    const Source& source, const std::vector<std::string>& source_names,
    const std::vector<std::shared_ptr<const LazyOp>>& ops, std::size_t at,
    std::optional<SourceApplication>& app) {
    std::vector<std::string> names = source_names;
    std::vector<Expr> by;
    by.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i)
        by.push_back(expr_col(static_cast<std::int32_t>(i)));
    std::size_t j = at;
    for (; j < ops.size(); ++j) {
        const auto* w = std::get_if<WithColumnOp>(&ops[j]->node);
        if (!w) break;
        Expr e = expr_rebind_cols(w->expr, by);
        const int c = col_index(names, w->name);
        if (c >= 0) {
            by[static_cast<std::size_t>(c)] = std::move(e);
        } else {
            names.push_back(w->name);
            by.push_back(std::move(e));
        }
    }
    if (j == at || j >= ops.size()) return 0;
    const auto* g = std::get_if<GroupByOp>(&ops[j]->node);
    if (!g) return 0;
    std::optional<AggregateSpec> spec = aggregate_spec(*g, names, by);
    if (!spec) return 0;
    app = source.apply_aggregation(*spec);
    return app ? j - at + 1 : 0;
}

// Offer `op` to the matching planning hook. `schema_changing` reports whether
// an accepted answer replaces the source schema.
std::optional<SourceApplication> offer_op(const Source& source,
                                          const std::vector<std::string>& names,
                                          const LazyOp& op,
                                          bool& schema_changing) {
    schema_changing = false;
    if (const auto* f = std::get_if<FilterOp>(&op.node))
        return source.apply_filter(f->pred);
    if (const auto* s = std::get_if<SelectOp>(&op.node)) {
        std::vector<NamedExpr> exprs;
        exprs.reserve(s->names.size());
        for (const std::string& name : s->names) {
            const int c = col_index(names, name);
            if (c < 0) return std::nullopt;
            exprs.push_back({name, expr_col(c)});
        }
        schema_changing = true;
        return source.apply_projection(exprs);
    }
    if (const auto* g = std::get_if<GroupByOp>(&op.node)) {
        std::vector<Expr> by;
        by.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i)
            by.push_back(expr_col(static_cast<std::int32_t>(i)));
        std::optional<AggregateSpec> spec = aggregate_spec(*g, names, by);
        if (!spec) return std::nullopt;
        schema_changing = true;
        return source.apply_aggregation(*spec);
    }
    if (const auto* o = std::get_if<SortByOp>(&op.node))
        return source.apply_sort({{o->name}, {o->descending}});
    if (const auto* o = std::get_if<SortByMultiOp>(&op.node))
        return source.apply_sort({o->by, o->descending});
    if (const auto* o = std::get_if<TopkOp>(&op.node))
        return source.apply_topn({{o->name}, {o->largest}}, o->k);
    if (const auto* o = std::get_if<SliceOp>(&op.node)) {
        if (o->offset < 0 || o->len < 0) return std::nullopt;
        return source.apply_limit(o->offset, o->len);
    }
    if (const auto* o = std::get_if<TailOp>(&op.node)) {
        if (o->n < 0) return std::nullopt;
        return source.apply_tail(o->n);
    }
    if (const auto* j = std::get_if<JoinOp>(&op.node)) {
        // Only a right side that its own source fully absorbed can be handed
        // over; anything left above it would have to run first.
        PlanParts right =
            optimize_parts(detail::PlanAccess::source_ptr(j->other),
                           detail::PlanAccess::ops(j->other));
        if (!right.ops.empty()) return std::nullopt;
        schema_changing = true;
        return source.apply_join({std::move(right.source), j->left_on,
                                  j->right_on, j->how, j->suffix});
    }
    return std::nullopt;
}

// Offer ops to the source bottom up. An Exact answer removes the op; an
// Inexact one keeps it above the derived source, after which only filters
// (which commute with it) are still offered. The first refused op ends the
// walk.
PlanParts absorb_into_source(const std::vector<std::string>& source_names,
                             PlanParts plan) {
    std::shared_ptr<const Source> source = std::move(plan.source);
    std::vector<std::string> names = source_names;
    std::vector<std::shared_ptr<const LazyOp>> kept;
    kept.reserve(plan.ops.size());
    bool offering = true;
    bool kept_only_filters = true;
    auto accept = [&](SourceApplication app, const LazyOp& op,
                      bool schema_changing) {
        if (!app.source || app.source == source)
            throw std::logic_error("source planning hook accepted '" +
                                   describe_op(op) + "' without a new source");
        if (schema_changing && app.status != ApplyStatus::Exact)
            throw std::logic_error("source planning hook accepted '" +
                                   describe_op(op) +
                                   "' as inexact, but it changes the schema");
        source = std::move(app.source);
        if (schema_changing) names = source->names();
        return app.status;
    };
    for (std::size_t i = 0; i < plan.ops.size(); ++i) {
        auto& op = plan.ops[i];
        const bool is_filter = std::holds_alternative<FilterOp>(op->node);
        if (offering && !kept.empty() && !(kept_only_filters && is_filter))
            offering = false;
        if (!offering) {
            kept.push_back(std::move(op));
            continue;
        }
        if (kept.empty() && std::holds_alternative<WithColumnOp>(op->node)) {
            std::optional<SourceApplication> app;
            const std::size_t covered =
                offer_merged_aggregation(*source, names, plan.ops, i, app);
            if (covered) {
                accept(std::move(*app), *plan.ops[i + covered - 1], true);
                i += covered - 1;
                continue;
            }
        }
        bool schema_changing = false;
        std::optional<SourceApplication> app =
            offer_op(*source, names, *op, schema_changing);
        if (!app) {
            offering = false;
            kept.push_back(std::move(op));
            continue;
        }
        if (accept(std::move(*app), *op, schema_changing) ==
            ApplyStatus::Inexact) {
            kept_only_filters = kept_only_filters && is_filter;
            kept.push_back(std::move(op));
        }
    }
    return {std::move(source), std::move(kept)};
}

PlanParts run_plan_rule(detail::PlanRule rule,
                        const std::vector<std::string>& source_names,
                        PlanParts plan) {
    switch (rule) {
        case detail::PlanRule::PredicatePushdown:
            plan.ops = pushdown_predicates(source_names, plan.ops);
            return plan;
        case detail::PlanRule::ProjectionPushdown:
            plan.ops = pushdown_projections(source_names, plan.ops);
            return plan;
        case detail::PlanRule::SourceAbsorption:
            return absorb_into_source(source_names, std::move(plan));
    }
    return plan;
}

// Absorption is last, so the source names stay valid for every rule.
PlanParts optimize_parts(std::shared_ptr<const Source> source,
                         std::vector<std::shared_ptr<const LazyOp>> ops) {
    const std::vector<std::string> names = source->names();
    PlanParts plan{std::move(source), std::move(ops)};
    for (detail::PlanRule rule : detail::PLAN_PIPELINE)
        plan = run_plan_rule(rule, names, std::move(plan));
    return plan;
}

// Fuse [filter]* [with_column]* [select] into one pass: one AND-ed mask, gather
// only the columns the outputs read, one CSE-fused eval_many - no intermediate
// frame. nullopt for any other shape, or a with_column that replaces a column
// or reads another with_column; the caller then runs op-by-op.
std::optional<DataFrame> try_fuse_map(
    const DataFrame& src,
    const std::vector<std::shared_ptr<const LazyOp>>& ops) {
    std::vector<const FilterOp*> filters;
    std::vector<const WithColumnOp*> withs;
    const SelectOp* sel = nullptr;
    for (const auto& op : ops) {
        const auto& n = op->node;
        if (const auto* f = std::get_if<FilterOp>(&n)) {
            if (!withs.empty() || sel) return std::nullopt;
            filters.push_back(f);
        } else if (const auto* w = std::get_if<WithColumnOp>(&n)) {
            if (sel) return std::nullopt;
            withs.push_back(w);
        } else if (const auto* s = std::get_if<SelectOp>(&n)) {
            if (sel) return std::nullopt;
            sel = s;
        } else {
            return std::nullopt;
        }
    }
    if (!sel) return std::nullopt;

    const std::int32_t nsrc = static_cast<std::int32_t>(src.columns.size());
    // Only append-new with_columns that read source columns; see the contract.
    for (const WithColumnOp* w : withs) {
        if (col_index(src.names, w->name) >= 0) return std::nullopt;
        for (std::int32_t j = nsrc;
             j < nsrc + static_cast<std::int32_t>(withs.size()); ++j)
            if (expr_references(w->expr, j)) return std::nullopt;
    }

    std::vector<Expr> outs;
    outs.reserve(sel->names.size());
    for (const std::string& nm : sel->names) {
        const Expr* we = nullptr;
        for (const WithColumnOp* w : withs)
            if (w->name == nm) we = &w->expr;
        if (we) {
            outs.push_back(*we);
        } else {
            int c = col_index(src.names, nm);
            if (c < 0) return std::nullopt;
            outs.push_back(expr_col(c));
        }
    }

    std::vector<char> need(static_cast<std::size_t>(nsrc), 0);
    for (const Expr& e : outs)
        for (std::int32_t c = 0; c < nsrc; ++c)
            if (expr_references(e, c)) need[static_cast<std::size_t>(c)] = 1;
    std::vector<std::int32_t> old_to_new(static_cast<std::size_t>(nsrc), -1);
    DataFrame sub;
    for (std::int32_t c = 0; c < nsrc; ++c)
        if (need[static_cast<std::size_t>(c)]) {
            old_to_new[static_cast<std::size_t>(c)] =
                static_cast<std::int32_t>(sub.columns.size());
            sub.names.push_back(src.names[static_cast<std::size_t>(c)]);
            sub.columns.push_back(
                src.columns[static_cast<std::size_t>(c)].share());
        }

    if (!filters.empty()) {
        // A trivial `col <cmp> scalar` filter runs as a direct Series kernel;
        // only a compound predicate goes through the expression compiler.
        auto mask_of = [&](const Expr& p) -> Series {
            std::int32_t c;
            CmpOp op;
            Scalar rhs;
            if (expr_as_col_cmp(p, &c, &op, &rhs))
                return src.columns[static_cast<std::size_t>(c)].compare(op,
                                                                        rhs);
            return eval(p, column_ptrs(src.columns));
        };
        Series mask = mask_of(filters[0]->pred);
        for (std::size_t i = 1; i < filters.size(); ++i)
            mask = mask & mask_of(filters[i]->pred);
        sub = sub.filter(mask);
    }

    // Bare columns pass through zero-copy, `col <op> col` runs a direct Series
    // kernel, and only the rest go through the CSE-fused compiler.
    std::vector<Expr> computed;
    for (const Expr& e : outs) {
        std::int32_t a, b;
        BinaryOp op;
        if (expr_col_index(e) < 0 && !expr_as_col_binary(e, &op, &a, &b))
            computed.push_back(expr_remap_cols(e, old_to_new));
    }
    std::vector<Series> comp =
        computed.empty() ? std::vector<Series>{}
                         : eval_many(computed, column_ptrs(sub.columns));
    DataFrame out;
    out.names = sel->names;
    out.columns.reserve(outs.size());
    std::size_t ci = 0;
    for (const Expr& e : outs) {
        std::int32_t c = expr_col_index(e), a, b;
        BinaryOp op;
        if (c >= 0) {
            out.columns.push_back(
                sub.columns[static_cast<std::size_t>(old_to_new[c])].share());
        } else if (expr_as_col_binary(e, &op, &a, &b)) {
            const Series& x =
                sub.columns[static_cast<std::size_t>(old_to_new[a])];
            const Series& y =
                sub.columns[static_cast<std::size_t>(old_to_new[b])];
            switch (op) {
                case BinaryOp::Add:
                    out.columns.push_back(x.add(y));
                    break;
                case BinaryOp::Sub:
                    out.columns.push_back(x.sub(y));
                    break;
                case BinaryOp::Mul:
                    out.columns.push_back(x.mul(y));
                    break;
                case BinaryOp::Div:
                    out.columns.push_back(x.div(y));
                    break;
            }
        } else {
            out.columns.push_back(std::move(comp[ci++]));
        }
    }
    return out;
}

// Build the cursor for one op over `in`, resolving names against `sch` (the
// op's input schema).
std::unique_ptr<Cursor> make_cursor(const LazyOp& op,
                                    std::unique_ptr<Cursor> in,
                                    const std::vector<std::string>& sch,
                                    std::uint64_t budget) {
    return std::visit(
        overloaded{
            [&](const FilterOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<FilterCursor>(std::move(in), o.pred);
            },
            [&](const SelectOp& o) -> std::unique_ptr<Cursor> {
                std::vector<int> idx;
                idx.reserve(o.names.size());
                for (const std::string& nm : o.names)
                    idx.push_back(col_index(sch, nm));
                return std::make_unique<SelectCursor>(std::move(in),
                                                      std::move(idx));
            },
            [&](const WithColumnOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<WithColumnCursor>(
                    std::move(in), o.expr, col_index(sch, o.name), sch.size());
            },
            [&](const RenameOp&) -> std::unique_ptr<Cursor> {
                return std::move(in);  // names-only; data passes through
            },
            [&](const SliceOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<SliceCursor>(std::move(in), o.offset,
                                                     o.len);
            },
            [&](const TailOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<TailCursor>(std::move(in), o.n);
            },
            [&](const DropNullsOp&) -> std::unique_ptr<Cursor> {
                return std::make_unique<DropNullsCursor>(std::move(in));
            },
            [&](const FillNullOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<FillNullCursor>(std::move(in), o.value);
            },
            [&](const WithRowIndexOp&) -> std::unique_ptr<Cursor> {
                return std::make_unique<WithRowIndexCursor>(std::move(in));
            },
            [&](const NullCountOp&) -> std::unique_ptr<Cursor> {
                return std::make_unique<NullCountCursor>(std::move(in));
            },
            [&](const ExplodeOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<ExplodeCursor>(std::move(in), sch,
                                                       o.column);
            },
            [&](const UnpivotOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<UnpivotCursor>(std::move(in), sch,
                                                       o.id_vars, o.value_vars);
            },
            [&](const TopkOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<TopkCursor>(std::move(in), sch, o.name,
                                                    o.k, o.largest);
            },
            [&](const GroupByOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<GroupByCursor>(std::move(in), sch,
                                                       o.keys, o.aggs, budget,
                                                       o.dyn, o.dyn_prefix);
            },
            [&](const SortByOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<SortMergeCursor>(
                    std::move(in), sch, o.name, o.descending, budget);
            },
            [&](const SortByMultiOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<SortByMultiCursor>(
                    std::move(in), sch, o.by, o.descending, budget);
            },
            [&](const ReverseOp&) -> std::unique_ptr<Cursor> {
                return std::make_unique<ReverseCursor>(std::move(in), sch,
                                                       budget);
            },
            [&](const TakeOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<TakeCursor>(std::move(in), sch,
                                                    o.indices, budget);
            },
            [&](const FilterMaskOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<FilterMaskCursor>(std::move(in),
                                                          o.mask.share());
            },
            [&](const JoinOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<JoinCursor>(
                    std::move(in), sch, o.left_fields, o.other, o.left_on,
                    o.right_on, o.how, o.suffix);
            },
            [&](const ConcatOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<ConcatCursor>(std::move(in), o.other);
            },
            [&](const UnnestOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<UnnestCursor>(std::move(in), sch,
                                                      o.column, o.keep_empty);
            },
            [&](const FrameOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<FrameOpCursor>(
                    std::move(in), sch, o.name, o.op, o.args, budget);
            },
            [&](const UniqueOp& o) -> std::unique_ptr<Cursor> {
                std::vector<std::int64_t> key_idx;
                for (const std::string& name : o.subset) {
                    const int k = col_index(sch, name);
                    if (k < 0)
                        throw std::out_of_range("unique: no column named " +
                                                name);
                    key_idx.push_back(k);
                }
                return std::make_unique<UniqueCursor>(
                    std::move(in), sch, std::move(key_idx), budget);
            },
            [&](const SampleOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<SampleCursor>(std::move(in), sch, o.n,
                                                      o.seed);
            },
            [&](const IsDupOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<IsDupCursor>(std::move(in), budget,
                                                     o.unique);
            },
            [&](const GroupByDynamicOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<GroupByDynamicCursor>(
                    std::move(in), sch, o.time_col, o.every, o.period, o.aggs,
                    o.origin, o.origin_min);
            },
            [&](const PivotOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<PivotCursor>(
                    std::move(in), budget, sch, o.index, o.on, o.values, o.agg);
            },
            [&](const ToDummiesOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<ToDummiesCursor>(std::move(in), budget,
                                                         sch, o.column);
            },
            [&](const DescribeOp&) -> std::unique_ptr<Cursor> {
                return std::make_unique<DescribeCursor>(std::move(in), sch);
            },
            [&](const NodeOp& o) -> std::unique_ptr<Cursor> {
                Schema declared = call_node_output_schema(o, sch);
                return std::make_unique<NodeCursor>(o, std::move(in), sch,
                                                    std::move(declared));
            }},
        op.node);
}

}  // namespace

InMemorySource::InMemorySource(DataFrame frame)
    : frame_(std::make_shared<const DataFrame>(std::move(frame))) {}

Schema InMemorySource::schema() const {
    Schema s;
    s.fields.reserve(frame_->columns.size());
    for (std::size_t i = 0; i < frame_->columns.size(); ++i)
        s.fields.push_back(
            Field{frame_->names[i], frame_->columns[i].data_type(), true});
    return s;
}

ScanResult InMemorySource::scan(const ScanRequest& req) const {
    // Honor projection zero-copy: share only the requested columns, in the
    // requested order. Every filter stays No - the whole-column engine applies
    // them (a resident source usually takes the as_frame() fast path anyway).
    std::shared_ptr<const DataFrame> src = frame_;
    if (!req.projection.empty()) {
        DataFrame proj;
        proj.names = req.projection;
        proj.columns.reserve(req.projection.size());
        for (const std::string& nm : req.projection) {
            const int c = col_index(frame_->names, nm);
            proj.columns.push_back(
                frame_->columns[static_cast<std::size_t>(c)].share());
        }
        src = std::make_shared<const DataFrame>(std::move(proj));
    }
    ScanResult r;
    r.cursor = std::make_unique<InMemoryCursor>(std::move(src));
    r.filters.assign(req.filters.size(), Pushed::No);
    return r;
}

// Whole-column execution over a resident frame (map plans fuse via
// try_fuse_map): an op with an eager form runs it; any other runs its own
// cursor over the frame as one morsel, never spilling.
coro::CoroTask<DataFrame> LazyFrame::run_ops_in_memory(
    DataFrame df, std::vector<std::shared_ptr<const LazyOp>> ops) {
    if (auto fused = try_fuse_map(df, ops)) co_return std::move(*fused);
    for (const auto& op : ops) {
        bool ok = true;
        DataFrame next = std::visit(
            overloaded{
                [&](const FilterOp& o) {
                    return df.filter(eval(o.pred, column_ptrs(df.columns)));
                },
                [&](const WithColumnOp& o) {
                    return df.with_column(
                        o.name, eval(o.expr, column_ptrs(df.columns)));
                },
                [&](const SelectOp& o) { return df.select(o.names); },
                [&](const RenameOp& o) { return df.rename(o.names); },
                [&](const SliceOp& o) { return df.slice(o.offset, o.len); },
                [&](const TailOp& o) { return df.tail(o.n); },
                [&](const DropNullsOp&) { return df.drop_nulls(); },
                [&](const FillNullOp& o) { return df.fill_null(o.value); },
                [&](const SortByOp& o) {
                    return df.sort_by(o.name, o.descending);
                },
                [&](const SortByMultiOp& o) {
                    return df.sort_by_multi(o.by, o.descending);
                },
                [&](const ReverseOp&) { return df.reverse(); },
                [&](const TakeOp& o) { return df.take(o.indices); },
                [&](const FilterMaskOp& o) { return df.filter(o.mask); },
                [&](const UniqueOp& o) { return df.unique(o.subset); },
                [&](const SampleOp& o) { return df.sample(o.n, o.seed); },
                [&](const TopkOp& o) {
                    return df.topk(o.name, o.k, o.largest);
                },
                [&](const WithRowIndexOp& o) {
                    return df.with_row_index(o.name);
                },
                [&](const NullCountOp&) { return df.null_count(); },
                [&](const GroupByDynamicOp& o) {
                    return df.group_by_dynamic(o.time_col, o.every, o.period,
                                               o.aggs, o.origin, o.origin_min);
                },
                [&](const GroupByOp& o) {
                    if (o.dyn.empty()) return df.group_by(o.keys, o.aggs);
                    // DataFrame::group_by drops the dyn side-channel; run the
                    // same AggState path the streaming cursor uses.
                    LoweredGroupAggs lowered = lower_group_aggs(o.aggs);
                    AggStatePtr state = agg_new(lowered.specs, o.dyn);
                    agg_accumulate_chunk(*state, df, o.keys,
                                         lowered.value_names, o.dyn_prefix);
                    return agg_finalize(*state, o.keys);
                },
                [&](const auto&) {
                    ok = false;
                    return DataFrame{};
                }},
            op->node);
        if (!ok) {
            const std::int64_t rows = std::max<std::int64_t>(df.num_rows(), 1);
            const LazyFrame whole(
                std::make_shared<InMemorySource>(std::move(df)), {op},
                NO_SPILL_BUDGET);
            next = co_await drain_stream(whole.stream(rows));
        }
        df = std::move(next);
    }
    co_return df;
}

LazyFrame LazyFrame::scan(std::shared_ptr<const Source> source) {
    return LazyFrame(std::move(source), {});
}

LazyFrame LazyFrame::select(std::vector<std::string> names) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{SelectOp{std::move(names)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::filter(Expr predicate) const {
    auto ops = ops_;
    ops.push_back(
        std::make_shared<LazyOp>(LazyOp{FilterOp{std::move(predicate)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::with_column(std::string name, Expr expr) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(
        LazyOp{WithColumnOp{std::move(name), std::move(expr)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::rename(std::vector<std::string> names) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{RenameOp{std::move(names)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::slice(std::int64_t offset, std::int64_t len) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{SliceOp{offset, len}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::head(std::int64_t n) const { return slice(0, n); }

LazyFrame LazyFrame::tail(std::int64_t n) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{TailOp{n}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::drop_nulls() const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{DropNullsOp{}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::fill_null(Scalar value) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{FillNullOp{value}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::take(std::vector<std::int64_t> indices) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{TakeOp{std::move(indices)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::filter_mask(Series mask) const {
    auto ops = ops_;
    ops.push_back(
        std::make_shared<LazyOp>(LazyOp{FilterMaskOp{std::move(mask)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::reverse() const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{ReverseOp{}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::with_row_index(std::string name) const {
    auto ops = ops_;
    ops.push_back(
        std::make_shared<LazyOp>(LazyOp{WithRowIndexOp{std::move(name)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::null_count() const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{NullCountOp{}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::explode(std::string column) const {
    auto ops = ops_;
    ops.push_back(
        std::make_shared<LazyOp>(LazyOp{ExplodeOp{std::move(column)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::unpivot(std::vector<std::string> id_vars,
                             std::vector<std::string> value_vars) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(
        LazyOp{UnpivotOp{std::move(id_vars), std::move(value_vars)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::topk(std::string name, std::int64_t k,
                          bool largest) const {
    auto ops = ops_;
    ops.push_back(
        std::make_shared<LazyOp>(LazyOp{TopkOp{std::move(name), k, largest}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::group_by(std::string key,
                              std::vector<GroupAgg> aggs) const {
    return group_by(std::vector<std::string>{std::move(key)}, std::move(aggs));
}

LazyFrame LazyFrame::group_by(std::vector<std::string> keys,
                              std::vector<GroupAgg> aggs,
                              std::vector<AggDynSpec> dyn,
                              std::string dyn_prefix) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(
        LazyOp{GroupByOp{std::move(keys), std::move(aggs), std::move(dyn),
                         std::move(dyn_prefix)}}));
    return with_ops(std::move(ops));
}

std::vector<GroupAgg> LazyFrame::reduce_specs(
    Agg agg, const std::vector<std::string>& keys) const {
    const Schema in = output_schema();
    std::vector<std::string> names;
    std::vector<TypeId> types;
    names.reserve(in.fields.size());
    types.reserve(in.fields.size());
    for (const Field& f : in.fields) {
        names.push_back(f.name);
        types.push_back(f.type.id);
    }
    for (const std::string& k : keys)
        if (col_index(names, k) < 0)
            throw std::out_of_range("reduce: no column named " + k);
    return dataframe::reduce_specs(names, types, agg, keys);
}

LazyFrame LazyFrame::reduce(Agg agg) const {
    return group_by(std::vector<std::string>{}, reduce_specs(agg));
}

LazyGroupBy LazyFrame::group_by(std::vector<std::string> keys) const {
    return LazyGroupBy(*this, std::move(keys));
}

LazyGroupBy::LazyGroupBy(LazyFrame plan, std::vector<std::string> keys)
    : plan_(std::move(plan)), keys_(std::move(keys)) {
    const std::vector<std::string> names = plan_.schema();
    if (!names.empty())
        for (const std::string& k : keys_)
            if (col_index(names, k) < 0)
                throw std::out_of_range("group_by: no column named " + k);
}

LazyFrame LazyGroupBy::agg(std::vector<GroupAgg> aggs) const {
    return plan_.group_by(keys_, std::move(aggs));
}

LazyFrame LazyGroupBy::reduce(Agg agg) const {
    return plan_.group_by(keys_, plan_.reduce_specs(agg, keys_));
}

LazyFrame LazyGroupBy::size() const {
    return plan_.group_by(keys_, {GroupAgg{Agg::Count, "", "size"}});
}

namespace {
// A computed KEY becomes the output key column, so it must be named as the
// eager DataFrame::group_by(Expr) path does ("key" for one key, "key<k>" for N)
// or lazy and eager schemas diverge; value/by temps stay hidden ("__gb_").
LazyFrame desugar_expr_group_by(const LazyFrame& self, std::vector<Expr> keys,
                                std::vector<AggExprSpec> aggs,
                                bool single_key) {
    LazyFrame lf = self;
    std::vector<std::string> sch = lf.schema();
    int tmp = 0;
    auto resolve_val = [&](const Expr& e, const char* prefix) -> std::string {
        const std::int32_t idx = expr_col_index(e);
        if (idx >= 0 && static_cast<std::size_t>(idx) < sch.size())
            return sch[static_cast<std::size_t>(idx)];
        std::string name =
            std::string("__gb_") + prefix + std::to_string(tmp++);
        lf = lf.with_column(name, e);
        sch.push_back(name);
        return name;
    };

    std::vector<std::string> key_names;
    key_names.reserve(keys.size());
    for (std::size_t k = 0; k < keys.size(); ++k) {
        const std::int32_t idx = expr_col_index(keys[k]);
        if (idx >= 0 && static_cast<std::size_t>(idx) < sch.size()) {
            key_names.push_back(sch[static_cast<std::size_t>(idx)]);
            continue;
        }
        std::string name = single_key ? "key" : "key" + std::to_string(k);
        lf = lf.with_column(name, keys[k]);
        sch.push_back(name);
        key_names.push_back(std::move(name));
    }

    std::vector<GroupAgg> gaggs;
    gaggs.reserve(aggs.size());
    for (const AggExprSpec& a : aggs) {
        GroupAgg g;
        g.op = from_agg_op(a.op);
        g.out = a.out;
        g.param = a.param;
        if (a.op != AggOp::Count) g.column = resolve_val(a.value, "v");
        if (agg_uses_by_col(a.op)) g.by = resolve_val(a.by, "by");
        gaggs.push_back(std::move(g));
    }
    return lf.group_by(std::move(key_names), std::move(gaggs));
}
}  // namespace

LazyFrame LazyFrame::group_by(Expr key, std::vector<AggExprSpec> aggs) const {
    return desugar_expr_group_by(*this, std::vector<Expr>{std::move(key)},
                                 std::move(aggs), /*single_key=*/true);
}

LazyFrame LazyFrame::group_by(std::vector<Expr> keys,
                              std::vector<AggExprSpec> aggs) const {
    return desugar_expr_group_by(*this, std::move(keys), std::move(aggs),
                                 /*single_key=*/false);
}

LazyFrame LazyFrame::sort_by(std::string name, bool descending) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(
        LazyOp{SortByOp{std::move(name), descending}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::sort_by_multi(std::vector<std::string> by,
                                   bool descending) const {
    std::vector<bool> flags(by.size(), descending);
    return sort_by_multi(std::move(by), std::move(flags));
}

LazyFrame LazyFrame::sort_by_multi(std::vector<std::string> by,
                                   std::vector<bool> descending) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(
        LazyOp{SortByMultiOp{std::move(by), std::move(descending)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::unique(std::vector<std::string> subset) const {
    const std::vector<std::string> names = schema();
    if (!names.empty())
        for (const std::string& s : subset)
            if (col_index(names, s) < 0)
                throw std::out_of_range("unique: no column named " + s);
    auto ops = ops_;
    ops.push_back(
        std::make_shared<LazyOp>(LazyOp{UniqueOp{std::move(subset)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::drop_duplicates(std::vector<std::string> subset) const {
    return unique(std::move(subset));
}

LazyFrame LazyFrame::sample(std::int64_t n, std::uint64_t seed) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{SampleOp{n, seed}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::is_duplicated() const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{IsDupOp{false}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::is_unique() const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{IsDupOp{true}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::group_by_dynamic(std::string time_col, std::int64_t every,
                                      std::int64_t period,
                                      std::vector<GroupAgg> aggs,
                                      std::int64_t origin,
                                      bool origin_min) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(
        LazyOp{GroupByDynamicOp{std::move(time_col), every, period,
                                std::move(aggs), origin, origin_min}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::melt(std::vector<std::string> id_vars,
                          std::vector<std::string> value_vars) const {
    return unpivot(std::move(id_vars), std::move(value_vars));
}

LazyFrame LazyFrame::join(LazyFrame other, std::vector<std::string> left_on,
                          std::vector<std::string> right_on, JoinHow how,
                          std::string suffix) const {
    if (!valid_join_how(how))
        throw std::invalid_argument("join: unknown join kind " +
                                    std::to_string(static_cast<int>(how)));
    if (how == JoinHow::Cross) {
        left_on.clear();
        right_on.clear();
    } else if (left_on.empty()) {
        throw std::invalid_argument("join: no key columns");
    }
    if (left_on.size() != right_on.size())
        throw std::invalid_argument(
            "join: left_on and right_on differ in length");
    auto ops = ops_;
    std::vector<Field> left_fields = output_schema().fields;
    ops.push_back(std::make_shared<LazyOp>(
        LazyOp{JoinOp{std::move(other), std::move(left_on), std::move(right_on),
                      how, std::move(suffix), std::move(left_fields)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::join(LazyFrame other, std::vector<std::string> on,
                          JoinHow how, std::string suffix) const {
    std::vector<std::string> right_on = on;
    return join(std::move(other), std::move(on), std::move(right_on), how,
                std::move(suffix));
}

LazyFrame LazyFrame::unnest(std::string column, bool keep_empty) const {
    const Schema in = output_schema();
    std::vector<std::string> names;
    bool found = false;
    bool known = true;
    for (const Field& f : in.fields) {
        if (f.name != column) {
            names.push_back(f.name);
            continue;
        }
        found = true;
        const bool is_list = f.type.id == TypeId::List ||
                             f.type.id == TypeId::LargeList ||
                             f.type.id == TypeId::FixedSizeList;
        if (f.type.id == TypeId::Unknown) {
            known = false;
        } else if (!is_list) {
            throw std::invalid_argument("unnest: " + column +
                                        " is not a List column");
        } else if (!f.type.fields.empty() &&
                   f.type.fields.front().type.id == TypeId::Struct) {
            for (const Field& sf : f.type.fields.front().type.fields)
                names.push_back(sf.name);
        } else if (f.type.fields.empty()) {
            known = false;
        } else {
            names.push_back(f.name);
        }
    }
    if (!found && !in.fields.empty())
        throw std::out_of_range("unnest: no column named " + column);
    if (!known || in.fields.empty()) names.clear();
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(
        LazyOp{UnnestOp{std::move(column), keep_empty, std::move(names)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::frame_op(std::string name, OpArgs args,
                              std::vector<LazyFrame> others,
                              std::vector<std::string> out_names) const {
    const dftu_op_desc* op = dftu_op_find(name.c_str());
    if (!op)
        throw std::invalid_argument("lazy frame op '" + name +
                                    "': no op registered under this name");
    if (dftu_op_kind_of(op->sig) != DFTU_OP_KIND_FRAME ||
        DFTU_OP_SIG_ARG(op->sig, 0) != DFTU_TOK_FRAME)
        throw std::invalid_argument("lazy frame op '" + name +
                                    "': not a table -> table op");
    auto owned =
        std::make_shared<const OwnedFrameOpArgs>(*op, args, std::move(others));
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{
        FrameOp{std::move(name), op, std::move(owned), std::move(out_names)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::compare_agg(LazyFrame variant, std::int64_t n_key) const {
    if (n_key < 1) throw std::invalid_argument("compare_agg: n_key < 1");
    OpArgs args;
    args.i64(2, n_key);
    return frame_op("dftu.frame.compare_agg", args, {std::move(variant)});
}

LazyFrame LazyFrame::concat(LazyFrame other) const {
    const std::vector<Field> lhs = output_schema().fields;
    const std::vector<Field> rhs = other.output_schema().fields;
    if (lhs.size() != rhs.size())
        throw std::invalid_argument("concat: column count differs (" +
                                    std::to_string(lhs.size()) + " vs " +
                                    std::to_string(rhs.size()) + ")");
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].name != rhs[i].name)
            throw std::invalid_argument("concat: column " + std::to_string(i) +
                                        " is '" + lhs[i].name + "' vs '" +
                                        rhs[i].name + "'");
        if (lhs[i].type.id != TypeId::Unknown &&
            rhs[i].type.id != TypeId::Unknown && lhs[i].type != rhs[i].type)
            throw std::invalid_argument("concat: column '" + lhs[i].name +
                                        "' type differs");
    }
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{ConcatOp{std::move(other)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::pivot(std::string index, std::string on,
                           std::string values, std::string agg) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{PivotOp{
        std::move(index), std::move(on), std::move(values), std::move(agg)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::to_dummies(std::string column) const {
    auto ops = ops_;
    ops.push_back(
        std::make_shared<LazyOp>(LazyOp{ToDummiesOp{std::move(column)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::describe() const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{DescribeOp{}}));
    return with_ops(std::move(ops));
}

Schema LazyFrame::output_schema() const {
    Schema s = source_->schema();
    for (const auto& op : ops_) s = out_types(*op, std::move(s));
    return s;
}

std::vector<std::string> LazyFrame::schema() const {
    Schema s = output_schema();
    std::vector<std::string> names;
    names.reserve(s.fields.size());
    for (const Field& f : s.fields) names.push_back(f.name);
    return names;
}

std::string LazyFrame::explain() const {
    const PlanParts plan = optimize_parts(source_, ops_);
    std::string s = "scan [" + join_names(plan.source->names()) + "]";
    for (const auto& op : plan.ops) s += "\n" + describe_op(*op);
    return s;
}

LazyFrame LazyFrame::memory_budget(std::uint64_t bytes) const {
    return LazyFrame(source_, ops_, bytes);
}

LazyFrame LazyFrame::auto_spill() const {
    // Same policy as the default (0) and as View: ~1/3 of available
    // memory.
    return LazyFrame(source_, ops_, resolve_spill_budget(0));
}

LazyFrame LazyFrame::op(std::string name, OpArgs args) const {
    std::optional<NodeEntry> entry = find_node(name.c_str());
    if (!entry)
        throw std::invalid_argument("lazy op '" + name +
                                    "': no node registered under this name");
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(
        LazyOp{NodeOp{std::move(name), entry->vt, entry->self, args.raw()}}));
    return with_ops(std::move(ops));
}

namespace {

// The pull chain a streaming terminal drives: the source cursor with every
// op the source did not apply stacked on it, plus the schema at its output.
struct CursorChain {
    // Declared first so it outlives the cursor built over it.
    std::shared_ptr<const Source> source;
    std::unique_ptr<Cursor> cursor;
    std::vector<std::string> schema;
    std::uint64_t budget = 0;
    // Every stage the lowering built and still alive, whoever owns it now.
    std::shared_ptr<ReclaimRegistry> registry;
};

// The chain's resident bytes against its budget: when over, the largest
// holders are asked to reclaim, in turn, until it fits or nothing more
// comes back. A stage that cannot measure itself reports 0 and is never
// asked. Advisory throughout: a stage may free less than asked, and a
// chain that stays over after every stage has been asked is logged once
// rather than stopped, since the result is not in question.
coro::CoroTask<void> reclaim_chain(CursorChain& chain, bool& reported) {
    if (chain.budget == 0 || !chain.registry) co_return;
    const std::vector<Cursor*> stages = chain.registry->snapshot();
    std::uint64_t total = 0;
    std::vector<std::pair<std::uint64_t, Cursor*>> holders;
    for (Cursor* c : stages) {
        const std::uint64_t b = c->resident_bytes();
        total += b;
        if (b) holders.emplace_back(b, c);
    }
    if (total <= chain.budget) co_return;
    std::sort(holders.begin(), holders.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [bytes, c] : holders) {
        const std::uint64_t want = total - chain.budget;
        const std::uint64_t freed = co_await c->reclaim(want);
        total -= std::min(freed, total);
        if (total <= chain.budget) co_return;
    }
    if (!reported) {
        reported = true;
        DFTRACER_UTILS_LOG_WARN(
            "lazy plan holds %llu bytes past its %llu byte budget and no "
            "stage could reclaim the rest",
            static_cast<unsigned long long>(total),
            static_cast<unsigned long long>(chain.budget));
    }
}

// Lower `ops` onto `source`: push a leading projection and the contiguous run
// of filters after it into the scan, then stack every op the source did not
// apply exactly. This is the plan's lowering (b); the driver below runs it.
CursorChain lower_cursor_chain(
    std::shared_ptr<const Source> source_ptr,
    const std::vector<std::shared_ptr<const LazyOp>>& ops,
    std::uint64_t budget) {
    const Source& source = *source_ptr;
    const std::vector<std::string> names = source.names();

    // A leading Select is a pure source projection: pushdown_projections emits
    // one with the filter col-refs already remapped into it, and a user's own
    // leading select is one too. Push it into the scan so the source harvests
    // only those columns; the source returns them in this exact order, so the
    // now-identity Select and the remapped filters stay positionally aligned.
    std::vector<std::string> projection;
    std::size_t first = 0;
    if (!ops.empty())
        if (const auto* s = std::get_if<SelectOp>(&ops.front()->node)) {
            projection = s->names;
            first = 1;
        }

    // Candidate filters: the contiguous run right after the optional
    // projection. Their col-refs are positional against the scan's column set
    // (projection when present, else the source schema), which is exactly what
    // scan() resolves them against.
    ScanRequest req;
    req.projection = projection;
    req.memory_budget = budget;
    std::vector<std::size_t> cand_pos;
    for (std::size_t i = first; i < ops.size(); ++i) {
        const auto* f = std::get_if<FilterOp>(&ops[i]->node);
        if (!f) break;
        req.filters.push_back(f->pred);
        cand_pos.push_back(i);
    }

    // A slice reachable from the scan through row-preserving ops bounds how
    // many rows the source need produce. Only when NOTHING removes rows on the
    // way: the source applies the request as a whole, and whether it honored a
    // filter is only known from the ScanResult it has not returned yet, so a
    // limit sent alongside an unapplied filter would truncate the source before
    // the engine ever filters. req.filters being empty is what rules that out.
    // The Slice op still runs below - limit is a hint, and a source may return
    // more rows than asked (or ignore it entirely).
    if (req.filters.empty()) {
        for (std::size_t i = first; i < ops.size(); ++i) {
            const auto& n = ops[i]->node;
            if (const auto* s = std::get_if<SliceOp>(&n)) {
                // offset + len is how many rows must be produced to satisfy
                // the window. Signed overflow is UB, so leave the limit unset
                // when the sum would not fit; unset just means the source
                // produces everything, which is what a window that large
                // wanted anyway.
                if (s->offset >= 0 && s->len >= 0 &&
                    s->offset <=
                        std::numeric_limits<std::int64_t>::max() - s->len)
                    req.limit = s->offset + s->len;
                break;
            }
            // Row-count- and order-preserving ops keep a source prefix a valid
            // prefix of this op's output; anything else (filter, sort, group,
            // explode, unique, sample, take, reverse) does not.
            if (!std::holds_alternative<SelectOp>(n) &&
                !std::holds_alternative<WithColumnOp>(n) &&
                !std::holds_alternative<RenameOp>(n) &&
                !std::holds_alternative<FillNullOp>(n) &&
                !std::holds_alternative<WithRowIndexOp>(n))
                break;
        }
    }

    ScanResult r = source.scan(req);

    // Drop every candidate the source applied exactly; the engine re-applies
    // No/Inexact (and any filter deeper in the plan).
    std::vector<bool> drop(ops.size(), false);
    for (std::size_t k = 0; k < cand_pos.size() && k < r.filters.size(); ++k)
        if (r.filters[k] == Pushed::Exact) drop[cand_pos[k]] = true;

    CursorChain chain;
    chain.source = std::move(source_ptr);
    chain.cursor = std::move(r.cursor);
    chain.schema = projection.empty() ? names : projection;
    chain.budget = budget;
    // Bottom up, each stage enrolled before the next one wraps it, so a
    // stage a plugin node later takes ownership of is already on the list.
    chain.registry = std::make_shared<ReclaimRegistry>();
    chain.cursor->attach(chain.registry);
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (drop[i]) continue;
        chain.cursor =
            make_cursor(*ops[i], std::move(chain.cursor), chain.schema, budget);
        chain.schema = out_schema(*ops[i], std::move(chain.schema));
        // A names-only op (rename) hands the same, already enrolled, cursor
        // back; attach re-enrols it once.
        chain.cursor->attach(chain.registry);
    }
    return chain;
}

// The pull driver: demand up, data down, one morsel at a time. A stage that
// must wait co_awaits inside next(), so the worker is never parked. Abandoning
// this generator drops the chain, and dropping the source cursor is the scan
// early-out (~StreamViewCursor stops and wakes the producer).
coro::AsyncGenerator<DataFrame> drive_cursor_chain(CursorChain chain,
                                                   std::int64_t rows) {
    bool reported = false;
    while (auto m = co_await chain.cursor->next(rows)) {
        DataFrame out = frame_from_morsel(std::move(*m), chain.schema,
                                          chain.cursor->out_names());
        co_await reclaim_chain(chain, reported);
        co_yield std::move(out);
    }
}

}  // namespace

namespace {

coro::AsyncGenerator<DataFrame> stream_plan(PlanParts plan,
                                            std::uint64_t memory_budget,
                                            std::int64_t morsel_rows) {
    // 0 resolves to auto (~1/3 RAM), same policy as View.
    const std::uint64_t budget = resolve_spill_budget(memory_budget);
    const std::int64_t eff_rows =
        morsel_rows > 0 ? morsel_rows : DEFAULT_MORSEL_ROWS;

    // Returned, not re-yielded through: an extra coroutine layer around a
    // parallel drain has miscompiled frames before (project_coro_threadlocal_
    // parser_pitfall), and it would delay the chain's destruction past the
    // consumer's last pull, which is the scan early-out.
    return drive_cursor_chain(
        lower_cursor_chain(std::move(plan.source), plan.ops, budget), eff_rows);
}

}  // namespace

coro::AsyncGenerator<DataFrame> LazyFrame::stream(
    std::int64_t morsel_rows) const {
    return stream_plan(optimize_parts(source_, ops_), memory_budget_,
                       morsel_rows);
}

std::unique_ptr<Cursor> LazyFrame::open_cursor() const {
    PlanParts plan = optimize_parts(source_, ops_);
    const std::uint64_t budget = resolve_spill_budget(memory_budget_);
    return lower_cursor_chain(std::move(plan.source), plan.ops, budget).cursor;
}

namespace {

DataFrame share_frame(const DataFrame& f) {
    DataFrame out;
    out.names = f.names;
    out.columns.reserve(f.columns.size());
    for (const Series& c : f.columns) out.columns.push_back(c.share());
    return out;
}

coro::CoroTask<DataFrame> collect_plan(PlanParts plan,
                                       std::uint64_t memory_budget,
                                       std::int64_t morsel_rows) {
    // A resident source runs whole-column, matching eager. An explicit
    // morsel_rows or memory_budget asks to stream/spill instead.
    if (morsel_rows <= 0 && memory_budget == 0) {
        if (const DataFrame* f = plan.source->as_frame())
            co_return co_await detail::PlanAccess::run_in_memory(
                share_frame(*f), std::move(plan.ops));
    }
    co_return co_await drain_stream(
        stream_plan(std::move(plan), memory_budget, morsel_rows));
}

}  // namespace

namespace {

// A plan optimized at every level: its own source and ops, and each child
// plan (a join's or concat's other side, a frame op's frame operands) keyed
// by the op that holds it.
struct PlannedTree {
    PlanParts parts;
    std::uint64_t memory_budget = 0;
    std::vector<std::pair<std::size_t, std::vector<PlannedTree>>> children;
};

PlannedTree plan_tree(const LazyFrame& lf) {
    using detail::PlanAccess;
    PlannedTree t{
        optimize_parts(PlanAccess::source_ptr(lf), PlanAccess::ops(lf)),
        PlanAccess::memory_budget(lf),
        {}};
    for (std::size_t i = 0; i < t.parts.ops.size(); ++i) {
        const LazyOp& op = *t.parts.ops[i];
        std::vector<PlannedTree> kids;
        if (const auto* j = std::get_if<JoinOp>(&op.node)) {
            kids.push_back(plan_tree(j->other));
        } else if (const auto* c = std::get_if<ConcatOp>(&op.node)) {
            kids.push_back(plan_tree(c->other));
        } else if (const auto* f = std::get_if<FrameOp>(&op.node)) {
            if (f->args)
                for (const LazyFrame& child : f->args->frames())
                    kids.push_back(plan_tree(child));
        }
        if (!kids.empty()) t.children.emplace_back(i, std::move(kids));
    }
    return t;
}

void tree_leaves(PlannedTree& t, std::vector<PlannedTree*>& out) {
    out.push_back(&t);
    for (auto& [idx, kids] : t.children)
        for (PlannedTree& k : kids) tree_leaves(k, out);
}

// A source over one cursor a batch already opened; scanned exactly once.
class OpenedSource final : public Source {
   public:
    OpenedSource(Schema schema, std::unique_ptr<Cursor> cursor)
        : schema_(std::move(schema)), cursor_(std::move(cursor)) {}

    Schema schema() const override { return schema_; }

    ScanResult scan(const ScanRequest& req) const override {
        if (!req.projection.empty() && req.projection != names())
            throw std::logic_error(
                "a batch-opened source cannot take a projection");
        std::lock_guard<std::mutex> lock(mu_);
        if (!cursor_)
            throw std::logic_error("a batch-opened source was scanned twice");
        ScanResult r;
        r.filters.assign(req.filters.size(), Pushed::No);
        r.cursor = std::move(cursor_);
        return r;
    }

   private:
    Schema schema_;
    mutable std::mutex mu_;
    mutable std::unique_ptr<Cursor> cursor_;
};

// The tree's own parts with every child op pointing at its rebuilt child.
PlanParts finish_tree(PlannedTree t) {
    for (auto& [idx, kids] : t.children) {
        std::vector<LazyFrame> rebuilt;
        rebuilt.reserve(kids.size());
        for (PlannedTree& k : kids) {
            const std::uint64_t budget = k.memory_budget;
            PlanParts p = finish_tree(std::move(k));
            rebuilt.push_back(detail::PlanAccess::make(
                std::move(p.source), std::move(p.ops), budget));
        }
        const LazyOp& op = *t.parts.ops[idx];
        if (const auto* j = std::get_if<JoinOp>(&op.node)) {
            JoinOp next = *j;
            next.other = std::move(rebuilt.front());
            t.parts.ops[idx] =
                std::make_shared<LazyOp>(LazyOp{std::move(next)});
        } else if (const auto* c = std::get_if<ConcatOp>(&op.node)) {
            ConcatOp next = *c;
            next.other = std::move(rebuilt.front());
            t.parts.ops[idx] =
                std::make_shared<LazyOp>(LazyOp{std::move(next)});
        } else if (const auto* f = std::get_if<FrameOp>(&op.node)) {
            FrameOp next = *f;
            next.args = f->args->with_frames(std::move(rebuilt));
            t.parts.ops[idx] =
                std::make_shared<LazyOp>(LazyOp{std::move(next)});
        }
    }
    return std::move(t.parts);
}

// Leaves whose sources share a batch key (within one source type) read their
// data once. A group of top-level plans (`roots`) whose ops start with no
// projection streams through Source::open_batch() when the source supports it,
// so the caller must then drain those roots concurrently; any other group
// reads through Source::collect_batch(), each leaf then scanning its result
// from memory. Either way a leaf keeps its remaining ops above its source.
coro::CoroTask<void> batch_leaves(std::vector<PlannedTree*> leaves,
                                  const std::vector<PlannedTree*>& roots = {}) {
    struct Group {
        std::type_index type;
        std::string key;
        std::vector<PlannedTree*> members;
    };
    std::vector<Group> groups;
    for (PlannedTree* leaf : leaves) {
        std::optional<std::string> key = leaf->parts.source->batch_key();
        if (!key) continue;
        const Source& source = *leaf->parts.source;
        const std::type_index type(typeid(source));
        auto it = std::find_if(
            groups.begin(), groups.end(),
            [&](const Group& g) { return g.type == type && g.key == *key; });
        if (it == groups.end())
            groups.push_back({type, std::move(*key), {leaf}});
        else
            it->members.push_back(leaf);
    }
    auto streamable = [&](const PlannedTree* m) {
        return std::find(roots.begin(), roots.end(), m) != roots.end() &&
               (m->parts.ops.empty() ||
                !std::holds_alternative<SelectOp>(m->parts.ops.front()->node));
    };
    for (Group& g : groups) {
        if (g.members.size() < 2) continue;
        std::vector<std::shared_ptr<const Source>> sources;
        sources.reserve(g.members.size());
        for (PlannedTree* m : g.members) sources.push_back(m->parts.source);
        if (std::all_of(g.members.begin(), g.members.end(), streamable)) {
            const std::uint64_t budget =
                resolve_spill_budget(g.members.front()->memory_budget);
            if (auto cursors = sources.front()->open_batch(sources, budget)) {
                if (cursors->size() != g.members.size())
                    throw std::logic_error(
                        "Source::open_batch returned " +
                        std::to_string(cursors->size()) + " cursors for " +
                        std::to_string(g.members.size()) + " members");
                for (std::size_t k = 0; k < g.members.size(); ++k)
                    g.members[k]->parts.source = std::make_shared<OpenedSource>(
                        sources[k]->schema(), std::move((*cursors)[k]));
                continue;
            }
        }
        std::vector<DataFrame> frames =
            co_await sources.front()->collect_batch(sources);
        if (frames.size() != g.members.size())
            throw std::logic_error(
                "Source::collect_batch returned " +
                std::to_string(frames.size()) + " frames for " +
                std::to_string(g.members.size()) + " members");
        for (std::size_t k = 0; k < g.members.size(); ++k)
            g.members[k]->parts.source =
                std::make_shared<InMemorySource>(std::move(frames[k]));
    }
}

}  // namespace

namespace {

coro::CoroTask<DataFrame> collect_owned(LazyFrame plan, std::uint64_t budget,
                                        std::int64_t morsel_rows) {
    PlannedTree tree = plan_tree(plan);
    if (!tree.children.empty()) {
        std::vector<PlannedTree*> leaves;
        tree_leaves(tree, leaves);
        co_await batch_leaves(std::move(leaves));
    }
    co_return co_await collect_plan(finish_tree(std::move(tree)), budget,
                                    morsel_rows);
}

}  // namespace

coro::CoroTask<DataFrame> LazyFrame::collect(std::int64_t morsel_rows) const {
    return collect_owned(*this, memory_budget_, morsel_rows);
}

coro::CoroTask<std::vector<DataFrame>> Source::collect_batch(
    std::vector<std::shared_ptr<const Source>> members) const {
    (void)members;
    throw std::logic_error(
        "Source::collect_batch: a source with a batch_key() must override it");
    co_return {};
}

coro::CoroTask<std::vector<DataFrame>> collect_all(
    std::vector<LazyFrame> plans) {
    std::vector<PlannedTree> trees;
    trees.reserve(plans.size());
    for (const LazyFrame& lf : plans) trees.push_back(plan_tree(lf));
    std::vector<PlannedTree*> leaves;
    std::vector<PlannedTree*> roots;
    for (PlannedTree& t : trees) {
        roots.push_back(&t);
        tree_leaves(t, leaves);
    }
    co_await batch_leaves(std::move(leaves), roots);

    // Drained together: a streamed batch feeds every root from one read, so
    // the roots run at once and split each one's spill budget between them.
    std::vector<coro::CoroTask<DataFrame>> runs;
    runs.reserve(trees.size());
    for (PlannedTree& t : trees) {
        const std::uint64_t budget =
            share_spill_budget(t.memory_budget, trees.size());
        runs.push_back(collect_plan(finish_tree(std::move(t)), budget, 0));
    }
    co_return co_await coro::when_all(std::move(runs));
}

LoweredGroupAggs lower_group_aggs(const std::vector<GroupAgg>& aggs) {
    LoweredGroupAggs out;
    ankerl::unordered_dense::map<std::string, std::int32_t> dedup;
    auto resolve = [&](const std::string& name) -> std::int32_t {
        auto it = dedup.find(name);
        if (it != dedup.end()) return it->second;
        const std::int32_t idx =
            static_cast<std::int32_t>(out.value_names.size());
        out.value_names.push_back(name);
        dedup.emplace(name, idx);
        return idx;
    };
    out.specs.reserve(aggs.size());
    for (const GroupAgg& a : aggs) {
        AggSpec sp;
        sp.op = to_agg_op(a.op);
        sp.out = a.out;
        sp.param = a.param;
        sp.value_col = sp.op == AggOp::Count ? -1 : resolve(a.column);
        if (agg_uses_by_col(sp.op)) sp.by_col = resolve(a.by);
        out.specs.push_back(std::move(sp));
    }
    return out;
}

void agg_accumulate_chunk(AggState& state, const DataFrame& frame,
                          const std::vector<std::string>& keys,
                          const std::vector<std::string>& value_names,
                          const std::string& dyn_prefix) {
    std::vector<const Series*> kcols;
    kcols.reserve(keys.size());
    for (const std::string& k : keys) {
        const std::int64_t ki = frame.column_index(k);
        if (ki < 0) throw std::out_of_range("group_by: no column named " + k);
        kcols.push_back(&frame.columns[static_cast<std::size_t>(ki)]);
    }
    std::vector<const Series*> vcols;
    vcols.reserve(value_names.size());
    for (const std::string& v : value_names) {
        const std::int64_t vi = frame.column_index(v);
        if (vi < 0) throw std::out_of_range("group_by: no column named " + v);
        vcols.push_back(&frame.columns[static_cast<std::size_t>(vi)]);
    }
    // Pass the row count explicitly: an empty key list (a global reduce to one
    // group) carries no key column to infer the length from.
    if (dyn_prefix.empty()) {
        agg_accumulate(state, kcols, vcols, 0, frame.num_rows());
        return;
    }
    std::vector<AggDynInput> dcols;
    for (std::size_t i = 0; i < frame.names.size(); ++i)
        if (frame.names[i].rfind(dyn_prefix, 0) == 0)
            dcols.push_back(
                {frame.names[i].substr(dyn_prefix.size()), &frame.columns[i]});
    agg_accumulate(state, kcols, vcols, dcols, 0, frame.num_rows());
}

coro::CoroTask<AggStatePtr> LazyFrame::collect_group_state(
    std::vector<std::string> keys, std::vector<GroupAgg> aggs,
    std::vector<AggDynSpec> dyn, std::string dyn_prefix,
    std::int64_t morsel_rows) const {
    LoweredGroupAggs lowered = lower_group_aggs(aggs);
    AggStatePtr state = agg_new(lowered.specs, dyn);
    auto gen = stream(morsel_rows);
    while (auto df = co_await gen.next())
        agg_accumulate_chunk(*state, *df, keys, lowered.value_names,
                             dyn.empty() ? std::string() : dyn_prefix);
    co_return state;
}

LazyFrame DataFrame::lazy() const {
    DataFrame shared;
    shared.names = names;
    shared.columns.reserve(columns.size());
    for (const Series& c : columns) shared.columns.push_back(c.share());
    return LazyFrame::scan(std::make_shared<InMemorySource>(std::move(shared)));
}

LazyFrame lazy(DataFrame frame) {
    return LazyFrame::scan(std::make_shared<InMemorySource>(std::move(frame)));
}

namespace detail {

const char* plan_rule_name(PlanRule rule) noexcept {
    switch (rule) {
        case PlanRule::PredicatePushdown:
            return "predicate_pushdown";
        case PlanRule::ProjectionPushdown:
            return "projection_pushdown";
        case PlanRule::SourceAbsorption:
            return "source_absorption";
    }
    return "unknown";
}

namespace {

LazyFrame rebuild(const LazyFrame& lf, PlanParts plan) {
    return PlanAccess::make(std::move(plan.source), std::move(plan.ops),
                            PlanAccess::memory_budget(lf));
}

}  // namespace

LazyFrame apply_plan_rule(const LazyFrame& lf, PlanRule rule) {
    return rebuild(
        lf, run_plan_rule(rule, PlanAccess::source(lf).names(),
                          {PlanAccess::source_ptr(lf), PlanAccess::ops(lf)}));
}

LazyFrame optimize_plan(const LazyFrame& lf) {
    return rebuild(
        lf, optimize_parts(PlanAccess::source_ptr(lf), PlanAccess::ops(lf)));
}

namespace {

void fingerprint_group_aggs(Fingerprint& fp,
                            const std::vector<GroupAgg>& aggs) {
    fp.pod(static_cast<std::uint64_t>(aggs.size()));
    for (const GroupAgg& a : aggs) {
        fp.pod(a.op);
        fp.str(a.column);
        fp.str(a.out);
        fp.pod(a.param);
        fp.str(a.by);
    }
}

void fingerprint_op(Fingerprint& fp, const LazyOp& op) {
    fp.pod(static_cast<std::uint64_t>(op.node.index()));
    std::visit(
        overloaded{
            [&](const FilterOp& o) { fp.pod(expr_fingerprint(o.pred)); },
            [&](const SelectOp& o) { fp.strs(o.names); },
            [&](const WithColumnOp& o) {
                fp.str(o.name);
                fp.pod(expr_fingerprint(o.expr));
            },
            [&](const RenameOp& o) { fp.strs(o.names); },
            [&](const SliceOp& o) {
                fp.pod(o.offset);
                fp.pod(o.len);
            },
            [&](const TailOp& o) { fp.pod(o.n); },
            [&](const DropNullsOp&) {},
            [&](const FillNullOp& o) { fp.scalar(o.value); },
            [&](const WithRowIndexOp& o) { fp.str(o.name); },
            [&](const NullCountOp&) {},
            [&](const ExplodeOp& o) { fp.str(o.column); },
            [&](const UnpivotOp& o) {
                fp.strs(o.id_vars);
                fp.strs(o.value_vars);
            },
            [&](const TopkOp& o) {
                fp.str(o.name);
                fp.pod(o.k);
                fp.pod(o.largest);
            },
            [&](const GroupByOp& o) {
                fp.strs(o.keys);
                fingerprint_group_aggs(fp, o.aggs);
                fp.pod(static_cast<std::uint64_t>(o.dyn.size()));
                for (const AggDynSpec& d : o.dyn) {
                    fp.pod(d.op);
                    fp.pod(d.param);
                    fp.str(d.out_prefix);
                }
                fp.str(o.dyn_prefix);
            },
            [&](const SortByOp& o) {
                fp.str(o.name);
                fp.pod(o.descending);
            },
            [&](const UniqueOp& o) { fp.strs(o.subset); },
            [&](const SampleOp& o) {
                fp.pod(o.n);
                fp.pod(o.seed);
            },
            [&](const IsDupOp& o) { fp.pod(o.unique); },
            [&](const GroupByDynamicOp& o) {
                fp.str(o.time_col);
                fp.pod(o.every);
                fp.pod(o.period);
                fingerprint_group_aggs(fp, o.aggs);
                fp.pod(o.origin);
                fp.pod(o.origin_min);
            },
            [&](const PivotOp& o) {
                fp.str(o.index);
                fp.str(o.on);
                fp.str(o.values);
                fp.str(o.agg);
            },
            [&](const ToDummiesOp& o) { fp.str(o.column); },
            [&](const DescribeOp&) {},
            [&](const ReverseOp&) {},
            [&](const TakeOp& o) {
                fp.pod(static_cast<std::uint64_t>(o.indices.size()));
                fp.bytes(o.indices.data(),
                         o.indices.size() * sizeof(std::int64_t));
            },
            [&](const FilterMaskOp& o) {
                const dftu_series* m = o.mask.handle();
                fp.pod(m ? dftu_series_data(m) : nullptr);
                fp.pod(m ? o.mask.length() : std::int64_t{0});
            },
            [&](const SortByMultiOp& o) {
                fp.strs(o.by);
                fp.pod(static_cast<std::uint64_t>(o.descending.size()));
                for (bool d : o.descending) fp.pod(d);
            },
            [&](const JoinOp& o) {
                fp.pod(plan_fingerprint(o.other));
                fp.strs(o.left_on);
                fp.strs(o.right_on);
                fp.pod(o.how);
                fp.str(o.suffix);
                fp.pod(static_cast<std::uint64_t>(o.left_fields.size()));
                for (const Field& f : o.left_fields) {
                    fp.str(f.name);
                    fp.pod(f.type.id);
                }
            },
            [&](const ConcatOp& o) { fp.pod(plan_fingerprint(o.other)); },
            [&](const UnnestOp& o) {
                fp.str(o.column);
                fp.pod(o.keep_empty);
                fp.strs(o.out_names);
            },
            [&](const FrameOp& o) {
                fp.str(o.name);
                fp.pod(static_cast<const void*>(o.op));
                fp.pod(static_cast<const void*>(o.args.get()));
                fp.strs(o.out_names);
                if (o.args)
                    for (const LazyFrame& f : o.args->frames())
                        fp.pod(plan_fingerprint(f));
            },
            [&](const NodeOp& o) {
                fp.str(o.name);
                fp.pod(static_cast<const void*>(o.self));
                fp.pod(static_cast<const void*>(&op));
            }},
        op.node);
}

void visit_plan_at(const LazyFrame& lf,
                   const std::function<void(const LazyFrame&, int)>& f,
                   int depth) {
    f(lf, depth);
    for (const auto& op : PlanAccess::ops(lf)) {
        if (const auto* j = std::get_if<JoinOp>(&op->node)) {
            visit_plan_at(j->other, f, depth + 1);
        } else if (const auto* c = std::get_if<ConcatOp>(&op->node)) {
            visit_plan_at(c->other, f, depth + 1);
        } else if (const auto* fo = std::get_if<FrameOp>(&op->node)) {
            if (fo->args)
                for (const LazyFrame& child : fo->args->frames())
                    visit_plan_at(child, f, depth + 1);
        }
    }
}

}  // namespace

std::uint64_t plan_fingerprint(const LazyFrame& lf) {
    Fingerprint fp;
    fp.pod(static_cast<const void*>(&PlanAccess::source(lf)));
    fp.pod(PlanAccess::memory_budget(lf));
    const auto& ops = PlanAccess::ops(lf);
    fp.pod(static_cast<std::uint64_t>(ops.size()));
    for (const auto& op : ops) fingerprint_op(fp, *op);
    return fp.value();
}

void visit_plan(const LazyFrame& lf,
                const std::function<void(const LazyFrame&, int depth)>& f) {
    visit_plan_at(lf, f, 0);
}

const std::shared_ptr<const Source>& plan_source(const LazyFrame& lf) {
    return PlanAccess::source_ptr(lf);
}

LazyFrame rebase(const LazyFrame& lf, std::shared_ptr<const Source> source) {
    return PlanAccess::make(std::move(source), PlanAccess::ops(lf),
                            PlanAccess::memory_budget(lf));
}

std::optional<std::string> first_non_filter_op(const LazyFrame& lf) {
    for (const auto& op : PlanAccess::ops(lf))
        if (!std::holds_alternative<FilterOp>(op->node))
            return describe_op(*op);
    return std::nullopt;
}

std::optional<std::string> first_op(const LazyFrame& lf) {
    const auto& ops = PlanAccess::ops(lf);
    if (ops.empty()) return std::nullopt;
    return describe_op(*ops.front());
}

std::optional<std::pair<std::int64_t, std::int64_t>> sole_slice(
    const LazyFrame& lf) {
    const auto& ops = PlanAccess::ops(lf);
    if (ops.size() != 1) return std::nullopt;
    const auto* s = std::get_if<SliceOp>(&ops.front()->node);
    if (!s || s->offset < 0 || s->len < 0) return std::nullopt;
    return std::make_pair(s->offset, s->len);
}

}  // namespace detail

}  // namespace dftracer::utils::dataframe
