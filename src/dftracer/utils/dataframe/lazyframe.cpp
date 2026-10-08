#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/hash/hash.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>  // compute_memory_budget
#include <dftracer/utils/core/coro/task_abi.h>                   // task_to_abi
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/agg.h>        // streaming group-by state
#include <dftracer/utils/dataframe/batch_ops.h>  // concat_columns, take, concat
#include <dftracer/utils/dataframe/field_stat.h>  // FieldStat (describe)
#include <dftracer/utils/dataframe/grace_join.h>  // GraceJoin (join cursor)
#include <dftracer/utils/dataframe/internal/cell_ops.h>  // row_key, cell_to_string
#include <dftracer/utils/dataframe/internal/column_data.h>  // dftu_series (typed null templates)
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>  // dataframe_handle_wrap/take
#include <dftracer/utils/dataframe/internal/decimal.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>  // expr_handle_wrap/unwrap
#include <dftracer/utils/dataframe/internal/fingerprint.h>
#include <dftracer/utils/dataframe/internal/float16.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dataframe/internal/native_transform.h>
#include <dftracer/utils/dataframe/internal/node_registry.h>  // find_node
#include <dftracer/utils/dataframe/internal/reclaim_registry.h>
#include <dftracer/utils/dataframe/internal/rest_column.h>
#include <dftracer/utils/dataframe/internal/schema_types.h>   // dftu_schema
#include <dftracer/utils/dataframe/internal/spill.h>  // external-merge spill
#include <dftracer/utils/dataframe/join.h>            // HashJoin (join cursor)
#include <dftracer/utils/dataframe/kernels/field_stat.h>  // field_stat_reduce (SIMD)
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/parallel.h>  // parallel_for (parallel sinks)
#include <dftracer/utils/dataframe/types.h>     // byte_width, buffer_bytes

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
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

bool has_rest(const std::vector<std::string>& sch) {
    return !sch.empty() && sch.back() == REST_COLUMN;
}

// The fields of a rest column as named columns, flat.
DataFrame rest_fields(const Series& rest) {
    const Series flat = rest.materialize();
    DataFrame out;
    for (std::int64_t f = 0; f < flat.num_children(); ++f) {
        out.names.push_back(flat.field_name(f));
        out.columns.push_back(flat.child(f));
    }
    return out;
}

// Replaces the rest column of `f` with its fields, after the other columns.
// A field with the name of another column is an error: the plan cannot tell
// which one the caller means.
// Moves the rest column of `f`, if any, after the others.
DataFrame rest_to_back(DataFrame f) {
    const int at = column_index_of(f.names, std::string(REST_COLUMN));
    if (at < 0 || static_cast<std::size_t>(at) + 1 == f.names.size()) return f;
    std::rotate(f.names.begin() + at, f.names.begin() + at + 1, f.names.end());
    std::rotate(f.columns.begin() + at, f.columns.begin() + at + 1,
                f.columns.end());
    return f;
}

void unpack_rest(DataFrame& f) {
    const int at = column_index_of(f.names, std::string(REST_COLUMN));
    if (at < 0) return;
    const Series rest = std::move(f.columns[static_cast<std::size_t>(at)]);
    f.names.erase(f.names.begin() + at);
    f.columns.erase(f.columns.begin() + at);
    DataFrame fields = rest_fields(rest);
    for (std::size_t i = 0; i < fields.names.size(); ++i) {
        if (column_index_of(f.names, fields.names[i]) >= 0)
            throw std::invalid_argument(
                "column '" + fields.names[i] +
                "' is both a plan column and a column the scan returned "
                "beyond the plan's schema; rename or drop one of them");
        f.names.push_back(std::move(fields.names[i]));
        f.columns.push_back(std::move(fields.columns[i]));
    }
}

// Build one standalone DataFrame from a morsel. Names come from the morsel's
// own schema (streaming, self-describing), else the cursor's data-dependent
// out_names(), else the static plan schema.
DataFrame frame_from_morsel(
    Morsel&& m, const std::vector<std::string>& static_names,
    const std::optional<std::vector<std::string>>& out_names) {
    DataFrame out;
    out.columns = std::move(m.columns);
    if (!m.name_ids().empty()) {
        out.names.reserve(m.name_ids().size());
        for (std::uint32_t id : m.name_ids())
            out.names.emplace_back(m.dyn->intern->resolve(id));
    } else if (out_names) {
        out.names = *out_names;
    } else {
        out.names = static_names;
    }
    unpack_rest(out);
    return out;
}

// Drain a chunk generator to a single DataFrame.
coro::CoroTask<void> column_task(const std::function<void(std::size_t)>* fn,
                                 std::size_t c) {
    (*fn)(c);
    co_return;
}

// Runs fn(c) for every column c, in parallel on the current executor if any.
coro::CoroTask<void> each_column(std::size_t n,
                                 std::function<void(std::size_t)> fn) {
    if (n < 2 || Executor::current() == nullptr) {
        for (std::size_t c = 0; c < n; ++c) fn(c);
        co_return;
    }
    co_await run_coro_scope([&fn, n](CoroScope& scope) -> coro::CoroTask<void> {
        std::vector<coro::SpawnFuture<void>> tasks;
        tasks.reserve(n);
        for (std::size_t c = 0; c < n; ++c)
            tasks.push_back(scope.spawn(
                [&fn, c](CoroScope&) { return column_task(&fn, c); }));
        for (auto& task : tasks) co_await task;
        co_return;
    });
}

// Once the parts held in memory pass `budget`, every later part is written to
// one unlinked spill file and held as mapped columns. The parts reach
// ConcatPlan as they arrived and it aligns them to the output schema, so
// a part that needs a type conversion or a null fill is copied back in memory
// for that column only.
coro::CoroTask<DataFrame> drain_stream(coro::AsyncGenerator<DataFrame> gen,
                                       std::uint64_t budget = NO_SPILL_BUDGET) {
    std::vector<DataFrame> parts;
    std::unique_ptr<spill::PartFile> file;
    std::uint64_t held = 0;
    std::unordered_set<const void*> seen;
    while (auto df = co_await gen.next()) {
        if (budget != NO_SPILL_BUDGET) {
            if (held > budget) {
                if (!file) file = std::make_unique<spill::PartFile>();
                df->columns = file->append(df->columns);
            } else {
                held += spill::new_buffer_bytes(df->columns, seen);
            }
        }
        parts.push_back(std::move(*df));
    }
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
    const ConcatPlan plan(ptrs,
                          uniform ? ConcatHow::Vertical : ConcatHow::Diagonal);
    DataFrame out;
    out.names = plan.names();
    out.columns.resize(out.names.size());
    co_await each_column(out.columns.size(), [&plan, &out](std::size_t c) {
        out.columns[c] = chunked_column(plan.chunks(c));
    });
    co_return out;
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

// Approximate in-memory byte size of a set of FLAT columns (spill trigger).
std::size_t morsel_bytes(const std::vector<Series>& cols) {
    std::size_t total = 0;
    for (const Series& c : cols) {
        const std::int64_t n = c.length();
        const TypeId t = c.type();
        if (c.encoding() == Encoding::Dictionary ||
            c.encoding() == Encoding::View) {
            total +=
                static_cast<std::size_t>(dftu_series_buffer_bytes(c.handle()));
        } else if (is_wide_offset_type(t)) {  // LargeString / LargeBinary
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
std::size_t unique_spill_hash(std::string_view key, int depth) {
    const std::size_t h = std::hash<std::string_view>{}(key);
    if (depth == 0) return h;
    return static_cast<std::size_t>(hash::splitmix64(
        static_cast<std::uint64_t>(h) ^
        (static_cast<std::uint64_t>(depth) * hash::GOLDEN_RATIO)));
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
        : in_(std::move(in)),
          mask_(mask.is_flat() ? std::move(mask) : mask.materialize()) {}

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

    // Drop mode: the last selected column is the rest column, and its fields
    // survive except the named ones.
    SelectCursor(std::unique_ptr<Cursor> in, std::vector<int> idx,
                 std::unordered_set<std::string> rest_drop)
        : in_(std::move(in)),
          idx_(std::move(idx)),
          rest_drop_(std::move(rest_drop)) {}

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
        if (rest_drop_) {
            DataFrame fields = rest_fields(out.columns.back());
            std::vector<std::string> names;
            std::vector<Series> kept;
            for (std::size_t i = 0; i < fields.names.size(); ++i) {
                if (rest_drop_->count(fields.names[i])) continue;
                names.push_back(std::move(fields.names[i]));
                kept.push_back(std::move(fields.columns[i]));
            }
            out.columns.back() = struct_of_length(
                std::move(names), std::move(kept), out.columns.back().length());
        }
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
    std::optional<std::unordered_set<std::string>> rest_drop_;
};

// Renames the fields of the rest column, the last one; the plan columns are
// renamed by the plan schema alone and pass through untouched.
class RenameRestCursor : public Cursor {
   public:
    RenameRestCursor(std::unique_ptr<Cursor> in,
                     std::unordered_map<std::string, std::string> map,
                     std::unordered_set<std::string> plan_out)
        : in_(std::move(in)),
          map_(std::move(map)),
          plan_out_(std::move(plan_out)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return std::nullopt;
        apply(*m);
        co_return std::move(m);
    }

    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        if (!in_->try_next(max_rows, out)) return false;
        if (out) apply(*out);
        return true;
    }

    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        co_return co_await in_->narrow(predicate);
    }

   private:
    void apply(Morsel& m) const {
        DataFrame fields = rest_fields(m.columns.back());
        std::unordered_set<std::string> seen;
        for (std::string& n : fields.names) {
            auto it = map_.find(n);
            if (it != map_.end()) n = it->second;
            if (plan_out_.count(n) || !seen.insert(n).second)
                throw std::invalid_argument(
                    "rename_columns: column name '" + n +
                    "' is used by two columns after the rename");
        }
        m.columns.back() =
            struct_of_length(std::move(fields.names), std::move(fields.columns),
                             m.columns.back().length());
    }

    std::unique_ptr<Cursor> in_;
    std::unordered_map<std::string, std::string> map_;
    std::unordered_set<std::string> plan_out_;
};

// Lays a self-describing source morsel (name_ids set) out as the plan schema,
// by name, so every positional op above sees column i as schema column i. A
// column the morsel lacks is null of its declared type. With `rest`, the
// columns the schema does not declare follow as one rest column, a Struct of
// them in batch order (no fields for a morsel without any); without it they
// are dropped. A morsel without name_ids is already aligned.
class AlignCursor : public Cursor {
   public:
    AlignCursor(std::unique_ptr<Cursor> in, std::vector<Field> fields,
                bool rest)
        : in_(std::move(in)), fields_(std::move(fields)), rest_(rest) {}

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

    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        co_return co_await in_->narrow(predicate);
    }

   private:
    Morsel apply(Morsel&& m) {
        if (m.name_ids().empty()) {
            if (rest_) m.columns.push_back(struct_of_length({}, {}, m.rows));
            return std::move(m);
        }
        Morsel out;
        out.rows = m.rows;
        out.batch_index = m.batch_index;
        out.ordering =
            m.ordering == Ordering::ByColumn ? Ordering::Unordered : m.ordering;
        const std::vector<std::uint32_t>& ids = m.name_ids();
        out.columns.reserve(fields_.size() + (rest_ ? 1 : 0));
        std::vector<char> used(ids.size(), 0);
        for (std::size_t f = 0; f < fields_.size(); ++f) {
            std::size_t at = ids.size();
            for (std::size_t c = 0; c < ids.size(); ++c)
                if (m.dyn->intern->resolve(ids[c]) == fields_[f].name) {
                    at = c;
                    break;
                }
            if (at < ids.size()) {
                if (m.ordering == Ordering::ByColumn &&
                    static_cast<std::int32_t>(at) == m.ordered_column) {
                    out.ordering = Ordering::ByColumn;
                    out.ordered_column = static_cast<std::int32_t>(f);
                    out.ordered_descending = m.ordered_descending;
                }
                used[at] = 1;
                out.columns.push_back(std::move(m.columns[at]));
                continue;
            }
            // A column of no known type and no values here: the concat of
            // the morsels gives it the type of those that have values.
            const TypeId t = fields_[f].type.id == TypeId::Unknown
                                 ? TypeId::Int64
                                 : fields_[f].type.id;
            out.columns.push_back(Series::nulls(t, m.rows));
        }
        if (rest_) {
            std::vector<std::string> names;
            std::vector<Series> extra;
            for (std::size_t c = 0; c < ids.size(); ++c) {
                if (used[c]) continue;
                names.emplace_back(m.dyn->intern->resolve(ids[c]));
                extra.push_back(m.columns[c].materialize());
            }
            out.columns.push_back(
                struct_of_length(std::move(names), std::move(extra), m.rows));
        }
        return out;
    }

    std::unique_ptr<Cursor> in_;
    std::vector<Field> fields_;
    bool rest_;
};

// Adds or replaces one column from an expr.
class WithColumnCursor : public Cursor {
   public:
    // With `rest`, the input's last column is the rest column and a new
    // column goes before it; `width` counts the columns before it.
    WithColumnCursor(std::unique_ptr<Cursor> in, Expr e, int replace,
                     std::size_t width, bool rest)
        : in_(std::move(in)),
          expr_(std::move(e)),
          replace_(replace),
          width_(width),
          rest_(rest) {}

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
        out.dyn = std::move(m.dyn);
        if (replace_ >= 0)
            out.columns[static_cast<std::size_t>(replace_)] = std::move(nc);
        else if (rest_)
            out.columns.insert(out.columns.end() - 1, std::move(nc));
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
    bool rest_;
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
        : in_(std::move(in)),
          offset_(offset),
          len_(len),
          end_(len > std::numeric_limits<std::int64_t>::max() - offset
                   ? std::numeric_limits<std::int64_t>::max()
                   : offset + len) {}

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
        const std::int64_t w_end = std::min(end_, seen_);
        if (w_end <= w_start) return std::nullopt;
        emitted_ += w_end - w_start;
        return slice_morsel(m, w_start - start, w_end - w_start);
    }

    std::unique_ptr<Cursor> in_;
    std::int64_t offset_, len_, end_;
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

// Fills nulls per morsel; with `rest`, in the fields of the last column too.
class FillNullCursor : public Cursor {
   public:
    FillNullCursor(std::unique_ptr<Cursor> in, dftu_scalar value, bool rest)
        : in_(std::move(in)), value_(value), rest_(rest) {}
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
        if (rest_) {
            const std::int64_t rows = out.columns.back().length();
            DataFrame fields =
                rest_fields(out.columns.back()).fill_null(value_);
            out.columns.back() = struct_of_length(
                std::move(fields.names), std::move(fields.columns), rows);
        }
        out.batch_index = bi;
        out.ordering = ordered_col_had_nulls ? Ordering::Unordered : ord;
        out.ordered_column = ordered_column;
        out.ordered_descending = ordered_descending;
        return out;
    }

    std::unique_ptr<Cursor> in_;
    dftu_scalar value_;
    bool rest_;
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

constexpr std::size_t MAX_ACCUMULATE_BATCH = 32;
constexpr std::size_t GROUP_SPILL_PARTS = 64;
constexpr int GROUP_SPILL_MAX_DEPTH = 3;

// How many morsels to accumulate at once into separate partial states: the
// partials together stay within a quarter of the budget. `partial_bytes` is the
// measured size of one (agg_approx_bytes leaves out the group index, so it is
// doubled); 0 means not measured yet.
std::size_t accumulate_batch(std::uint64_t budget,
                             std::uint64_t partial_bytes) {
    if (partial_bytes == 0) return 2;
    if (budget == NO_SPILL_BUDGET) return MAX_ACCUMULATE_BATCH;
    return std::clamp<std::size_t>(budget / 4 / (2 * partial_bytes), 1,
                                   MAX_ACCUMULATE_BATCH);
}

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
        if (it == s.end())
            throw DFTUtilsException::cat(ErrorCode::INVALID_ARGUMENT,
                                         "group_by: no column named ", n);
        return static_cast<int>(it - s.begin());
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

        // Dyn columns are the prefix-tagged plan columns, and the
        // prefix-tagged fields of the rest column.
        std::vector<std::pair<int, std::string>> sch_dyn;
        if (!dyn_specs_.empty() && !dyn_prefix_.empty())
            for (std::size_t i = 0; i < sch_.size(); ++i)
                if (sch_[i].rfind(dyn_prefix_, 0) == 0)
                    sch_dyn.emplace_back(static_cast<int>(i),
                                         sch_[i].substr(dyn_prefix_.size()));
        const bool rest_dyn =
            !dyn_specs_.empty() && !dyn_prefix_.empty() && has_rest(sch_);

        AggStatePtr state = agg_new(specs_, dyn_specs_);
        // Bounded parallel sink: pull a batch of morsels, accumulate each into
        // its own partial AggState in parallel (the mergeable agg IR), then
        // merge the partials into the running state. Memory stays bounded to
        // one batch; a serial-pull single morsel skips the fan-out.
        std::size_t cap = accumulate_batch(budget_, 0);
        std::vector<Morsel> batch;
        batch.reserve(MAX_ACCUMULATE_BATCH);
        bool eof = false;
        std::int64_t rows_seen = 0;
        std::vector<std::int64_t> row_base;
        while (!eof) {
            batch.clear();
            for (std::size_t b = 0; b < cap; ++b) {
                auto m = co_await in_->next(max_rows);
                if (!m) {
                    eof = true;
                    break;
                }
                batch.push_back(std::move(*m));
            }
            if (batch.empty()) break;
            row_base.clear();
            for (const Morsel& m : batch) {
                row_base.push_back(rows_seen);
                rows_seen += m.rows;
            }
            auto accumulate = [&](AggState& st, std::size_t j) {
                const Morsel& m = batch[j];
                agg_set_row_base(st, row_base[j]);
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
                const DataFrame rest =
                    rest_dyn ? rest_fields(m.columns.back()) : DataFrame{};
                dyn.reserve(sch_dyn.size() + rest.columns.size());
                for (const auto& [ci, name] : sch_dyn)
                    dyn.push_back(
                        {name, &m.columns[static_cast<std::size_t>(ci)]});
                for (std::size_t i = 0; i < rest.columns.size(); ++i)
                    if (rest.names[i].rfind(dyn_prefix_, 0) == 0)
                        dyn.push_back({rest.names[i].substr(dyn_prefix_.size()),
                                       &rest.columns[i]});
                agg_accumulate(st, keys, values, dyn);
            };
            if (batch.size() == 1) {
                accumulate(*state, 0);
            } else {
                std::vector<AggStatePtr> partials(batch.size());
                parallel_for(static_cast<std::int64_t>(batch.size()), 1,
                             [&](std::int64_t bi, std::int64_t ei) {
                                 for (std::int64_t j = bi; j < ei; ++j) {
                                     auto st = agg_new(specs_, dyn_specs_);
                                     accumulate(*st,
                                                static_cast<std::size_t>(j));
                                     partials[static_cast<std::size_t>(j)] =
                                         std::move(st);
                                 }
                             });
                std::uint64_t partial_bytes = 0;
                for (auto& p : partials)
                    if (p) partial_bytes += agg_approx_bytes(*p);
                cap =
                    accumulate_batch(budget_, partial_bytes / partials.size());
                for (auto& p : partials)
                    if (p) agg_merge(*state, *p);
            }
            if (budget_ > 0 && agg_approx_bytes(*state) > budget_ / 2) {
                spill_state(*state);
                state = agg_new(specs_, dyn_specs_);
            }
        }

        if (!spilled_) {
            DataFrame r = agg_finalize(*state, keys_);
            out_names_ = r.names;
            result_ = to_morsel(r);
        } else {
            if (agg_num_groups(*state) > 0) spill_state(*state);
            for (std::size_t p = 0; p < part_out_.size(); ++p) {
                part_out_[p].close();
                if (!part_out_[p])
                    throw DFTUtilsException::cat(
                        ErrorCode::IO,
                        "group_by spill: cannot write a part file");
                if (part_bytes_[p])
                    todo_.push_back({part_paths_[p], part_bytes_[p], 0});
            }
        }
        built_ = true;
    }

    // A spilled state is cut into hash parts, each appended to its part file as
    // one blob, so equal keys from every flush end up in the same part and a
    // part can later be merged in memory on its own.
    void spill_state(const AggState& state) {
        if (!spilled_) {
            spilled_ = true;
            for (std::size_t p = 0; p < GROUP_SPILL_PARTS; ++p) {
                part_paths_.push_back(dir_.run_path(dir_.next_run()));
                part_out_.emplace_back(part_paths_.back(), std::ios::binary);
                part_bytes_.push_back(0);
            }
        }
        write_split(state, part_out_, part_bytes_, 0);
    }

    void write_split(const AggState& state, std::vector<std::ofstream>& out,
                     std::vector<std::uint64_t>& bytes, int salt) const {
        const auto split = agg_split_groups(state, out.size(), salt);
        for (std::size_t p = 0; p < split.size(); ++p) {
            if (split[p].empty()) continue;
            const std::string blob =
                agg_serialize(*agg_extract_groups(state, split[p]), true);
            const auto len = static_cast<std::uint32_t>(blob.size());
            out[p].write(reinterpret_cast<const char*>(&len), sizeof len);
            out[p].write(blob.data(),
                         static_cast<std::streamsize>(blob.size()));
            if (!out[p])
                throw DFTUtilsException::cat(
                    ErrorCode::IO, "group_by spill: cannot write a part file");
            bytes[p] += sizeof len + blob.size();
        }
    }

    // Reads blobs in batches of about `limit` bytes, deserializes each batch
    // in parallel and calls `fn` on it until it returns false.
    template <class F>
    static void read_batches(const std::string& path, std::uint64_t limit,
                             F&& fn) {
        std::ifstream is(path, std::ios::binary);
        if (!is)
            throw DFTUtilsException::cat(
                ErrorCode::IO, "group_by spill: cannot open part file ", path);
        std::vector<std::string> raw;
        std::uint64_t have = 0;
        auto flush = [&] {
            if (raw.empty()) return true;
            std::vector<AggStatePtr> states(raw.size());
            parallel_for(
                static_cast<std::int64_t>(raw.size()), 1,
                [&](std::int64_t b, std::int64_t e) {
                    for (std::int64_t j = b; j < e; ++j)
                        states[static_cast<std::size_t>(j)] = agg_deserialize(
                            raw[static_cast<std::size_t>(j)], true);
                });
            raw.clear();
            have = 0;
            return fn(states);
        };
        std::uint32_t len = 0;
        while (is.read(reinterpret_cast<char*>(&len), sizeof len)) {
            std::string blob(len, '\0');
            if (!is.read(blob.data(), static_cast<std::streamsize>(len)))
                throw DFTUtilsException::cat(
                    ErrorCode::IO, "group_by spill: truncated part file");
            have += len;
            raw.push_back(std::move(blob));
            if (have >= limit && !flush()) return;
        }
        flush();
    }

    AggStatePtr fold(AggStatePtr acc, std::vector<AggStatePtr>& batch) const {
        for (const AggStatePtr& st : batch) agg_merge(*acc, *st);
        return acc;
    }

    std::uint64_t batch_bytes() const {
        return std::max<std::uint64_t>(budget_ / 8, 1 << 16);
    }

    struct Part {
        std::string path;
        std::uint64_t bytes;
        int depth;
    };

    // A part whose merged state does not fit the budget is split again by a
    // salted hash, so the same keys spread over new parts. Blobs are folded
    // into one state first, so the pieces written are as large as a flush.
    void split_part(const Part& part) {
        std::vector<std::string> paths;
        std::vector<std::ofstream> out;
        std::vector<std::uint64_t> bytes(GROUP_SPILL_PARTS, 0);
        for (std::size_t p = 0; p < GROUP_SPILL_PARTS; ++p) {
            paths.push_back(dir_.run_path(dir_.next_run()));
            out.emplace_back(paths.back(), std::ios::binary);
        }
        AggStatePtr acc = agg_new(specs_, dyn_specs_);
        read_batches(part.path, batch_bytes(),
                     [&](std::vector<AggStatePtr>& batch) {
                         acc = fold(std::move(acc), batch);
                         if (agg_approx_bytes(*acc) > budget_ / 2) {
                             write_split(*acc, out, bytes, part.depth + 1);
                             acc = agg_new(specs_, dyn_specs_);
                         }
                         return true;
                     });
        if (agg_num_groups(*acc) > 0)
            write_split(*acc, out, bytes, part.depth + 1);
        for (std::size_t p = 0; p < out.size(); ++p) {
            out[p].close();
            if (!out[p])
                throw DFTUtilsException::cat(
                    ErrorCode::IO, "group_by spill: cannot write a part file");
            if (bytes[p]) todo_.push_back({paths[p], bytes[p], part.depth + 1});
        }
        std::error_code ec;
        fs::remove(part.path, ec);
    }

    // Merges one part of the spill at a time and emits its groups. With dyn
    // columns the column set is the union over every group, so all parts merge
    // into one state that is finalized once.
    std::optional<Morsel> merge_next(std::int64_t max_rows) {
        if (!dyn_specs_.empty()) {
            if (dyn_drained_) return std::nullopt;
            dyn_drained_ = true;
            AggStatePtr merged = agg_new(specs_, dyn_specs_);
            for (const Part& part : todo_)
                read_batches(part.path, batch_bytes(),
                             [&](std::vector<AggStatePtr>& batch) {
                                 merged = fold(std::move(merged), batch);
                                 return true;
                             });
            if (agg_num_groups(*merged) == 0) return std::nullopt;
            DataFrame r = agg_finalize(*merged, keys_);
            out_names_ = r.names;
            return to_morsel(r);
        }
        while (true) {
            if (emit_ && emit_at_ < emit_->num_rows()) {
                const std::int64_t len =
                    std::min(max_rows, emit_->num_rows() - emit_at_);
                Morsel out = to_morsel(emit_->slice(emit_at_, len));
                emit_at_ += len;
                return out;
            }
            emit_.reset();
            if (todo_.empty()) return std::nullopt;
            const Part part = std::move(todo_.back());
            todo_.pop_back();
            AggStatePtr merged = agg_new(specs_, dyn_specs_);
            bool too_big = false;
            read_batches(part.path, batch_bytes(),
                         [&](std::vector<AggStatePtr>& batch) {
                             merged = fold(std::move(merged), batch);
                             too_big = part.depth < GROUP_SPILL_MAX_DEPTH &&
                                       agg_approx_bytes(*merged) > budget_;
                             return !too_big;
                         });
            if (too_big) {
                merged.reset();
                split_part(part);
                continue;
            }
            if (agg_num_groups(*merged) == 0) continue;
            emit_ = agg_finalize(*merged, keys_);
            out_names_ = emit_->names;
            emit_at_ = 0;
        }
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
    std::vector<std::string> part_paths_;
    std::vector<std::ofstream> part_out_;
    std::vector<std::uint64_t> part_bytes_;
    std::vector<Part> todo_;
    std::optional<DataFrame> emit_;
    std::int64_t emit_at_ = 0;
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
        std::int64_t rows_seen = 0;
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
            agg_set_row_base(*state, rows_seen);
            rows_seen += static_cast<std::int64_t>(rowsv.size());
            agg_accumulate(*state, keyc, values);
        }
        DataFrame r = agg_finalize(*state, time_col_).sort_by(time_col_, false);
        co_return morsel_of(std::move(r));
    }

   private:
    static int index_in(const std::vector<std::string>& s,
                        const std::string& n) {
        auto it = std::find(s.begin(), s.end(), n);
        if (it == s.end())
            throw DFTUtilsException::cat(ErrorCode::INVALID_ARGUMENT,
                                         "group_by: no column named ", n);
        return static_cast<int>(it - s.begin());
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

// Appends cell `i` to a head_by key. Numbers encode by value, so equal numbers
// of different numeric types (1, 1u, 1.0, and 0.0 / -0.0) share one key and
// every NaN is one key; any other cell uses append_cell's exact bytes.
void append_value_key(std::string& key, const Series& c, std::int64_t i) {
    const TypeId t = c.type();
    if (!is_numeric(t) || c.is_null(i)) {
        append_cell(key, c, i);
        return;
    }
    auto put = [&](char tag, auto v) {
        key.push_back(tag);
        key.append(reinterpret_cast<const char*>(&v), sizeof(v));
    };
    if (t == TypeId::Float32 || t == TypeId::Float64) {
        const double d = read_f64(c, i);
        constexpr double TWO_63 = 9223372036854775808.0;
        if (std::isnan(d)) {
            key.push_back('n');
        } else if (d != std::trunc(d)) {
            put('f', d);
        } else if (d >= -TWO_63 && d < TWO_63) {
            put('i', static_cast<std::int64_t>(d));
        } else if (d >= 0 && d < 2 * TWO_63) {
            put('u', static_cast<std::uint64_t>(d));
        } else {
            put('f', d);
        }
        return;
    }
    if (t == TypeId::Uint64) {
        const std::uint64_t u = read_u64(c, i);
        if (u > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
            put('u', u);
            return;
        }
        put('i', static_cast<std::int64_t>(u));
        return;
    }
    put('i', static_cast<std::int64_t>(read_u64(c, i)));
}

constexpr std::size_t SURVIVOR_CHUNK = 512;

// The part of "keep the first `keep` rows of each key" that runs past the
// memory budget. The rows still to come are spooled in order; only their
// (row id, key) pairs are hash-partitioned to disk, so a partition holds ids in
// ascending order and the first `keep` it sees per key are the survivors, which
// come out already sorted. An oversized partition is split again with a
// salted hash. The spool is then replayed and only the surviving rows are
// emitted, so no row data is merged and every file is read in sequence.
class FirstRowsSpill {
   public:
    using Already = std::function<std::int64_t(const std::string&)>;

    FirstRowsSpill(std::uint64_t budget, std::int64_t keep,
                   std::int64_t first_id, Already already)
        : budget_(budget),
          keep_(keep),
          next_id_(first_id),
          replay_id_(first_id),
          already_(std::move(already)),
          spool_(budget / 4) {
        for (int p = 0; p < UNIQUE_SPILL_FANOUT; ++p) {
            paths_.push_back(dir_.run_path(dir_.next_run()));
            writers_.emplace_back(paths_.back());
        }
        bytes_.assign(UNIQUE_SPILL_FANOUT, 0);
        rows_.assign(UNIQUE_SPILL_FANOUT, 0);
    }

    void add(Morsel&& m, std::vector<std::string> keys) {
        const std::int64_t n = m.rows;
        std::vector<std::vector<std::int64_t>> ids(UNIQUE_SPILL_FANOUT);
        std::vector<std::vector<std::string>> ks(UNIQUE_SPILL_FANOUT);
        for (std::int64_t i = 0; i < n; ++i) {
            std::string& key = keys[static_cast<std::size_t>(i)];
            if (already_(key) >= keep_) continue;
            const std::size_t p =
                unique_spill_hash(key, 0) % UNIQUE_SPILL_FANOUT;
            ids[p].push_back(next_id_ + i);
            ks[p].push_back(std::move(key));
        }
        for (std::size_t p = 0; p < UNIQUE_SPILL_FANOUT; ++p) {
            if (ids[p].empty()) continue;
            const auto count = static_cast<std::int64_t>(ids[p].size());
            std::vector<Series> cols;
            cols.push_back(Series::flat_i64(ids[p].data(), count));
            cols.push_back(Series::strings(ks[p]));
            bytes_[p] += spill::columns_bytes(cols);
            rows_[p] += count;
            writers_[p].write(cols, count);
        }
        next_id_ += n;
        spool_.add(std::move(m.columns), n);
    }

    coro::CoroTask<void> finish() {
        for (spill::Writer& w : writers_) w.close();
        for (int p = 0; p < UNIQUE_SPILL_FANOUT; ++p)
            co_await settle(paths_[static_cast<std::size_t>(p)],
                            bytes_[static_cast<std::size_t>(p)],
                            rows_[static_cast<std::size_t>(p)], 0);
        for (const std::string& path : survivor_paths_)
            runs_.emplace_back(std::make_unique<spill::Reader>(path));
        replay_ = spool_.reader();
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) {
        if (runs_.empty()) co_return std::nullopt;
        while (auto m = co_await replay_->next(max_rows)) {
            const std::int64_t lo = replay_id_;
            const std::int64_t hi = lo + m->rows;
            replay_id_ = hi;
            std::vector<std::int64_t> keep;
            for (Run& r : runs_) {
                while (!r.done) {
                    if (!r.cur || r.pos >= r.cur->rows) {
                        r.cur = co_await r.reader->next(SURVIVOR_CHUNK);
                        r.pos = 0;
                        if (!r.cur) {
                            r.done = true;
                            break;
                        }
                    }
                    const std::int64_t id =
                        r.cur->columns[0].data<std::int64_t>()[r.pos];
                    if (id >= hi) break;
                    keep.push_back(id - lo);
                    ++r.pos;
                }
            }
            if (keep.empty()) continue;
            std::sort(keep.begin(), keep.end());
            DataFrame mf;
            mf.names.assign(m->columns.size(), std::string());
            mf.columns = std::move(m->columns);
            co_return morsel_of(take(mf, keep));
        }
        co_return std::nullopt;
    }

   private:
    struct Run {
        explicit Run(std::unique_ptr<spill::Reader> r) : reader(std::move(r)) {}
        std::unique_ptr<spill::Reader> reader;
        std::optional<Morsel> cur;
        std::int64_t pos = 0;
        bool done = false;
    };

    // A partition that fits a fraction of the budget is counted in memory; a
    // larger one is split again (until the depth limit or too few rows).
    coro::CoroTask<void> settle(const std::string& path, std::size_t bytes,
                                std::int64_t rows, int depth) {
        if (depth < UNIQUE_SPILL_MAX_DEPTH && bytes > budget_ / 8 &&
            rows > UNIQUE_SPILL_MIN_LEAF_ROWS) {
            std::vector<std::string> sub_paths;
            std::vector<spill::Writer> writers;
            for (int p = 0; p < UNIQUE_SPILL_FANOUT; ++p) {
                sub_paths.push_back(dir_.run_path(dir_.next_run()));
                writers.emplace_back(sub_paths.back());
            }
            std::vector<std::size_t> sub_bytes(UNIQUE_SPILL_FANOUT, 0);
            std::vector<std::int64_t> sub_rows(UNIQUE_SPILL_FANOUT, 0);
            spill::Reader reader(path);
            while (auto m = co_await reader.next(DEFAULT_MORSEL_ROWS)) {
                std::vector<std::vector<std::int64_t>> pick(
                    UNIQUE_SPILL_FANOUT);
                for (std::int64_t i = 0; i < m->rows; ++i)
                    pick[unique_spill_hash(m->columns[1].string_at(i),
                                           depth + 1) %
                         UNIQUE_SPILL_FANOUT]
                        .push_back(i);
                DataFrame mf;
                mf.names.assign(m->columns.size(), std::string());
                mf.columns = std::move(m->columns);
                for (std::size_t p = 0; p < UNIQUE_SPILL_FANOUT; ++p) {
                    if (pick[p].empty()) continue;
                    DataFrame part = take(mf, pick[p]);
                    sub_bytes[p] += morsel_bytes(part.columns);
                    sub_rows[p] += part.num_rows();
                    writers[p].write(part.columns, part.num_rows());
                }
            }
            for (spill::Writer& w : writers) w.close();
            for (std::size_t p = 0; p < UNIQUE_SPILL_FANOUT; ++p)
                co_await settle(sub_paths[p], sub_bytes[p], sub_rows[p],
                                depth + 1);
            co_return;
        }
        StringViewMap<std::int64_t> emitted;
        const std::string out_path = dir_.run_path(dir_.next_run());
        spill::Writer out(out_path);
        std::vector<std::int64_t> ids;
        ids.reserve(SURVIVOR_CHUNK);
        bool any = false;
        auto flush = [&] {
            if (ids.empty()) return;
            const auto n = static_cast<std::int64_t>(ids.size());
            std::vector<Series> cols;
            cols.push_back(Series::flat_i64(ids.data(), n));
            out.write(cols, n);
            ids.clear();
            any = true;
        };
        spill::Reader reader(path);
        while (auto m = co_await reader.next(DEFAULT_MORSEL_ROWS)) {
            const std::int64_t* row_ids = m->columns[0].data<std::int64_t>();
            for (std::int64_t i = 0; i < m->rows; ++i) {
                const std::string_view key = m->columns[1].string_at(i);
                auto it = emitted.find(key);
                if (it == emitted.end())
                    it = emitted
                             .emplace(std::string(key),
                                      already_(std::string(key)))
                             .first;
                if (it->second >= keep_) continue;
                ++it->second;
                ids.push_back(row_ids[i]);
                if (ids.size() == SURVIVOR_CHUNK) flush();
            }
        }
        flush();
        out.close();
        if (any) survivor_paths_.push_back(out_path);
    }

    std::uint64_t budget_;
    std::int64_t keep_;
    std::int64_t next_id_;
    std::int64_t replay_id_;
    Already already_;
    spill::Spool spool_;
    spill::Dir dir_;
    std::vector<std::string> paths_;
    std::vector<spill::Writer> writers_;
    std::vector<std::size_t> bytes_;
    std::vector<std::int64_t> rows_;
    std::vector<std::string> survivor_paths_;
    std::vector<Run> runs_;
    std::unique_ptr<Cursor> replay_;
};

// Keeps the first n rows of each distinct key tuple, in input order. Streams:
// holds one counter per distinct key. A null key is its own key.
class HeadByCursor : public Cursor {
   public:
    HeadByCursor(std::unique_ptr<Cursor> in, std::vector<std::int64_t> key_idx,
                 std::int64_t n, std::uint64_t budget)
        : in_(std::move(in)),
          key_idx_(std::move(key_idx)),
          n_(n),
          budget_(budget) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (n_ <= 0) co_return std::nullopt;
        if (!spilling_) {
            while (auto m = co_await in_->next(max_rows)) {
                const std::int64_t rows = m->rows;
                std::vector<std::string> keys = keys_of(*m);
                std::vector<std::int64_t> keep;
                keep.reserve(static_cast<std::size_t>(rows));
                for (std::int64_t i = 0; i < rows; ++i) {
                    std::string& key = keys[static_cast<std::size_t>(i)];
                    const std::size_t key_bytes =
                        key.size() + DEDUP_ENTRY_OVERHEAD;
                    auto [it, fresh] = counts_.try_emplace(std::move(key), 0);
                    if (fresh) fast_bytes_ += key_bytes;
                    if (it->second < n_) {
                        ++it->second;
                        keep.push_back(i);
                    }
                }
                next_row_id_ += rows;
                if (budget_ > 0 && fast_bytes_ > budget_ / 2) spilling_ = true;
                if (keep.empty()) {
                    if (spilling_) break;
                    continue;
                }
                if (static_cast<std::int64_t>(keep.size()) == rows) co_return m;
                DataFrame mf;
                mf.names.assign(m->columns.size(), std::string());
                mf.columns = std::move(m->columns);
                Morsel out = morsel_of(take(mf, keep));
                out.ordering = m->ordering;
                out.ordered_column = m->ordered_column;
                out.ordered_descending = m->ordered_descending;
                co_return out;
            }
            if (!spilling_) co_return std::nullopt;
        }
        if (!external_) {
            external_ = std::make_unique<FirstRowsSpill>(
                budget_, n_, next_row_id_, [this](const std::string& key) {
                    const auto it = counts_.find(key);
                    return it == counts_.end() ? std::int64_t{0} : it->second;
                });
            while (auto m = co_await in_->next(max_rows)) {
                std::vector<std::string> keys = keys_of(*m);
                external_->add(std::move(*m), std::move(keys));
            }
            co_await external_->finish();
        }
        co_return co_await external_->next(max_rows);
    }

   private:
    std::vector<std::string> keys_of(const Morsel& m) const {
        std::vector<std::string> keys(static_cast<std::size_t>(m.rows));
        parallel_for(
            m.rows, std::int64_t{1} << 13, [&](std::int64_t b, std::int64_t e) {
                for (std::int64_t i = b; i < e; ++i) {
                    std::string& k = keys[static_cast<std::size_t>(i)];
                    for (std::int64_t c : key_idx_)
                        append_value_key(
                            k, m.columns[static_cast<std::size_t>(c)], i);
                }
            });
        return keys;
    }

    std::unique_ptr<Cursor> in_;
    std::vector<std::int64_t> key_idx_;
    std::int64_t n_;
    std::uint64_t budget_;
    ankerl::unordered_dense::map<std::string, std::int64_t> counts_;
    std::size_t fast_bytes_ = 0;
    std::int64_t next_row_id_ = 0;
    bool spilling_ = false;
    std::unique_ptr<FirstRowsSpill> external_;
};

// The key cells of one run morsel read once into typed arrays, so a merge
// compares plain values instead of calling the column ABI for every cell.
// Integers compare exactly (an unsigned 64-bit value has its sign bit flipped
// to keep its order as a signed one), floats as doubles, strings as views into
// the morsel; nulls sort last in both directions, as in cmp_cell.
struct KeyData {
    enum class Kind { INT, FLOAT, BYTES, DEC128, DEC256 };
    Kind kind = Kind::INT;
    std::vector<std::int64_t> ints;
    std::vector<double> floats;
    std::vector<std::string_view> bytes;  // strings, binary and decimal cells
    std::vector<std::uint8_t> nulls;

    // The key is read by its physical type, so a Date, Timestamp, Time or
    // Duration orders as the integer it is stored as.
    explicit KeyData(const Series& c) {
        const std::int64_t n = c.length();
        const auto size = static_cast<std::size_t>(n);
        if (c.null_count() > 0) {
            nulls.resize(size);
            for (std::int64_t i = 0; i < n; ++i)
                nulls[static_cast<std::size_t>(i)] = c.is_null(i) ? 1 : 0;
        }
        const TypeId t = narrow_varwidth_type(physical_type(c.type()));
        switch (t) {
            case TypeId::String:
            case TypeId::Binary:
            case TypeId::FixedSizeBinary:
                kind = Kind::BYTES;
                bytes.resize(size);
                for (std::int64_t i = 0; i < n; ++i)
                    if (!null_at(i))
                        bytes[static_cast<std::size_t>(i)] = read_bytes(c, i);
                break;
            case TypeId::Decimal128:
            case TypeId::Decimal256: {
                const std::size_t width = t == TypeId::Decimal128 ? 16 : 32;
                kind = t == TypeId::Decimal128 ? Kind::DEC128 : Kind::DEC256;
                bytes.resize(size);
                const char* base =
                    reinterpret_cast<const char*>(c.data<std::uint8_t>());
                for (std::size_t i = 0; i < size; ++i)
                    bytes[i] = std::string_view(base + i * width, width);
                break;
            }
            case TypeId::Float16:
                kind = Kind::FLOAT;
                floats.resize(size);
                for (std::int64_t i = 0; i < n; ++i)
                    floats[static_cast<std::size_t>(i)] = static_cast<double>(
                        half_to_float(c.data<std::uint16_t>()[i]));
                break;
            case TypeId::Float32:
                kind = Kind::FLOAT;
                floats.resize(size);
                for (std::int64_t i = 0; i < n; ++i)
                    floats[static_cast<std::size_t>(i)] =
                        static_cast<double>(c.data<float>()[i]);
                break;
            case TypeId::Float64:
                kind = Kind::FLOAT;
                floats.resize(size);
                for (std::int64_t i = 0; i < n; ++i)
                    floats[static_cast<std::size_t>(i)] = c.data<double>()[i];
                break;
            default:
                kind = Kind::INT;
                ints.resize(size);
                for (std::int64_t i = 0; i < n; ++i)
                    ints[static_cast<std::size_t>(i)] = int_cell(c, t, i);
        }
    }

    bool null_at(std::int64_t i) const {
        return !nulls.empty() && nulls[static_cast<std::size_t>(i)];
    }

    // Negative when row `i` sorts before row `j` of `o`, ascending. Floats
    // order as the sort kernels do: every NaN equal and above infinity.
    int compare(std::int64_t i, const KeyData& o, std::int64_t j) const {
        const auto a = static_cast<std::size_t>(i);
        const auto b = static_cast<std::size_t>(j);
        switch (kind) {
            case Kind::INT:
                return ints[a] < o.ints[b] ? -1 : (ints[a] > o.ints[b] ? 1 : 0);
            case Kind::BYTES:
                return bytes[a] < o.bytes[b] ? -1
                                             : (bytes[a] > o.bytes[b] ? 1 : 0);
            case Kind::DEC128:
                return compare_decimal128(bytes[a].data(), o.bytes[b].data());
            case Kind::DEC256:
                return compare_decimal256(bytes[a].data(), o.bytes[b].data());
            case Kind::FLOAT:
                break;
        }
        const double x = floats[a];
        const double y = o.floats[b];
        const bool nx = x != x;
        const bool ny = y != y;
        if (nx || ny) return nx == ny ? 0 : (nx ? 1 : -1);
        return x < y ? -1 : (x > y ? 1 : 0);
    }

    static bool is_integer_key(TypeId t) {
        return t == TypeId::Bool || t == TypeId::Int8 || t == TypeId::Int16 ||
               t == TypeId::Int32 || t == TypeId::Int64 || t == TypeId::Uint8 ||
               t == TypeId::Uint16 || t == TypeId::Uint32 ||
               t == TypeId::Uint64;
    }

    static std::int64_t int_cell(const Series& c, TypeId t, std::int64_t i) {
        switch (t) {
            case TypeId::Bool:
                return (c.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
            case TypeId::Int8:
                return c.data<std::int8_t>()[i];
            case TypeId::Int16:
                return c.data<std::int16_t>()[i];
            case TypeId::Int32:
                return c.data<std::int32_t>()[i];
            case TypeId::Int64:
                return c.data<std::int64_t>()[i];
            case TypeId::Uint8:
                return c.data<std::uint8_t>()[i];
            case TypeId::Uint16:
                return c.data<std::uint16_t>()[i];
            case TypeId::Uint32:
                return c.data<std::uint32_t>()[i];
            case TypeId::Uint64:
                return static_cast<std::int64_t>(c.data<std::uint64_t>()[i] ^
                                                 (std::uint64_t{1} << 63));
            default:
                throw std::invalid_argument(
                    "sort: a key of this type has no order");
        }
    }
};

// External merge sort by one or several keys (lexicographic, each ascending or
// descending). Generates sorted runs bounded by `budget` bytes (spilled to
// disk; budget 0 keeps one in-memory run), then k-way range-merges them into a
// sorted stream. Peak memory is O(budget + one output morsel) when spilling.
// The merge prefers the earlier run on a tie, so the sort is stable.
class SortMergeCursor : public Cursor {
   public:
    // Sorting a run holds its concatenated rows, its sorted copy and the sort
    // permutation at once, and the allocator does not hand the freed morsels
    // back to the sort (measured: one run peaks at about 3.5 times its data),
    // so a run is cut at this fraction of the budget.
    static constexpr std::uint64_t RUN_COPIES = 4;
    static constexpr std::uint64_t RUN_MORSEL_BYTES = 128 * 1024;

    SortMergeCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                    std::vector<std::string> keys, std::vector<bool> descending,
                    std::uint64_t budget)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          keys_(std::move(keys)),
          descending_(std::move(descending)),
          budget_(budget) {}

    // Sorting commutes with a row-wise narrowing, and the input is untouched
    // until the first next(), which is when a join sends one.
    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        if (built_) co_return false;
        co_return co_await in_->narrow(predicate);
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!built_) co_await build(max_rows);

        // A heap over the run heads picks the next row; the rows taken from one
        // run morsel are gathered once, and one more gather puts the pieces in
        // order, so the cost per row is a few comparisons, not a morsel.
        const auto after = [&](int a, int b) {
            const auto ia = static_cast<std::size_t>(a);
            const auto ib = static_cast<std::size_t>(b);
            const int c = cmp_rows(ia, pos_[ia], ib, pos_[ib]);
            return c > 0 || (c == 0 && a > b);
        };
        std::priority_queue<int, std::vector<int>, decltype(after)> heap(after);
        for (std::size_t k = 0; k < cur_.size(); ++k)
            if (cur_[k] && pos_[k] < cur_[k]->rows)
                heap.push(static_cast<int>(k));

        std::vector<std::vector<Series>> segments;
        std::vector<std::int64_t> segment_rows;
        std::vector<std::int32_t> out_segment;
        std::vector<std::int64_t> out_at;
        std::vector<std::int64_t> open_rows;
        std::vector<int> open_segment(cur_.size(), -1);
        std::vector<std::vector<std::int64_t>> picked(cur_.size());
        std::int64_t out_rows = 0;

        const auto close_segment = [&](std::size_t k) {
            std::vector<Series> cols;
            cols.reserve(cur_[k]->columns.size());
            for (const Series& c : cur_[k]->columns)
                cols.push_back(c.take(picked[k]));
            segments[static_cast<std::size_t>(open_segment[k])] =
                std::move(cols);
            segment_rows[static_cast<std::size_t>(open_segment[k])] =
                static_cast<std::int64_t>(picked[k].size());
            picked[k].clear();
            open_segment[k] = -1;
        };

        while (out_rows < max_rows && !heap.empty()) {
            const int w = heap.top();
            heap.pop();
            const auto k = static_cast<std::size_t>(w);
            if (open_segment[k] < 0) {
                open_segment[k] = static_cast<int>(segments.size());
                segments.emplace_back();
                segment_rows.push_back(0);
            }
            // The winner keeps going while its rows still sort before the
            // best head of the other runs (ties go to the earlier run).
            const std::int64_t stop =
                std::min(cur_[k]->rows, pos_[k] + (max_rows - out_rows));
            std::int64_t take_n = 1;
            if (heap.empty()) {
                take_n = stop - pos_[k];
            } else {
                const auto t = static_cast<std::size_t>(heap.top());
                const int allowed = w < heap.top() ? 1 : 0;
                while (pos_[k] + take_n < stop &&
                       cmp_rows(k, pos_[k] + take_n, t, pos_[t]) < allowed)
                    ++take_n;
            }
            for (std::int64_t i = 0; i < take_n; ++i) {
                out_segment.push_back(open_segment[k]);
                out_at.push_back(static_cast<std::int64_t>(picked[k].size()));
                picked[k].push_back(pos_[k] + i);
            }
            pos_[k] += take_n;
            out_rows += take_n;
            if (pos_[k] >= cur_[k]->rows) {
                close_segment(k);
                co_await advance(w, max_rows);
                if (cur_[k] && cur_[k]->rows > 0) heap.push(w);
            } else {
                heap.push(w);
            }
        }
        for (std::size_t k = 0; k < cur_.size(); ++k)
            if (open_segment[k] >= 0) close_segment(k);
        if (out_rows == 0) co_return std::nullopt;

        Morsel out;
        out.rows = out_rows;
        const std::size_t ncols = segments.front().size();
        if (segments.size() == 1) {
            out.columns = std::move(segments.front());
        } else {
            std::vector<std::int64_t> offset(segments.size(), 0);
            for (std::size_t sg = 1; sg < segments.size(); ++sg)
                offset[sg] = offset[sg - 1] + segment_rows[sg - 1];
            std::vector<std::int64_t> order(static_cast<std::size_t>(out_rows));
            for (std::size_t j = 0; j < order.size(); ++j)
                order[j] = offset[static_cast<std::size_t>(out_segment[j])] +
                           out_at[j];
            out.columns.reserve(ncols);
            for (std::size_t c = 0; c < ncols; ++c) {
                std::vector<const Series*> parts;
                parts.reserve(segments.size());
                for (auto& seg : segments) parts.push_back(&seg[c]);
                out.columns.push_back(concat_columns(parts).take(order));
            }
        }
        // The merge always takes the globally smallest key across the whole
        // call sequence (pos_/cur_ persist between next() calls), so the
        // concatenation of every morsel this cursor returns is sorted. The
        // leading key is the one ordering claim a morsel carries.
        out.ordering = Ordering::ByColumn;
        out.ordered_column = key_idx_.front();
        out.ordered_descending = descending_.front();
        co_return out;
    }

   private:
    // Lexicographic over the keys: negative when the head row `ia` of run `a`
    // sorts before row `ib` of run `b`. A null sorts last in both directions.
    int cmp_rows(std::size_t a, std::int64_t ia, std::size_t b,
                 std::int64_t ib) const {
        for (std::size_t i = 0; i < key_idx_.size(); ++i) {
            const KeyData& x = run_keys_[a][i];
            const KeyData& y = run_keys_[b][i];
            const bool nx = x.null_at(ia);
            const bool ny = y.null_at(ib);
            if (nx || ny) {
                if (nx && ny) continue;
                return nx ? 1 : -1;
            }
            const int r =
                descending_[i] ? -x.compare(ia, y, ib) : x.compare(ia, y, ib);
            if (r != 0) return r;
        }
        return 0;
    }

    DataFrame sort_rows(DataFrame df) const {
        return keys_.size() == 1
                   ? df.sort_by(keys_.front(), descending_.front())
                   : df.sort_by_multi(keys_, descending_);
    }

    void load(std::size_t k, std::optional<Morsel> m) {
        cur_[k] = std::move(m);
        pos_[k] = 0;
        run_keys_[k].clear();
        if (!cur_[k]) return;
        for (int idx : key_idx_)
            run_keys_[k].emplace_back(
                cur_[k]->columns[static_cast<std::size_t>(idx)]);
    }

    coro::CoroTask<void> advance(int k, std::int64_t max_rows) {
        const auto i = static_cast<std::size_t>(k);
        load(i, co_await runs_[i]->next(max_rows));
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
        DataFrame joined = concat_pending(pending);
        pending.clear();
        DataFrame sorted = sort_rows(std::move(joined));
        spill::Writer w(dir_.run_path(id));
        const std::int64_t total = sorted.num_rows();
        // Morsels of about RUN_MORSEL_BYTES, so a merge can read many runs.
        if (total > 0) {
            const std::uint64_t bytes = spill::columns_bytes(sorted.columns);
            const std::uint64_t per_row = std::max<std::uint64_t>(
                1, bytes / static_cast<std::uint64_t>(total));
            chunk = std::min(chunk, std::max<std::int64_t>(
                                        256, static_cast<std::int64_t>(
                                                 RUN_MORSEL_BYTES / per_row)));
        }
        for (std::int64_t off = 0; off < total; off += chunk) {
            const std::int64_t len = std::min(chunk, total - off);
            DataFrame s = sorted.slice(off, len);
            std::vector<Series> cols;
            cols.reserve(s.columns.size());
            for (const Series& c : s.columns) cols.push_back(c.materialize());
            run_morsel_bytes_ = std::max<std::uint64_t>(
                run_morsel_bytes_, spill::columns_bytes(cols));
            w.write(cols, len);
        }
        w.close();
    }

    // A merge holds one morsel per run it reads (measured: about three and a
    // half times the morsel with its keys and read buffers), so when there are
    // more runs than fit the budget, consecutive groups of runs are merged
    // into longer ones first. Consecutive groups keep ties in input order.
    std::size_t merge_fan_in() const {
        const std::uint64_t per_run =
            std::max<std::uint64_t>(run_morsel_bytes_ * 2, 1);
        return static_cast<std::size_t>(
            std::max<std::uint64_t>(4, budget_ / per_run));
    }

    coro::CoroTask<int> merge_runs(const std::vector<int>& ids, int out_id,
                                   std::int64_t max_rows) {
        SortMergeCursor merger(nullptr, sch_, keys_, descending_, budget_);
        merger.key_idx_ = key_idx_;
        for (int id : ids)
            merger.runs_.push_back(
                std::make_unique<spill::Reader>(dir_.run_path(id)));
        merger.cur_.resize(ids.size());
        merger.pos_.assign(ids.size(), 0);
        merger.run_keys_.resize(ids.size());
        for (std::size_t k = 0; k < ids.size(); ++k)
            merger.load(k, co_await merger.runs_[k]->next(max_rows));
        merger.built_ = true;
        spill::Writer w(dir_.run_path(out_id));
        while (auto m = co_await merger.next(max_rows)) {
            run_morsel_bytes_ = std::max<std::uint64_t>(
                run_morsel_bytes_, spill::columns_bytes(m->columns));
            w.write(m->columns, m->rows);
        }
        w.close();
        merger.runs_.clear();
        std::error_code ec;
        for (int id : ids) fs::remove(dir_.run_path(id), ec);
        co_return out_id;
    }

    coro::CoroTask<void> build(std::int64_t max_rows) {
        for (const std::string& key : keys_) {
            const int idx = column_index_of(sch_, key);
            if (idx < 0)
                throw std::out_of_range("sort_by: no column named " + key);
            key_idx_.push_back(idx);
        }

        std::vector<std::vector<Series>> pending;
        std::size_t pend_bytes = 0;
        int run_id = 0;
        while (auto m = co_await in_->next(max_rows)) {
            pend_bytes += morsel_bytes(m->columns);
            pending.push_back(std::move(m->columns));
            if (budget_ > 0 && pend_bytes > budget_ / RUN_COPIES) {
                spill_run(pending, run_id++, max_rows);
                pending.clear();
                pend_bytes = 0;
            }
        }

        if (run_id == 0) {  // everything fits in memory: one sorted run
            DataFrame buf =
                pending.empty() ? DataFrame{} : concat_pending(pending);
            if (pending.empty()) buf.names = sch_;
            DataFrame sorted =
                buf.num_rows() ? sort_rows(std::move(buf)) : std::move(buf);
            runs_.push_back(std::make_unique<InMemoryCursor>(
                std::make_shared<const DataFrame>(std::move(sorted))));
        } else {
            if (!pending.empty()) spill_run(pending, run_id++, max_rows);
            std::vector<int> ids(static_cast<std::size_t>(run_id));
            std::iota(ids.begin(), ids.end(), 0);
            const std::size_t fan_in = merge_fan_in();
            while (ids.size() > fan_in) {
                std::vector<int> next_ids;
                for (std::size_t at = 0; at < ids.size(); at += fan_in) {
                    const std::size_t end = std::min(ids.size(), at + fan_in);
                    if (end - at == 1) {
                        next_ids.push_back(ids[at]);
                        continue;
                    }
                    const std::vector<int> group(ids.begin() + at,
                                                 ids.begin() + end);
                    next_ids.push_back(
                        co_await merge_runs(group, run_id++, max_rows));
                }
                ids = std::move(next_ids);
            }
            for (int id : ids)
                runs_.push_back(
                    std::make_unique<spill::Reader>(dir_.run_path(id)));
        }
        cur_.resize(runs_.size());
        pos_.assign(runs_.size(), 0);
        run_keys_.resize(runs_.size());
        for (std::size_t k = 0; k < runs_.size(); ++k)
            load(k, co_await runs_[k]->next(max_rows));
        built_ = true;
    }

    std::uint64_t run_morsel_bytes_ = 0;
    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::vector<std::string> keys_;
    std::vector<bool> descending_;
    std::uint64_t budget_;
    bool built_ = false;
    std::vector<int> key_idx_;
    spill::Dir dir_;
    std::vector<std::unique_ptr<Cursor>> runs_;
    std::vector<std::optional<Morsel>> cur_;
    std::vector<std::vector<KeyData>> run_keys_;
    std::vector<std::int64_t> pos_;
};

// Streaming distinct (keep first occurrence, original order). The fast path
// holds only the set of distinct row keys - which is the result itself,
// materialized by collect anyway - and streams input and output morsel by
// morsel. When that set outgrows a quarter of `budget_`, the rest of the input
// goes through FirstRowsSpill with one row kept per key.
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
                if (budget_ > 0 && fast_bytes_ > budget_ / 2) spilling_ = true;
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
        if (!external_) {
            external_ = std::make_unique<FirstRowsSpill>(
                budget_, 1, next_row_id_, [this](const std::string& key) {
                    return already_seen(key) ? std::int64_t{1}
                                             : std::int64_t{0};
                });
            while (auto m = co_await in_->next(max_rows)) {
                std::vector<std::string> keys(
                    static_cast<std::size_t>(m->rows));
                const std::vector<Series> kc = key_cols(m->columns);
                parallel_for(m->rows, std::int64_t{1} << 13,
                             [&](std::int64_t b, std::int64_t e) {
                                 for (std::int64_t i = b; i < e; ++i)
                                     keys[static_cast<std::size_t>(i)] =
                                         row_key(kc, i);
                             });
                external_->add(std::move(*m), std::move(keys));
            }
            co_await external_->finish();
        }
        co_return co_await external_->next(max_rows);
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

    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::vector<std::int64_t> key_idx_;
    std::uint64_t budget_;
    ankerl::unordered_dense::set<std::string> seen_;
    std::vector<ankerl::unordered_dense::set<std::string>> seen_p_;
    std::size_t fast_bytes_ = 0;
    std::int64_t next_row_id_ = 0;
    bool spilling_ = false;
    std::unique_ptr<FirstRowsSpill> external_;
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

class RowKeyCursor : public Cursor {
   public:
    explicit RowKeyCursor(std::unique_ptr<Cursor> in) : in_(std::move(in)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return std::nullopt;
        std::vector<std::string> keys(static_cast<std::size_t>(m->rows));
        for (std::int64_t i = 0; i < m->rows; ++i)
            keys[static_cast<std::size_t>(i)] = row_key(m->columns, i);
        Morsel out;
        out.rows = m->rows;
        out.columns.push_back(Series::strings(keys));
        co_return out;
    }

   private:
    std::unique_ptr<Cursor> in_;
};

// Over rows sorted by (key, row_index) as (row_index, key), emits
// (row_index, flag) where the flag says whether the row's key occurs more than
// once (or exactly once, with `unique`). One row of lookahead is the only
// state, so a key repeated across the whole input costs nothing extra.
class RunFlagCursor : public Cursor {
   public:
    RunFlagCursor(std::unique_ptr<Cursor> in, bool unique)
        : in_(std::move(in)), unique_(unique) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        std::vector<std::int64_t> idx;
        std::vector<bool> dup;
        while (!eof_ && static_cast<std::int64_t>(idx.size()) < max_rows) {
            auto m = co_await in_->next(max_rows);
            if (!m) {
                eof_ = true;
                if (has_prev_ && !prev_emitted_) {
                    idx.push_back(prev_idx_);
                    dup.push_back(false);
                }
                break;
            }
            const std::int64_t* ids = m->columns[0].data<std::int64_t>();
            for (std::int64_t i = 0; i < m->rows; ++i) {
                const std::string_view key = m->columns[1].string_at(i);
                if (has_prev_ && key == prev_key_) {
                    if (!prev_emitted_) {
                        idx.push_back(prev_idx_);
                        dup.push_back(true);
                    }
                    idx.push_back(ids[i]);
                    dup.push_back(true);
                    prev_emitted_ = true;
                } else {
                    if (has_prev_ && !prev_emitted_) {
                        idx.push_back(prev_idx_);
                        dup.push_back(false);
                    }
                    prev_key_.assign(key);
                    prev_emitted_ = false;
                }
                has_prev_ = true;
                prev_idx_ = ids[i];
            }
        }
        if (idx.empty()) co_return std::nullopt;
        const auto n = static_cast<std::int64_t>(idx.size());
        std::vector<std::uint8_t> bits(static_cast<std::size_t>((n + 7) / 8),
                                       0);
        for (std::int64_t i = 0; i < n; ++i)
            if (dup[static_cast<std::size_t>(i)] != unique_)
                bits[static_cast<std::size_t>(i / 8)] |=
                    static_cast<std::uint8_t>(1u << (i % 8));
        Morsel out;
        out.rows = n;
        out.columns.push_back(Series::flat_i64(idx.data(), n));
        out.columns.push_back(Series::flat(TypeId::Bool, bits.data(), n));
        co_return out;
    }

   private:
    std::unique_ptr<Cursor> in_;
    bool unique_;
    bool eof_ = false;
    bool has_prev_ = false;
    bool prev_emitted_ = false;
    std::int64_t prev_idx_ = 0;
    std::string prev_key_;
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
        : first_(std::move(in)),
          spool_(budget / 4),
          budget_(budget),
          unique_(unique) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!counted_) co_await count(max_rows);
        if (external_) co_return co_await external_->next(max_rows);
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
        std::size_t cap = accumulate_batch(budget_, 0);
        std::vector<Morsel> batch;
        batch.reserve(MAX_ACCUMULATE_BATCH);
        auto key_series = [](const Morsel& m) {
            std::vector<std::string> keys(static_cast<std::size_t>(m.rows));
            for (std::int64_t i = 0; i < m.rows; ++i)
                keys[static_cast<std::size_t>(i)] = row_key(m.columns, i);
            return Series::strings(keys);
        };
        bool eof = false;
        while (!eof) {
            batch.clear();
            for (std::size_t b = 0; b < cap; ++b) {
                auto m = co_await first_->next(max_rows);
                if (!m) {
                    eof = true;
                    break;
                }
                batch.push_back(std::move(*m));
            }
            if (batch.empty()) break;
            if (spilled_) {
            } else if (batch.size() == 1) {
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
                std::uint64_t partial_bytes = 0;
                for (auto& p : partials)
                    if (p) partial_bytes += agg_approx_bytes(*p);
                cap =
                    accumulate_batch(budget_, partial_bytes / partials.size());
                for (auto& p : partials)
                    if (p) agg_merge(*state, *p);
            }
            for (auto& m : batch) spool_.add(std::move(m.columns), m.rows);
            if (!spilled_ && budget_ > 0 &&
                agg_approx_bytes(*state) > budget_ / 2) {
                spilled_ = true;
                state.reset();
            }
        }
        first_.reset();
        if (spilled_) {
            auto keyed = std::make_unique<RowKeyCursor>(spool_.reader());
            auto numbered =
                std::make_unique<WithRowIndexCursor>(std::move(keyed));
            auto by_key = std::make_unique<SortMergeCursor>(
                std::move(numbered),
                std::vector<std::string>{"row_index", "key"},
                std::vector<std::string>{"key", "row_index"},
                std::vector<bool>{false, false}, budget_);
            auto flagged =
                std::make_unique<RunFlagCursor>(std::move(by_key), unique_);
            auto by_row = std::make_unique<SortMergeCursor>(
                std::move(flagged),
                std::vector<std::string>{"row_index", "flag"},
                std::vector<std::string>{"row_index"}, std::vector<bool>{false},
                budget_);
            external_ = std::make_unique<SelectCursor>(std::move(by_row),
                                                       std::vector<int>{1});
            counted_ = true;
            co_return;
        }
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

    std::unique_ptr<Cursor> first_, pass2_, external_;
    spill::Spool spool_;
    std::uint64_t budget_;
    bool unique_;
    bool spilled_ = false;
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

// Long->wide pivot, matching DataFrame::pivot. Pass 1 spools (index, on,
// value) and collects the distinct `on` values (the output columns, ascending
// as in the eager op). The spooled rows then go through the spilling group-by
// on (index, on) and the spilling sort on index, and the sorted cells are
// widened a batch of index values at a time, so memory is the budget plus one
// batch of output rows, not the input or the output. Rows with a null index are
// dropped; a null `on` makes a row of nulls. Output columns are data-dependent,
// reported via out_names().
class PivotCursor : public Cursor {
   public:
    PivotCursor(std::unique_ptr<Cursor> in, std::uint64_t budget,
                std::vector<std::string> sch, std::string index, std::string on,
                std::string values, std::string agg)
        : first_(std::move(in)),
          spool_(budget / 4),
          budget_(budget),
          sch_(std::move(sch)),
          index_(std::move(index)),
          on_(std::move(on)),
          values_(std::move(values)),
          agg_(std::move(agg)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!built_) co_await build(max_rows);
        const std::int64_t C = static_cast<std::int64_t>(col_of_.size());
        // One output row costs C cells of up to 16 bytes (the source value and
        // its take index), so a batch holds at most a fraction of the budget.
        const std::int64_t cap =
            budget_ == 0 ? std::int64_t{1} << 20
                         : std::max<std::int64_t>(
                               1, static_cast<std::int64_t>(budget_ / 4) /
                                      std::max<std::int64_t>(1, C * 16));
        const std::int64_t pull = max_rows > 0 ? std::min(max_rows, cap) : cap;
        while (true) {
            // `m` is the next sorted morsel and `ahead_` the one after it, so
            // an index value that continues into the next morsel is held
            // back and every other one leaves with this batch.
            std::optional<Morsel> m = std::move(ahead_);
            ahead_.reset();
            if (!m && !eof_) {
                m = co_await sorted_->next(pull);
                if (!m) eof_ = true;
            }
            if (m && !eof_) {
                ahead_ = co_await sorted_->next(pull);
                if (!ahead_) eof_ = true;
            }
            if (!m && pending_rows_ == 0) co_return std::nullopt;
            std::vector<Series> idx, on, val;
            if (pending_rows_ > 0) {
                idx.push_back(pending_[0].share());
                on.push_back(pending_[1].share());
                val.push_back(pending_[2].share());
            }
            if (m) {
                idx.push_back(m->columns[0].share());
                on.push_back(m->columns[1].share());
                val.push_back(m->columns[2].share());
            }
            Series ic = (idx.size() == 1 ? idx[0].share()
                                         : concat_columns(column_ptrs(idx)))
                            .materialize();
            Series oc = (on.size() == 1 ? on[0].share()
                                        : concat_columns(column_ptrs(on)))
                            .materialize();
            Series vc = (val.size() == 1 ? val[0].share()
                                         : concat_columns(column_ptrs(val)))
                            .materialize();
            const std::int64_t n = ic.length();
            std::int64_t done = n;
            if (ahead_) {
                std::vector<Series> one;
                one.push_back(ic.share());
                const std::string last = row_key(one, n - 1);
                std::vector<Series> next_idx;
                next_idx.push_back(ahead_->columns[0].share());
                if (row_key(next_idx, 0) == last) {
                    done = n - 1;
                    while (done > 0 && row_key(one, done - 1) == last) --done;
                }
            }
            const std::int64_t rest = n - done;
            pending_.clear();
            if (rest > 0) {
                std::vector<std::int64_t> tail(static_cast<std::size_t>(rest));
                std::iota(tail.begin(), tail.end(), done);
                pending_.push_back(ic.take(tail));
                pending_.push_back(oc.take(tail));
                pending_.push_back(vc.take(tail));
            }
            pending_rows_ = rest;
            if (done == 0) continue;
            co_return widen(ic, oc, vc, done);
        }
    }

    std::optional<std::vector<std::string>> out_names() const override {
        return produced_;
    }

   private:
    int idx_of(const std::string& name) const {
        auto it = std::find(sch_.begin(), sch_.end(), name);
        return it == sch_.end() ? -1 : static_cast<int>(it - sch_.begin());
    }

    coro::CoroTask<void> build(std::int64_t max_rows) {
        const int ii = idx_of(index_), ci = idx_of(on_), vi = idx_of(values_);
        if (ii < 0) throw std::out_of_range("pivot: no column named " + index_);
        if (ci < 0) throw std::out_of_range("pivot: no column named " + on_);
        if (vi < 0)
            throw std::out_of_range("pivot: no column named " + values_);

        ankerl::unordered_dense::set<std::string> seen;
        std::vector<Series> distinct;
        while (auto m = co_await first_->next(max_rows)) {
            const Series& ic = m->columns[static_cast<std::size_t>(ii)];
            const Series& oc = m->columns[static_cast<std::size_t>(ci)];
            const Series& vc = m->columns[static_cast<std::size_t>(vi)];
            distinct_into(oc, seen, distinct);
            std::vector<std::int64_t> keep;
            keep.reserve(static_cast<std::size_t>(m->rows));
            for (std::int64_t i = 0; i < m->rows; ++i)
                if (!ic.is_null(i)) keep.push_back(i);
            std::vector<Series> cols;
            if (static_cast<std::int64_t>(keep.size()) == m->rows) {
                cols.push_back(ic.share());
                cols.push_back(oc.share());
                cols.push_back(vc.share());
            } else {
                cols.push_back(ic.take(keep));
                cols.push_back(oc.take(keep));
                cols.push_back(vc.take(keep));
            }
            spool_.add(std::move(cols), static_cast<std::int64_t>(keep.size()));
        }
        first_.reset();

        uniq_col_ =
            distinct.empty()
                ? Series{}
                : concat_columns(column_ptrs(distinct)).unique().materialize();
        const std::int64_t C = uniq_col_.length();
        std::vector<Series> one;
        one.push_back(uniq_col_.share());
        for (std::int64_t c = 0; c < C; ++c)
            col_of_.emplace(row_key(one, c), c);
        produced_.push_back(index_);
        for (std::int64_t c = 0; c < C; ++c)
            produced_.push_back(cell_to_string(uniq_col_, c));

        const std::string i_name = "__dftu_pivot_index__";
        const std::string o_name = "__dftu_pivot_on__";
        const std::string v_name = "__dftu_pivot_value__";
        auto grouped = std::make_unique<GroupByCursor>(
            spool_.reader(), std::vector<std::string>{i_name, o_name, v_name},
            std::vector<std::string>{i_name, o_name},
            std::vector<GroupAgg>{GroupAgg{agg_from_string(agg_), v_name, "v"}},
            budget_ / 4);
        sorted_ = std::make_unique<SortMergeCursor>(
            std::move(grouped), std::vector<std::string>{i_name, o_name, "v"},
            std::vector<std::string>{i_name}, std::vector<bool>{false},
            budget_ / 4);
        built_ = true;
    }

    // `rows` leading rows of the three sorted cell columns hold whole index
    // values; each distinct index value becomes one output row.
    Morsel widen(const Series& ic, const Series& oc, const Series& vc,
                 std::int64_t rows) const {
        const std::int64_t C = static_cast<std::int64_t>(col_of_.size());
        std::vector<Series> ione, cone;
        ione.push_back(ic.share());
        cone.push_back(oc.share());
        std::vector<std::int64_t> first_row;
        std::vector<std::int64_t> cell_src;
        std::string prev;
        for (std::int64_t i = 0; i < rows; ++i) {
            std::string key = row_key(ione, i);
            if (first_row.empty() || key != prev) {
                first_row.push_back(i);
                cell_src.resize(cell_src.size() + static_cast<std::size_t>(C),
                                -1);
                prev = std::move(key);
            }
            if (oc.is_null(i)) continue;
            auto it = col_of_.find(row_key(cone, i));
            if (it != col_of_.end())
                cell_src[(first_row.size() - 1) * static_cast<std::size_t>(C) +
                         static_cast<std::size_t>(it->second)] = i;
        }
        const std::size_t R = first_row.size();
        DataFrame out;
        out.names.push_back(index_);
        out.columns.push_back(ic.take(first_row).materialize());
        std::vector<std::int64_t> ti(R);
        for (std::int64_t c = 0; c < C; ++c) {
            for (std::size_t r = 0; r < R; ++r)
                ti[r] = cell_src[r * static_cast<std::size_t>(C) +
                                 static_cast<std::size_t>(c)];
            out.names.push_back(produced_[static_cast<std::size_t>(c) + 1]);
            out.columns.push_back(vc.take(ti).materialize());
        }
        return morsel_of(std::move(out));
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

    std::unique_ptr<Cursor> first_, sorted_;
    spill::Spool spool_;
    std::uint64_t budget_;
    std::vector<std::string> sch_;
    std::string index_, on_, values_, agg_;
    bool built_ = false;
    bool eof_ = false;
    Series uniq_col_;
    ankerl::unordered_dense::map<std::string, std::int64_t> col_of_;
    std::vector<Series> pending_;
    std::int64_t pending_rows_ = 0;
    std::optional<Morsel> ahead_;
    std::vector<std::string> produced_;
};

// Gathers the rows at `indices` in one pass: each input morsel yields the
// wanted rows it holds, and the pieces are put in index order at the end, so
// memory is the result (the size of `indices`), not the input.
class TakeCursor : public Cursor {
   public:
    TakeCursor(std::unique_ptr<Cursor> in, std::vector<std::int64_t> indices)
        : in_(std::move(in)), indices_(std::move(indices)) {
        order_.resize(indices_.size());
        std::iota(order_.begin(), order_.end(), std::int64_t{0});
        std::stable_sort(order_.begin(), order_.end(),
                         [&](std::int64_t a, std::int64_t b) {
                             return indices_[a] < indices_[b];
                         });
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (done_) co_return std::nullopt;
        done_ = true;
        std::vector<std::vector<Series>> pieces;
        std::vector<std::int64_t> where;
        std::size_t at = 0;
        std::int64_t base = 0;
        while (auto m = co_await in_->next(max_rows)) {
            const std::int64_t end = base + m->rows;
            std::vector<std::int64_t> local;
            std::vector<std::int64_t> slots;
            for (; at < order_.size() && indices_[order_[at]] < end; ++at) {
                if (indices_[order_[at]] < base) continue;
                local.push_back(indices_[order_[at]] - base);
                slots.push_back(order_[at]);
            }
            base = end;
            if (local.empty()) continue;
            std::vector<Series> piece;
            piece.reserve(m->columns.size());
            for (const Series& c : m->columns) piece.push_back(c.take(local));
            pieces.push_back(std::move(piece));
            where.insert(where.end(), slots.begin(), slots.end());
        }
        in_.reset();
        if (where.size() != indices_.size())
            throw std::out_of_range("take: index out of range");
        if (pieces.empty()) co_return std::nullopt;
        std::vector<std::int64_t> back(where.size());
        for (std::size_t j = 0; j < where.size(); ++j)
            back[static_cast<std::size_t>(where[j])] =
                static_cast<std::int64_t>(j);
        Morsel out;
        out.rows = static_cast<std::int64_t>(back.size());
        for (std::size_t c = 0; c < pieces.front().size(); ++c) {
            std::vector<const Series*> parts;
            parts.reserve(pieces.size());
            for (auto& pc : pieces) parts.push_back(&pc[c]);
            out.columns.push_back(concat_columns(parts).take(back));
        }
        co_return out;
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::vector<std::int64_t> indices_;
    std::vector<std::int64_t> order_;
    bool done_ = false;
};

// Hash join: collects the right plan on the first pull (the build side), then
// streams each left morsel through it. A Right / Outer join emits the
// unmatched right rows as one final morsel once the left is drained. A right
// side that outgrows the plan's memory budget is joined by GraceJoin instead,
// partition by partition, in no particular row order.
// Whether `lf` can return columns beyond its schema: its source can, or a
// plan it joins or concatenates can.
bool can_carry_rest(const LazyFrame& lf);

// The plan `other` as a cursor whose morsels end in a rest column when
// `rest`, and carry none when not.
std::unique_ptr<Cursor> concat_side(const LazyFrame& other, bool rest);

// The right side's rest column while it passes through a join, apart from
// the left one's.
constexpr std::string_view RIGHT_REST_COLUMN = "__rest_right";

// `cursor`'s morsels as frames named `names`.
coro::AsyncGenerator<DataFrame> named_frames(std::unique_ptr<Cursor> cursor,
                                             std::vector<std::string> names,
                                             std::int64_t rows) {
    while (auto m = co_await cursor->next(rows)) {
        DataFrame f;
        f.names = names;
        f.columns = std::move(m->columns);
        co_yield std::move(f);
    }
}

class JoinCursor : public Cursor {
   public:
    JoinCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
               std::vector<Field> left_fields, LazyFrame other,
               std::vector<std::string> left_on,
               std::vector<std::string> right_on, JoinHow how,
               std::string suffix, std::uint64_t budget, bool nulls_equal,
               bool suffix_extras)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          left_fields_(std::move(left_fields)),
          other_(std::move(other)),
          left_on_(std::move(left_on)),
          right_on_(std::move(right_on)),
          how_(how),
          suffix_(std::move(suffix)),
          nulls_equal_(nulls_equal),
          budget_(budget),
          right_rest_(has_rest(sch_) && how_ != JoinHow::Semi &&
                      how_ != JoinHow::Anti && how_ != JoinHow::Nest &&
                      can_carry_rest(other_)),
          suffix_extras_(suffix_extras) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!join_ && !grace_) co_await build(max_rows);
        if (grace_) co_return co_await next_spilled(max_rows);
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
            if (out.num_rows() > 0) co_return morsel_of(finish(std::move(out)));
        }
        if (flushed_) co_return std::nullopt;
        flushed_ = true;
        if (templates_.empty()) templates_ = null_templates();
        DataFrame rest = join_->flush(sch_, templates_);
        if (rest.num_rows() == 0) co_return std::nullopt;
        co_return morsel_of(finish(std::move(rest)));
    }

   private:
    // Reads the right plan into memory, or into a GraceJoin once its rows pass
    // the budget.
    // The right plan's frames. With a right rest column they keep it, as
    // RIGHT_REST_COLUMN, so every frame has the same columns.
    coro::AsyncGenerator<DataFrame> right_frames(std::int64_t max_rows) const {
        if (!right_rest_) return other_.stream(max_rows);
        std::vector<std::string> names = other_.schema();
        names.emplace_back(RIGHT_REST_COLUMN);
        return named_frames(concat_side(other_, true), std::move(names),
                            max_rows);
    }

    // A join output with the right rest fields moved into the rest column. A
    // right field gets the join suffix when the left can carry undeclared
    // columns (any batch may hold one of that name), else when a left plan
    // column has its name; a name that is still taken is an error.
    DataFrame finish(DataFrame f) const {
        if (!right_rest_) return rest_to_back(std::move(f));
        const int r = column_index_of(f.names, std::string(RIGHT_REST_COLUMN));
        if (r < 0) return rest_to_back(std::move(f));
        const DataFrame right =
            rest_fields(f.columns[static_cast<std::size_t>(r)]);
        f.names.erase(f.names.begin() + r);
        f.columns.erase(f.columns.begin() + r);
        const auto l = static_cast<std::size_t>(
            column_index_of(f.names, std::string(REST_COLUMN)));
        DataFrame fields = rest_fields(f.columns[l]);
        const std::string sfx = suffix_.empty() ? "_right" : suffix_;
        auto taken = [&](const std::string& n) {
            return column_index_of(f.names, n) >= 0 ||
                   column_index_of(fields.names, n) >= 0;
        };
        for (std::size_t i = 0; i < right.names.size(); ++i) {
            std::string name = right.names[i];
            if (suffix_extras_ ||
                std::find(sch_.begin(), sch_.end(), name) != sch_.end())
                name += sfx;
            if (taken(name))
                throw std::invalid_argument(
                    "join: column '" + name +
                    "' is taken on the left; rename or drop one of them");
            fields.names.push_back(std::move(name));
            fields.columns.push_back(right.columns[i].share());
        }
        f.columns[l] =
            struct_of_length(std::move(fields.names), std::move(fields.columns),
                             f.columns[l].length());
        return rest_to_back(std::move(f));
    }

    coro::CoroTask<void> build(std::int64_t max_rows) {
        if (!right_rest_ &&
            (budget_ == NO_SPILL_BUDGET || how_ == JoinHow::Cross)) {
            DataFrame right = co_await other_.collect();
            co_await start_in_memory(std::move(right));
            co_return;
        }
        const bool spill = budget_ != NO_SPILL_BUDGET && how_ != JoinHow::Cross;
        std::vector<DataFrame> parts;
        std::uint64_t bytes = 0;
        auto gen = right_frames(max_rows);
        while (auto df = co_await gen.next()) {
            if (grace_) {
                grace_->add_right(*df);
                continue;
            }
            bytes += spill::columns_bytes(flat_columns(*df));
            parts.push_back(std::move(*df));
            if (!spill || bytes <= budget_) continue;
            grace_ = std::make_unique<GraceJoin>(
                sch_, parts.front().names, left_on_, right_on_, how_, suffix_,
                budget_, nulls_equal_);
            for (const DataFrame& p : parts) grace_->add_right(p);
            parts.clear();
        }
        if (grace_) co_return;
        co_await start_in_memory(merge_morsels(parts));
    }

    static std::vector<Series> flat_columns(const DataFrame& f) {
        std::vector<Series> out;
        out.reserve(f.columns.size());
        for (const Series& c : f.columns)
            out.push_back(c.encoding() == Encoding::Flat ? c.share()
                                                         : c.materialize());
        return out;
    }

    coro::CoroTask<void> start_in_memory(DataFrame right) {
        if (std::optional<Expr> pred = build_side_predicate(right))
            co_await in_->narrow(*pred);
        join_.emplace(std::move(right), left_on_, right_on_, how_, suffix_,
                      nulls_equal_);
        co_return;
    }

    coro::CoroTask<std::optional<Morsel>> next_spilled(std::int64_t max_rows) {
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
            grace_->add_left(left);
        }
        if (!started_) {
            started_ = true;
            if (templates_.empty()) templates_ = null_templates();
            std::vector<Series> shared;
            for (const Series& t : templates_) shared.push_back(t.share());
            grace_->start(std::move(shared));
        }
        auto out = co_await grace_->next(max_rows);
        if (!out) co_return std::nullopt;
        co_return morsel_of(finish(std::move(*out)));
    }

    // A left row whose key is not on the build side is dropped by an Inner,
    // Right or Semi join, so the build keys narrow the left input: the set
    // itself when it is small, its range for a large numeric key, nothing
    // for a large key of another type. Positional against sch_, the join's
    // input; a key column absent from either side is left to probe(), which
    // names it in its error. Nothing is offered when null keys match: the
    // build keys leave out their nulls, and a left row with a null key must
    // still reach the probe.
    std::optional<Expr> build_side_predicate(const DataFrame& right) const {
        if (nulls_equal_ || (how_ != JoinHow::Inner && how_ != JoinHow::Right &&
                             how_ != JoinHow::Semi))
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
            h->set_time_unit(dt.time_unit());
            h->set_timezone(dt.timezone());
            h->set_decimal(dt.decimal_precision(), dt.decimal_scale());
            h->set_fixed_size(dt.fixed_size());
            out.push_back(std::move(s));
        }
        if (has_rest(sch_)) out.push_back(struct_of_length({}, {}, 1));
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
    bool nulls_equal_ = false;
    std::optional<HashJoin> join_;
    std::unique_ptr<GraceJoin> grace_;
    std::uint64_t budget_;
    std::vector<Series> templates_;
    bool flushed_ = false;
    bool started_ = false;
    bool right_rest_;
    bool suffix_extras_;
};

// Vertical concatenation: every morsel of the left plan, then every morsel
// of the right one, which is opened only when the left is drained. Both plans
// share a schema (checked when the op is built), so morsels pass through.
class ConcatCursor : public Cursor {
   public:
    ConcatCursor(std::unique_ptr<Cursor> in, LazyFrame other, bool rest)
        : in_(std::move(in)), other_(std::move(other)), rest_(rest) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (in_) {
            if (auto m = co_await in_->next(max_rows)) {
                m->ordering = Ordering::Unordered;
                co_return m;
            }
            in_.reset();
            right_ = concat_side(other_, rest_);
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
    bool rest_;
    std::unique_ptr<Cursor> right_;
};

// Every morsel of `in`, unchanged, shown to a tap on the way; the tap's end
// runs once the input is drained.
class TapCursor : public Cursor {
   public:
    TapCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
              const std::shared_ptr<const detail::Tap>& tap)
        : in_(std::move(in)), sch_(std::move(sch)), run_(tap->open()) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!in_) co_return std::nullopt;
        auto m = co_await in_->next(max_rows);
        if (!m) {
            in_.reset();
            if (run_.end) run_.end();
            co_return std::nullopt;
        }
        if (run_.rows) {
            DataFrame f;
            if (m->name_ids().empty()) {
                f.names = sch_;
            } else {
                for (const std::uint32_t id : m->name_ids())
                    f.names.emplace_back(m->dyn->intern->resolve(id));
            }
            for (const Series& c : m->columns) f.columns.push_back(c.share());
            unpack_rest(f);
            run_.rows(f);
        }
        co_return m;
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    detail::TapRun run_;
};

// The operands of a registry frame op, deep-copied so the plan owns them for
// as long as it lives: a LazyOp is shared and re-run, so it cannot borrow the
// caller's strings and lists the way a one-shot dftu_op_run_frame call does.
// A FRAME operand after the primary one is a whole LazyFrame, collected when
// the op runs; SERIES shares the column; EXPR / DUQL / LAZY operands are
// refused (no owned form).
// The column names a window spec's union holds for its function.
const char* window_time(const dftu_window_spec& w) {
    if (w.func == DFTU_WINDOW_RATE) return w.param.rate.time;
    if (w.func == DFTU_WINDOW_SESSIONIZE) return w.param.session.time;
    return nullptr;
}

const char* window_end(const dftu_window_spec& w) {
    if (w.func == DFTU_WINDOW_FRAME_ARG_MAX ||
        w.func == DFTU_WINDOW_FRAME_ARG_MIN)
        return w.param.frame.by;
    return w.func == DFTU_WINDOW_SESSIONIZE ? w.param.session.end : nullptr;
}

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
                        const char* time = window_time(w);
                        const char* end = window_end(w);
                        s.strings.emplace_back(time ? time : "");
                        s.has_time.push_back(time != nullptr);
                        s.strings.emplace_back(w.out ? w.out : "");
                        s.strings.emplace_back(end ? end : "");
                        s.has_end.push_back(end != nullptr);
                        s.wins.push_back(w);
                    }
                    break;
                case DFTU_TOK_EXPR:
                case DFTU_TOK_DUQL:
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

    // The strings of operand `i` (a STR or STRLIST operand).
    const std::vector<std::string>& strings_at(std::size_t i) const {
        return slots_[i].strings;
    }

    // The value of operand `i` (an I64 operand).
    std::int64_t i64_at(std::size_t i) const { return slots_[i].val.i64; }
    // The value of operand `i` (an I32 operand).
    std::int32_t i32_at(std::size_t i) const { return slots_[i].val.i32; }

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
            c.has_end = s.has_end;
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
                case DFTU_TOK_DUQL:
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
                            s.has_value[k] ? s.strings[4 * k].c_str() : nullptr;
                        const char* time = s.has_time[k]
                                               ? s.strings[4 * k + 1].c_str()
                                               : nullptr;
                        w.out = s.strings[4 * k + 2].c_str();
                        if (w.func == DFTU_WINDOW_FRAME_ARG_MAX ||
                            w.func == DFTU_WINDOW_FRAME_ARG_MIN) {
                            w.param.frame.by =
                                s.has_end[k] ? s.strings[4 * k + 3].c_str()
                                             : nullptr;
                        } else if (w.func == DFTU_WINDOW_RATE) {
                            w.param.rate.time = time;
                        } else if (w.func == DFTU_WINDOW_SESSIONIZE) {
                            w.param.session.time = time;
                            w.param.session.end =
                                s.has_end[k] ? s.strings[4 * k + 3].c_str()
                                             : nullptr;
                        }
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
        std::vector<bool> has_end;
        std::size_t frame_index = 0;
    };
    std::vector<Slot> slots_;
    std::vector<LazyFrame> frames_;
};

// The registry op that applies per partition: its second operand lists the
// partition columns.
constexpr const char* WINDOW_OP = "dftu.frame.window";

// Runs the registry op `op` over `primary`, collecting each further frame
// operand's plan, and returns its result.
coro::CoroTask<DataFrame> run_frame_op(const dftu_op_desc* op,
                                       const OwnedFrameOpArgs& args,
                                       DataFrame primary,
                                       const std::string& name,
                                       const std::vector<std::string>& sch) {
    std::vector<dftu_dataframe*> handles;
    struct Free {
        std::vector<dftu_dataframe*>& h;
        ~Free() {
            for (dftu_dataframe* p : h) dftu_dataframe_free(p);
        }
    } guard{handles};
    handles.push_back(dataframe_handle_wrap(std::move(primary)));
    for (const LazyFrame& other : args.frames()) {
        DataFrame f = co_await other.collect();
        handles.push_back(dataframe_handle_wrap(std::move(f)));
    }
    std::vector<std::vector<const char*>> cstrs;
    std::vector<std::vector<dftu_window_spec>> wins;
    std::vector<std::vector<dftu_group_agg>> aggs;
    const dftu_op_arg bag = args.bind(cstrs, wins, aggs);
    std::vector<const dftu_dataframe*> frames(handles.begin(), handles.end());
    dftu_dataframe* result = dftu_op_run_frame(
        op, frames.data(), static_cast<uint32_t>(frames.size()), &bag);
    if (!result)
        throw std::runtime_error("lazy frame op '" + name +
                                 "' failed: the op returned no frame");
    DataFrame out = dataframe_handle_take(result);
    if (has_rest(sch)) {
        if (column_index_of(out.names, std::string(REST_COLUMN)) < 0)
            throw std::runtime_error(
                "lazy frame op '" + name +
                "' dropped the columns the scan returned beyond the "
                "plan's schema");
        out = rest_to_back(std::move(out));
    }
    co_return out;
}

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
        // A plan whose columns are only known once it runs has no static
        // schema; its cursor names them.
        std::vector<std::string> names = sch_;
        if (auto produced = in_->out_names(); produced && !produced->empty())
            names = std::move(*produced);
        in_.reset();
        std::unique_ptr<Cursor> reader = spool_.reader();
        DataFrame primary = co_await drain_cursor(*reader, names, max_rows);
        DataFrame out = co_await run_frame_op(op_, *args_, std::move(primary),
                                              name_, names);
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

// compare_agg as a plan: the metrics of each side are prefixed `l_` / `r_`,
// the sides are outer joined on the key columns and sorted by them, and each
// metric both sides carry and that is numeric gets `delta_` and `pct_` columns.
LazyFrame compose_compare_agg(const LazyFrame& base, const LazyFrame& variant,
                              const std::vector<Field>& lhs,
                              const std::vector<Field>& rhs, std::size_t nk) {
    if (nk > lhs.size() || nk > rhs.size())
        throw std::invalid_argument(
            "compare_agg: n_key exceeds a frame's column count");
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < nk; ++i) {
        if (lhs[i].name != rhs[i].name)
            throw std::invalid_argument(
                "compare_agg: key column " + std::to_string(i) + " is '" +
                lhs[i].name + "' vs '" + rhs[i].name + "'");
        keys.push_back(lhs[i].name);
    }
    std::vector<std::string> from_l, to_l, from_r, to_r;
    for (std::size_t i = nk; i < lhs.size(); ++i) {
        from_l.push_back(lhs[i].name);
        to_l.push_back("l_" + lhs[i].name);
    }
    for (std::size_t i = nk; i < rhs.size(); ++i) {
        from_r.push_back(rhs[i].name);
        to_r.push_back("r_" + rhs[i].name);
    }
    LazyFrame out = base.rename_columns(from_l, to_l)
                        .join(variant.rename_columns(from_r, to_r), keys, keys,
                              JoinHow::Outer, "_right")
                        .sort_by_multi(keys, false);
    const Schema joined = out.output_schema();
    auto index_of = [&](const std::string& name) {
        for (std::size_t i = 0; i < joined.fields.size(); ++i)
            if (joined.fields[i].name == name)
                return static_cast<std::int32_t>(i);
        return std::int32_t{-1};
    };
    for (std::size_t i = nk; i < lhs.size(); ++i) {
        const std::string& m = lhs[i].name;
        const std::int32_t li = index_of("l_" + m), ri = index_of("r_" + m);
        if (ri < 0 || !is_numeric_dispatchable(joined.fields[li].type.id) ||
            !is_numeric_dispatchable(joined.fields[ri].type.id))
            continue;
        const Expr delta = expr_col(ri) - expr_col(li);
        out = out.with_column("delta_" + m, delta)
                  .with_column("pct_" + m, (delta * lit(1.0)) /
                                               (expr_col(li) * lit(1.0)) *
                                               lit(100.0));
    }
    return out;
}

constexpr const char* COMPARE_AGG_OP = "dftu.frame.compare_agg";

// The columns of `in` picked by index, in that order.
class ProjectCursor : public Cursor {
   public:
    ProjectCursor(std::unique_ptr<Cursor> in, std::vector<std::size_t> pick)
        : in_(std::move(in)), pick_(std::move(pick)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return std::nullopt;
        Morsel out;
        out.rows = m->rows;
        for (std::size_t i : pick_)
            out.columns.push_back(m->columns[i].share());
        co_return out;
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::vector<std::size_t> pick_;
};

// A plan source over a spool: the rows a cursor produced, with the schema
// read off its first morsel, replayable and spilled past the budget.
class SpoolSource : public Source {
   public:
    SpoolSource(std::shared_ptr<spill::Spool> spool, Schema schema)
        : spool_(std::move(spool)), schema_(std::move(schema)) {}

    Schema schema() const override { return schema_; }

    ScanResult scan(const ScanRequest& req) const override {
        ScanResult r;
        std::unique_ptr<Cursor> reader = spool_->reader();
        if (req.projection.empty()) {
            r.cursor = std::move(reader);
        } else {
            std::vector<std::size_t> pick;
            for (const std::string& name : req.projection) {
                const auto it = std::find_if(
                    schema_.fields.begin(), schema_.fields.end(),
                    [&](const Field& f) { return f.name == name; });
                if (it == schema_.fields.end())
                    throw std::out_of_range("scan: no column named " + name);
                pick.push_back(
                    static_cast<std::size_t>(it - schema_.fields.begin()));
            }
            r.cursor = std::make_unique<ProjectCursor>(std::move(reader),
                                                       std::move(pick));
        }
        r.filters.assign(req.filters.size(), Pushed::No);
        return r;
    }

   private:
    std::shared_ptr<spill::Spool> spool_;
    Schema schema_;
};

// compare_agg over two plans whose columns are only known once they run: both
// sides are spooled (and spill past the budget), their names and types are read
// off the spooled morsels, and the typed plan of compose_compare_agg runs over
// the two spools.
class CompareAggCursor : public Cursor {
   public:
    CompareAggCursor(std::unique_ptr<Cursor> in, std::vector<std::string> sch,
                     LazyFrame other, std::int64_t n_key, std::uint64_t budget)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          other_(std::move(other)),
          n_key_(n_key),
          budget_(budget) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!built_) co_await build(max_rows);
        auto df = co_await gen_->next();
        if (!df) co_return std::nullopt;
        out_names_ = df->names;
        co_return morsel_of(std::move(*df));
    }

    std::optional<std::vector<std::string>> out_names() const override {
        return out_names_;
    }

   private:
    static std::uint64_t share_of(std::uint64_t budget) {
        return budget == 0 || budget == NO_SPILL_BUDGET ? NO_SPILL_BUDGET
                                                        : budget / 4;
    }

    static std::vector<Field> fields_of(const std::vector<std::string>& names,
                                        const std::vector<DataType>& types) {
        if (!types.empty() && types.size() != names.size())
            throw std::runtime_error(
                "compare_agg: a plan names " + std::to_string(names.size()) +
                " columns and returns " + std::to_string(types.size()));
        std::vector<Field> out;
        for (std::size_t i = 0; i < names.size(); ++i)
            out.push_back(
                Field{names[i], types.empty() ? DataType{} : types[i], true});
        return out;
    }

    coro::CoroTask<void> build(std::int64_t max_rows) {
        auto lspool = std::make_shared<spill::Spool>(share_of(budget_));
        auto rspool = std::make_shared<spill::Spool>(share_of(budget_));
        std::vector<std::string> lnames = sch_;
        std::vector<DataType> ltypes;
        while (auto m = co_await in_->next(max_rows)) {
            if (ltypes.empty())
                for (const Series& c : m->columns)
                    ltypes.push_back(c.data_type());
            lspool->add(std::move(m->columns), m->rows);
        }
        if (auto produced = in_->out_names(); produced && !produced->empty())
            lnames = std::move(*produced);
        in_.reset();

        const LazyFrame& other = other_;
        std::vector<std::string> rnames;
        std::vector<DataType> rtypes;
        auto gen = other.stream(max_rows);
        while (auto df = co_await gen.next()) {
            if (rtypes.empty()) {
                rnames = df->names;
                for (const Series& c : df->columns)
                    rtypes.push_back(c.data_type());
            }
            std::vector<Series> cols;
            for (const Series& c : df->columns)
                cols.push_back(c.encoding() == Encoding::Flat
                                   ? c.share()
                                   : c.materialize());
            rspool->add(std::move(cols), df->num_rows());
        }

        const std::vector<Field> lf = fields_of(lnames, ltypes);
        const std::vector<Field> rf = fields_of(rnames, rtypes);
        const LazyFrame left =
            LazyFrame::scan(std::make_shared<SpoolSource>(lspool, Schema{lf}));
        const LazyFrame right =
            LazyFrame::scan(std::make_shared<SpoolSource>(rspool, Schema{rf}));
        LazyFrame composed = compose_compare_agg(
            left, right, lf, rf, static_cast<std::size_t>(n_key_));
        if (budget_ != 0 && budget_ != NO_SPILL_BUDGET)
            composed = composed.memory_budget(budget_);
        plan_.emplace(std::move(composed));
        gen_.emplace(plan_->stream(max_rows));
        built_ = true;
    }

    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    LazyFrame other_;
    std::int64_t n_key_;
    std::uint64_t budget_;
    std::optional<LazyFrame> plan_;
    std::optional<coro::AsyncGenerator<DataFrame>> gen_;
    std::optional<std::vector<std::string>> out_names_;
    bool built_ = false;
};

// One window function of a streaming window, as the cursor needs it: the
// column it reads, its output name, and how many rows around a row it reads.
struct WindowSpecInfo {
    dftu_window_func func = DFTU_WINDOW_ROW_NUMBER;
    int value = -1;
    std::string out;
    std::int64_t before = 0;
    std::int64_t after = 0;
    bool unbounded = false;  // a frame the cursor cannot cut inside
};

std::vector<WindowSpecInfo> window_specs(const OwnedFrameOpArgs& args,
                                         const std::vector<std::string>& sch) {
    std::vector<std::vector<const char*>> cstrs;
    std::vector<std::vector<dftu_window_spec>> wins;
    std::vector<std::vector<dftu_group_agg>> aggs;
    const dftu_op_arg bag = args.bind(cstrs, wins, aggs);
    const auto& list = bag.args[3].winlist;
    std::vector<WindowSpecInfo> out;
    for (std::int32_t k = 0; k < list.n; ++k) {
        const dftu_window_spec& w = list.items[k];
        WindowSpecInfo i;
        i.func = w.func;
        i.out = w.out ? w.out : "";
        if (w.value) {
            const auto it = std::find(sch.begin(), sch.end(), w.value);
            if (it != sch.end()) i.value = static_cast<int>(it - sch.begin());
        }
        switch (w.func) {
            case DFTU_WINDOW_LAG:
                i.before = w.param.offset;
                i.unbounded = w.param.offset < 0;
                break;
            case DFTU_WINDOW_LEAD:
                i.after = w.param.offset;
                i.unbounded = w.param.offset < 0;
                break;
            case DFTU_WINDOW_DELTA:
                i.before = 1;
                break;
            case DFTU_WINDOW_FRAME_SUM:
            case DFTU_WINDOW_FRAME_MIN:
            case DFTU_WINDOW_FRAME_MAX:
            case DFTU_WINDOW_FRAME_COUNT: {
                const auto& f = w.param.frame;
                i.unbounded = f.mode == DFTU_WINDOW_FRAME_RANGE ||
                              f.preceding == DFTU_WINDOW_UNBOUNDED ||
                              f.following == DFTU_WINDOW_UNBOUNDED ||
                              f.preceding < 0 || f.following < 0;
                if (!i.unbounded) {
                    i.before = f.preceding;
                    i.after = f.following;
                }
                break;
            }
            default:
                break;
        }
        out.push_back(std::move(i));
    }
    return out;
}

// Runs the window over a stream sorted by (partition, order), a chunk of about
// `chunk_bytes` at a time. When every function can continue across a cut, a
// chunk may end inside a partition and the next one is given what the
// functions need from the rows before it, so memory is a chunk and its
// result, whatever the size of a partition:
//   running functions (sum, product, min, max, forward fill) get a seed row
//   that holds their value so far; counts and ranks are shifted by the rows
//   already emitted; lag, lead, delta and bounded row frames get the rows
//   around the cut as context, and a chunk holds back the rows that still
//   lack their following context.
// Any other function (ntile, percent_rank, cume_dist, a frame without a bound
// or over a range, distinct and collect frames, sessionize, a float sum or
// mean over a frame) needs its whole partition, so a chunk then ends at a
// partition boundary and a partition larger than a chunk is held whole.
// The window kernel is the same in every case: the chunk's rows leave in the
// kernel's own (partition, order) order, which the sort has already given.
class WindowStreamCursor : public Cursor {
   public:
    WindowStreamCursor(std::unique_ptr<Cursor> sorted,
                       std::vector<std::string> sch,
                       std::vector<std::size_t> part,
                       std::vector<WindowSpecInfo> specs, std::string name,
                       const dftu_op_desc* op,
                       std::shared_ptr<const OwnedFrameOpArgs> args,
                       std::uint64_t chunk_bytes)
        : in_(std::move(sorted)),
          sch_(std::move(sch)),
          part_(std::move(part)),
          name_(std::move(name)),
          op_(op),
          args_(std::move(args)),
          chunk_bytes_(chunk_bytes) {
        for (WindowSpecInfo& w : specs) {
            SpecState s;
            s.info = std::move(w);
            specs_.push_back(std::move(s));
        }
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        while (true) {
            while (!eof_ && (held_rows_ <= after_ ||
                             held_bytes_ < chunk_bytes_ || need_more_)) {
                need_more_ = false;
                auto m = co_await in_->next(max_rows);
                if (!m) {
                    eof_ = true;
                    break;
                }
                if (!planned_) {
                    plan(m->columns);
                    planned_ = true;
                }
                held_bytes_ += spill::columns_bytes(m->columns);
                held_rows_ += m->rows;
                held_.push_back(std::move(*m));
            }
            if (held_rows_ == 0) co_return std::nullopt;

            const std::size_t ncols = sch_.size();
            std::vector<Series> cols;
            cols.reserve(ncols);
            for (std::size_t c = 0; c < ncols; ++c) {
                std::vector<Series> parts;
                for (const Morsel& m : held_)
                    parts.push_back(m.columns[c].share());
                cols.push_back((parts.size() == 1
                                    ? parts[0].share()
                                    : concat_columns(column_ptrs(parts)))
                                   .materialize());
            }
            held_.clear();
            const std::int64_t n = held_rows_;
            std::vector<Series> pk;
            for (std::size_t c : part_) pk.push_back(cols[c].share());
            const auto key = [&](std::int64_t i) { return row_key(pk, i); };

            std::int64_t emit = n;
            if (mode_ == Carry::ALIGNED) {
                if (!eof_) {
                    const std::string last = key(n - 1);
                    emit = n - 1;
                    while (emit > 0 && key(emit - 1) == last) --emit;
                }
            } else if (!eof_) {
                emit = n - after_;
            }
            if (emit <= 0) {
                Morsel all;
                all.rows = n;
                all.columns = std::move(cols);
                held_.push_back(std::move(all));
                need_more_ = true;
                continue;
            }

            const std::int64_t ctx_n = ctx_rows_;
            const std::int64_t take_n = mode_ == Carry::ALIGNED ? emit : n;
            std::vector<Series> fcols;
            fcols.reserve(ncols);
            for (std::size_t c = 0; c < ncols; ++c) {
                Series body = cols[c].share();
                if (take_n < n) {
                    std::vector<std::int64_t> head(
                        static_cast<std::size_t>(take_n));
                    std::iota(head.begin(), head.end(), std::int64_t{0});
                    body = cols[c].take(head).materialize();
                }
                fcols.push_back(
                    ctx_n > 0 ? concat_columns({&ctx_[c], &body}).materialize()
                              : std::move(body));
            }
            DataFrame frame;
            frame.names = sch_;
            for (const Series& c : fcols) frame.columns.push_back(c.share());
            DataFrame out = co_await run_frame_op(op_, *args_, std::move(frame),
                                                  name_, sch_);
            DataFrame res = out.slice(ctx_n, emit);
            for (Series& c : res.columns) c = c.materialize();

            if (mode_ != Carry::ALIGNED) carry(cols, fcols, res, key, n, emit);

            held_rows_ = n - emit;
            held_bytes_ = 0;
            if (n > emit) {
                std::vector<std::int64_t> tail(
                    static_cast<std::size_t>(n - emit));
                std::iota(tail.begin(), tail.end(), emit);
                Morsel rest;
                rest.rows = n - emit;
                for (std::size_t c = 0; c < ncols; ++c)
                    rest.columns.push_back(cols[c].take(tail).materialize());
                held_bytes_ = spill::columns_bytes(rest.columns);
                held_.push_back(std::move(rest));
            }
            out_names_ = res.names;
            co_return morsel_of(std::move(res));
        }
    }

    std::optional<std::vector<std::string>> out_names() const override {
        return out_names_;
    }

   private:
    enum class Carry { ALIGNED, SEED, HALO };
    enum class Acc { NONE, SEED, POST_ADD };

    struct SpecState {
        WindowSpecInfo info;
        Acc acc = Acc::NONE;
        bool is_count = false;
        bool is_rank = false;
        bool is_dense = false;
        std::int64_t last_i = 0;
        std::uint64_t last_u = 0;
    };

    static bool narrow_int(TypeId t) {
        return t == TypeId::Int8 || t == TypeId::Int16 || t == TypeId::Int32 ||
               t == TypeId::Uint8 || t == TypeId::Uint16 || t == TypeId::Uint32;
    }
    static bool any_int(TypeId t) {
        return narrow_int(t) || t == TypeId::Int64 || t == TypeId::Uint64;
    }

    // Chooses how a cut may fall from the functions and the column types.
    void plan(const std::vector<Series>& cols) {
        mode_ = Carry::ALIGNED;
        bool acc = false, halo = false, rank = false;
        std::int64_t before = 0, after = 0;
        std::vector<int> acc_cols;
        for (SpecState& s : specs_) {
            const WindowSpecInfo& w = s.info;
            const TypeId vt =
                w.value >= 0 ? cols[static_cast<std::size_t>(w.value)].type()
                             : TypeId::Unknown;
            if (w.unbounded) return;
            switch (w.func) {
                case DFTU_WINDOW_ROW_NUMBER:
                case DFTU_WINDOW_RUNNING_COUNT:
                    s.is_count = true;
                    break;
                case DFTU_WINDOW_RANK:
                    s.is_rank = true;
                    rank = true;
                    before = std::max<std::int64_t>(before, 1);
                    break;
                case DFTU_WINDOW_DENSE_RANK:
                    s.is_dense = true;
                    rank = true;
                    before = std::max<std::int64_t>(before, 1);
                    break;
                case DFTU_WINDOW_RUNNING_SUM:
                    if (vt == TypeId::Int64 || vt == TypeId::Uint64 ||
                        vt == TypeId::Float64)
                        s.acc = Acc::SEED;
                    else if (narrow_int(vt))
                        s.acc = Acc::POST_ADD;
                    else
                        return;
                    acc = true;
                    acc_cols.push_back(w.value);
                    break;
                case DFTU_WINDOW_RUNNING_PROD:
                    if (vt != TypeId::Float64) return;
                    s.acc = Acc::SEED;
                    acc = true;
                    acc_cols.push_back(w.value);
                    break;
                case DFTU_WINDOW_RUNNING_MIN:
                case DFTU_WINDOW_RUNNING_MAX:
                case DFTU_WINDOW_FILL_FORWARD:
                    s.acc = Acc::SEED;
                    acc = true;
                    acc_cols.push_back(w.value);
                    break;
                case DFTU_WINDOW_FRAME_SUM:
                    if (!any_int(vt)) return;
                    [[fallthrough]];
                case DFTU_WINDOW_LAG:
                case DFTU_WINDOW_LEAD:
                case DFTU_WINDOW_DELTA:
                case DFTU_WINDOW_FRAME_MIN:
                case DFTU_WINDOW_FRAME_MAX:
                case DFTU_WINDOW_FRAME_COUNT:
                    halo = true;
                    before = std::max(before, w.before);
                    after = std::max(after, w.after);
                    break;
                default:
                    return;
            }
        }
        if (acc && (halo || rank)) return;
        if (rank && before > 1) return;
        std::sort(acc_cols.begin(), acc_cols.end());
        if (std::adjacent_find(acc_cols.begin(), acc_cols.end()) !=
            acc_cols.end())
            return;
        mode_ = acc ? Carry::SEED : Carry::HALO;
        before_ = before;
        after_ = after;
    }

    template <class F>
    static Series patched(const Series& col, std::int64_t run, F f) {
        const std::int64_t n = col.length();
        if (col.type() == TypeId::Uint64) {
            std::vector<std::uint64_t> v(col.data<std::uint64_t>(),
                                         col.data<std::uint64_t>() + n);
            for (std::int64_t i = 0; i < run; ++i)
                v[static_cast<std::size_t>(i)] =
                    f(v[static_cast<std::size_t>(i)]);
            return Series::flat(TypeId::Uint64, v.data(), n);
        }
        std::vector<std::int64_t> v(col.data<std::int64_t>(),
                                    col.data<std::int64_t>() + n);
        for (std::int64_t i = 0; i < run; ++i)
            v[static_cast<std::size_t>(i)] = f(v[static_cast<std::size_t>(i)]);
        return Series::flat_i64(v.data(), n);
    }

    // The state after a chunk: the rows of the continuing partition are
    // corrected for the rows before the cut, then the context for the next
    // chunk is taken. `cols` are the chunk's rows, `fcols` the kernel input
    // (context first), `res` the emitted result.
    template <class Key>
    void carry(const std::vector<Series>& cols,
               const std::vector<Series>& fcols, DataFrame& res, const Key& key,
               std::int64_t n, std::int64_t emit) {
        (void)n;
        const std::int64_t ctx_n = ctx_rows_;
        const bool continuing = E_ > 0 && key(0) == last_key_;
        std::int64_t run = 0;
        if (continuing) {
            if (key(emit - 1) == last_key_) {
                run = emit;
            } else {
                std::int64_t lo = 0, hi = emit - 1;
                while (hi - lo > 1) {
                    const std::int64_t mid = lo + (hi - lo) / 2;
                    (key(mid) == last_key_ ? lo : hi) = mid;
                }
                run = hi;
            }
        }
        std::int64_t k_same = 0;
        if (continuing) {
            if (mode_ == Carry::SEED) {
                k_same = 1;
            } else if (ctx_n > 0) {
                std::vector<Series> ck;
                for (std::size_t c : part_) ck.push_back(ctx_[c].share());
                while (k_same < ctx_n &&
                       row_key(ck, ctx_n - 1 - k_same) == last_key_)
                    ++k_same;
            }
        }
        const std::int64_t shift = E_ - k_same;

        for (SpecState& s : specs_) {
            const int oc = column_index_of(res.names, s.info.out);
            if (oc < 0) continue;
            Series& col = res.columns[static_cast<std::size_t>(oc)];
            if (continuing && run > 0) {
                if (s.is_count && shift != 0) {
                    col = patched(col, run,
                                  [&](std::int64_t v) { return v + shift; });
                } else if (s.is_rank) {
                    col = patched(col, run, [&](std::int64_t v) {
                        return v == 1 ? s.last_i : v + shift;
                    });
                } else if (s.is_dense) {
                    col = patched(col, run, [&](std::int64_t v) {
                        return v - 1 + s.last_i;
                    });
                } else if (s.acc == Acc::POST_ADD) {
                    if (col.type() == TypeId::Uint64)
                        col = patched(col, run, [&](std::uint64_t v) {
                            std::uint64_t r = 0;
                            if (__builtin_add_overflow(v, s.last_u, &r))
                                throw std::overflow_error(
                                    "window: RUNNING_SUM overflows uint64");
                            return r;
                        });
                    else
                        col = patched(col, run, [&](std::int64_t v) {
                            std::int64_t r = 0;
                            if (__builtin_add_overflow(v, s.last_i, &r))
                                throw std::overflow_error(
                                    "window: RUNNING_SUM overflows int64");
                            return r;
                        });
                }
            }
            if (s.is_rank || s.is_dense || s.acc == Acc::POST_ADD) {
                if (col.type() == TypeId::Uint64)
                    s.last_u = col.data<std::uint64_t>()[emit - 1];
                else
                    s.last_i = col.data<std::int64_t>()[emit - 1];
            }
        }

        const std::vector<std::int64_t> last_row{emit - 1};
        const std::vector<std::int64_t> null_row{-1};
        ctx_.clear();
        if (mode_ == Carry::SEED) {
            for (std::size_t c = 0; c < cols.size(); ++c) {
                const SpecState* acc = nullptr;
                for (const SpecState& s : specs_)
                    if (s.acc != Acc::NONE &&
                        s.info.value == static_cast<int>(c))
                        acc = &s;
                if (!acc)
                    ctx_.push_back(cols[c].take(last_row).materialize());
                else if (acc->acc == Acc::POST_ADD)
                    ctx_.push_back(cols[c].take(null_row).materialize());
                else
                    ctx_.push_back(
                        res.columns[static_cast<std::size_t>(column_index_of(
                                        res.names, acc->info.out))]
                            .take(last_row)
                            .materialize());
            }
            ctx_rows_ = 1;
        } else {
            const std::int64_t total = ctx_n + emit;
            const std::int64_t keep = std::min(before_, total);
            ctx_rows_ = keep;
            if (keep > 0) {
                std::vector<std::int64_t> idx(static_cast<std::size_t>(keep));
                std::iota(idx.begin(), idx.end(), total - keep);
                for (const Series& c : fcols)
                    ctx_.push_back(c.take(idx).materialize());
            }
        }

        const std::string new_last = key(emit - 1);
        std::int64_t start = 0;
        if (key(0) != new_last) {
            std::int64_t lo = 0, hi = emit - 1;
            while (hi - lo > 1) {
                const std::int64_t mid = lo + (hi - lo) / 2;
                (key(mid) == new_last ? hi : lo) = mid;
            }
            start = hi;
        }
        E_ = (start == 0 && continuing) ? E_ + emit : emit - start;
        last_key_ = new_last;
    }

    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::vector<std::size_t> part_;
    std::string name_;
    const dftu_op_desc* op_;
    std::shared_ptr<const OwnedFrameOpArgs> args_;
    std::uint64_t chunk_bytes_;
    std::vector<SpecState> specs_;
    Carry mode_ = Carry::ALIGNED;
    std::int64_t before_ = 0;
    std::int64_t after_ = 0;
    bool planned_ = false;
    std::vector<Morsel> held_;
    std::int64_t held_rows_ = 0;
    std::uint64_t held_bytes_ = 0;
    std::vector<Series> ctx_;
    std::int64_t ctx_rows_ = 0;
    std::int64_t E_ = 0;
    std::string last_key_;
    bool eof_ = false;
    bool need_more_ = false;
    std::optional<std::vector<std::string>> out_names_;
};

// ---- native group-wise transforms ------------------------------------------

constexpr const char* GROUP_TRANSFORM_OP = "dftu.frame.group_transform";
constexpr std::int64_t NATIVE_SHIFT_MAX = 1024;

enum class NumClass { SIGNED, UNSIGNED, FLOAT };

bool num_class(TypeId t, NumClass& cls) {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
            cls = NumClass::SIGNED;
            return true;
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
            cls = NumClass::UNSIGNED;
            return true;
        case TypeId::Float32:
        case TypeId::Float64:
            cls = NumClass::FLOAT;
            return true;
        default:
            return false;
    }
}

std::uint64_t double_bits(double d) {
    std::uint64_t u;
    std::memcpy(&u, &d, sizeof u);
    return u;
}
double bits_double(std::uint64_t u) {
    double d;
    std::memcpy(&d, &u, sizeof d);
    return d;
}

// The cells of a numeric column as 64-bit patterns (an int64, a uint64 or a
// double by the column's class) and whether each is present.
struct NumCells {
    std::vector<std::uint64_t> bits;
    std::vector<std::uint8_t> ok;
};

NumCells read_cells(const Series& col) {
    const Series s = col.is_flat() ? col.share() : col.materialize();
    const std::int64_t n = s.length();
    NumCells c;
    c.bits.resize(static_cast<std::size_t>(n));
    c.ok.assign(static_cast<std::size_t>(n), 1);
    auto fill = [&](auto at) {
        for (std::int64_t i = 0; i < n; ++i)
            c.bits[static_cast<std::size_t>(i)] = at(i);
    };
    switch (s.type()) {
        case TypeId::Int8:
            fill([&](std::int64_t i) {
                return static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(s.data<std::int8_t>()[i]));
            });
            break;
        case TypeId::Int16:
            fill([&](std::int64_t i) {
                return static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(s.data<std::int16_t>()[i]));
            });
            break;
        case TypeId::Int32:
            fill([&](std::int64_t i) {
                return static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(s.data<std::int32_t>()[i]));
            });
            break;
        case TypeId::Int64:
            fill([&](std::int64_t i) {
                return static_cast<std::uint64_t>(s.data<std::int64_t>()[i]);
            });
            break;
        case TypeId::Uint8:
            fill([&](std::int64_t i) {
                return std::uint64_t{s.data<std::uint8_t>()[i]};
            });
            break;
        case TypeId::Uint16:
            fill([&](std::int64_t i) {
                return std::uint64_t{s.data<std::uint16_t>()[i]};
            });
            break;
        case TypeId::Uint32:
            fill([&](std::int64_t i) {
                return std::uint64_t{s.data<std::uint32_t>()[i]};
            });
            break;
        case TypeId::Uint64:
            fill([&](std::int64_t i) { return s.data<std::uint64_t>()[i]; });
            break;
        case TypeId::Float32:
            fill([&](std::int64_t i) {
                return double_bits(static_cast<double>(s.data<float>()[i]));
            });
            break;
        case TypeId::Float64:
            fill([&](std::int64_t i) {
                return double_bits(s.data<double>()[i]);
            });
            break;
        default:
            throw std::logic_error("group transform: a non-numeric column");
    }
    if (s.null_count() > 0)
        for (std::int64_t i = 0; i < n; ++i)
            if (s.is_null(i)) c.ok[static_cast<std::size_t>(i)] = 0;
    return c;
}

// A column of 64-bit patterns of class `cls`, narrowed or widened to the type
// the plan declares, with a null where `ok` is zero.
Series make_numeric(const std::vector<std::uint64_t>& bits,
                    const std::vector<std::uint8_t>& ok, NumClass cls,
                    TypeId out) {
    const auto n = static_cast<std::int64_t>(bits.size());
    bool any_null = false;
    std::vector<std::uint8_t> bitmap((bits.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (ok[i])
            bitmap[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        else
            any_null = true;
    }
    const std::uint8_t* validity = any_null ? bitmap.data() : nullptr;
    auto as = [&](auto tag) {
        using T = decltype(tag);
        std::vector<T> v(bits.size());
        for (std::size_t i = 0; i < bits.size(); ++i) {
            switch (cls) {
                case NumClass::SIGNED:
                    v[i] = static_cast<T>(static_cast<std::int64_t>(bits[i]));
                    break;
                case NumClass::UNSIGNED:
                    v[i] = static_cast<T>(bits[i]);
                    break;
                case NumClass::FLOAT:
                    v[i] = static_cast<T>(bits_double(bits[i]));
                    break;
            }
        }
        return Series::flat(out, v.data(), n, validity);
    };
    switch (out) {
        case TypeId::Int8:
            return as(std::int8_t{});
        case TypeId::Int16:
            return as(std::int16_t{});
        case TypeId::Int32:
            return as(std::int32_t{});
        case TypeId::Int64:
            return as(std::int64_t{});
        case TypeId::Uint8:
            return as(std::uint8_t{});
        case TypeId::Uint16:
            return as(std::uint16_t{});
        case TypeId::Uint32:
            return as(std::uint32_t{});
        case TypeId::Uint64:
            return as(std::uint64_t{});
        case TypeId::Float32:
            return as(float{});
        case TypeId::Float64:
            return as(double{});
        default:
            throw std::logic_error("group transform: a non-numeric output");
    }
}

// The state of one value column in one group.
struct TransformCell {
    std::uint64_t acc = 0;   // running sum, extreme or last present value
    std::uint64_t last = 0;  // the previous row's value
    double prod = 1.0;
    std::int64_t seen = 0;   // rows of the group so far
    bool has = false;        // `acc` holds a value
    bool last_ok = false;    // the previous row is present
};

// The group-wise transforms over numeric columns, one morsel at a time: each
// group keeps its state (an accumulator, the previous row, a ring of the last
// `n` rows) across morsels. The results are those of the window plan the
// transform otherwise composes, including its null handling (a null input
// gives a null output for the running ops, the running ops skip nulls) and its
// `x + (c - c)` that turns an infinite or NaN input into NaN.
class GroupTransformCore {
   public:
    GroupTransformCore(GroupwiseOp kind, std::int64_t shift,
                       std::vector<std::size_t> in,
                       std::vector<TypeId> declared)
        : kind_(kind),
          shift_(shift),
          in_(std::move(in)),
          declared_(std::move(declared)) {}

    // `gid[r]` is the group of row r; `starts[r]`, when given, restarts that
    // group's state before the row (a sorted stream reuses one group id).
    std::vector<Series> run(const std::vector<Series>& cols, std::int64_t rows,
                            const std::int32_t* gid,
                            const std::uint8_t* starts) {
        std::vector<Series> out;
        const auto n = static_cast<std::size_t>(rows);
        if (kind_ == GroupwiseOp::CumCount) {
            std::vector<std::int64_t> v(n);
            for (std::size_t r = 0; r < n; ++r) {
                const auto g = static_cast<std::size_t>(gid[r]);
                ensure(g);
                if (starts && starts[r]) counts_[g] = 0;
                v[r] = counts_[g]++;
            }
            out.push_back(Series::flat_i64(v.data(), rows));
            return out;
        }
        for (std::size_t c = 0; c < in_.size(); ++c) {
            NumClass cls = NumClass::SIGNED;
            if (!num_class(cols[in_[c]].type(), cls))
                throw std::logic_error("group transform: a non-numeric column");
            const NumCells cells = read_cells(cols[in_[c]]);
            std::vector<std::uint64_t> ob(n);
            std::vector<std::uint8_t> ook(n, 1);
            NumClass produced = cls;
            for (std::size_t r = 0; r < n; ++r) {
                const auto g = static_cast<std::size_t>(gid[r]);
                ensure(g);
                TransformCell& st = cell(g, c);
                if (starts && starts[r]) {
                    st = TransformCell{};
                    if (shift_ > 0) ring_of(g, c).clear();
                }
                produced = step(st, g, c, cls, cells.bits[r], cells.ok[r] != 0,
                                ob[r], ook[r]);
            }
            out.push_back(make_numeric(ob, ook, produced, declared_[c]));
        }
        return out;
    }

   private:
    struct RingCell {
        std::uint64_t bits = 0;
        std::uint8_t ok = 0;
    };

    static bool less_than(NumClass cls, std::uint64_t a, std::uint64_t b) {
        switch (cls) {
            case NumClass::SIGNED:
                return static_cast<std::int64_t>(a) <
                       static_cast<std::int64_t>(b);
            case NumClass::UNSIGNED:
                return a < b;
            case NumClass::FLOAT:
                return bits_double(a) < bits_double(b);
        }
        return false;
    }

    void ensure(std::size_t g) {
        if (g < counts_.size()) return;
        counts_.resize(g + 1, 0);
        cells_.resize((g + 1) * in_.size());
        if (shift_ > 0) rings_.resize((g + 1) * in_.size());
    }
    TransformCell& cell(std::size_t g, std::size_t c) {
        return cells_[g * in_.size() + c];
    }
    std::vector<RingCell>& ring_of(std::size_t g, std::size_t c) {
        return rings_[g * in_.size() + c];
    }

    // One row of one column; returns the class of the value it produced.
    NumClass step(TransformCell& st, std::size_t g, std::size_t c, NumClass cls,
                  std::uint64_t b, bool ok, std::uint64_t& out,
                  std::uint8_t& out_ok) {
        const double x = cls == NumClass::FLOAT ? bits_double(b) : 0.0;
        switch (kind_) {
            case GroupwiseOp::CumSum: {
                if (!ok) {
                    out_ok = 0;
                    return cls;
                }
                if (cls == NumClass::SIGNED) {
                    std::int64_t s = static_cast<std::int64_t>(st.acc);
                    if (__builtin_add_overflow(s, static_cast<std::int64_t>(b),
                                               &s))
                        throw std::overflow_error(
                            "window: RUNNING_SUM overflows int64");
                    st.acc = static_cast<std::uint64_t>(s);
                    out = st.acc;
                } else if (cls == NumClass::UNSIGNED) {
                    if (__builtin_add_overflow(st.acc, b, &st.acc))
                        throw std::overflow_error(
                            "window: RUNNING_SUM overflows uint64");
                    out = st.acc;
                } else {
                    const double s = bits_double(st.acc) + x;
                    st.acc = double_bits(s);
                    out = double_bits(s + (x - x));
                }
                return cls;
            }
            case GroupwiseOp::CumProd: {
                if (!ok) {
                    out_ok = 0;
                    return NumClass::FLOAT;
                }
                const double v = cls == NumClass::FLOAT
                                     ? x
                                     : (cls == NumClass::SIGNED
                                            ? static_cast<double>(
                                                  static_cast<std::int64_t>(b))
                                            : static_cast<double>(b));
                st.prod *= v;
                const double d = cls == NumClass::FLOAT ? (x - x) : 0.0;
                out = double_bits(st.prod + d);
                return NumClass::FLOAT;
            }
            case GroupwiseOp::CumMax:
            case GroupwiseOp::CumMin: {
                if (!ok) {
                    out_ok = 0;
                    return cls;
                }
                const bool smallest = kind_ == GroupwiseOp::CumMin;
                if (!st.has || (smallest ? less_than(cls, b, st.acc)
                                         : less_than(cls, st.acc, b))) {
                    st.acc = b;
                    st.has = true;
                }
                out = cls == NumClass::FLOAT
                          ? double_bits(bits_double(st.acc) + (x - x))
                          : st.acc;
                return cls;
            }
            case GroupwiseOp::Shift: {
                if (shift_ == 0) {
                    out = b;
                    out_ok = ok ? 1 : 0;
                    return cls;
                }
                std::vector<RingCell>& ring = ring_of(g, c);
                if (ring.empty()) ring.resize(static_cast<std::size_t>(shift_));
                RingCell& slot =
                    ring[static_cast<std::size_t>(st.seen % shift_)];
                if (st.seen >= shift_ && slot.ok) {
                    out = slot.bits;
                } else {
                    out_ok = 0;
                }
                slot.bits = b;
                slot.ok = ok ? 1 : 0;
                ++st.seen;
                return cls;
            }
            case GroupwiseOp::Diff: {
                const bool have = st.seen > 0 && st.last_ok && ok;
                const std::uint64_t prev = st.last;
                st.last = b;
                st.last_ok = ok;
                ++st.seen;
                if (!have) {
                    out_ok = 0;
                    return cls == NumClass::FLOAT ? NumClass::FLOAT
                                                  : NumClass::SIGNED;
                }
                if (cls == NumClass::FLOAT) {
                    out = double_bits(x - bits_double(prev));
                    return NumClass::FLOAT;
                }
                std::int64_t d = 0;
                bool bad = false;
                if (cls == NumClass::SIGNED) {
                    bad = __builtin_sub_overflow(
                        static_cast<std::int64_t>(b),
                        static_cast<std::int64_t>(prev), &d);
                } else if (b >= prev) {
                    const std::uint64_t m = b - prev;
                    bad = m > static_cast<std::uint64_t>(
                                  std::numeric_limits<std::int64_t>::max());
                    d = static_cast<std::int64_t>(m);
                } else {
                    const std::uint64_t m = prev - b;
                    bad = m > (std::uint64_t{1} << 63);
                    d = static_cast<std::int64_t>(0 - m);
                }
                if (bad)
                    throw std::overflow_error("window: DELTA overflows int64");
                out = static_cast<std::uint64_t>(d);
                return NumClass::SIGNED;
            }
            case GroupwiseOp::FFill: {
                if (ok) {
                    st.acc = b;
                    st.has = true;
                }
                if (st.has)
                    out = st.acc;
                else
                    out_ok = 0;
                return cls;
            }
            default:
                throw std::logic_error("group transform: an unsupported kind");
        }
    }

    GroupwiseOp kind_;
    std::int64_t shift_;
    std::vector<std::size_t> in_;
    std::vector<TypeId> declared_;
    std::vector<std::int64_t> counts_;
    std::vector<TransformCell> cells_;
    std::vector<std::vector<RingCell>> rings_;
};

// Runs a GroupTransformCore over a stream. Rows are grouped by a hash of the
// key columns, or, when the stream is sorted by them, by runs of equal keys
// that keep one group's state; with `keep_row` the last input column (the row
// number) follows the results.
class TransformPassCursor : public Cursor {
   public:
    TransformPassCursor(std::unique_ptr<Cursor> in,
                        std::vector<std::size_t> keys, bool sorted,
                        bool keep_row, GroupTransformCore core)
        : in_(std::move(in)),
          keys_(std::move(keys)),
          sorted_(sorted),
          keep_row_(keep_row),
          core_(std::move(core)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        while (true) {
            auto m = co_await in_->next(max_rows);
            if (!m) co_return std::nullopt;
            if (m->rows == 0) continue;
            const auto n = static_cast<std::size_t>(m->rows);
            std::vector<std::int32_t> gid(n, 0);
            std::vector<std::uint8_t> starts;
            if (!keys_.empty()) {
                std::vector<Series> kc;
                for (std::size_t k : keys_)
                    kc.push_back(m->columns[k].is_flat()
                                     ? m->columns[k].share()
                                     : m->columns[k].materialize());
                if (sorted_) {
                    starts.assign(n, 0);
                    for (std::size_t r = 0; r < n; ++r) {
                        std::string key =
                            row_key(kc, static_cast<std::int64_t>(r));
                        if (!have_last_ || key != last_key_) starts[r] = 1;
                        last_key_ = std::move(key);
                        have_last_ = true;
                    }
                } else {
                    for (std::size_t r = 0; r < n; ++r) {
                        const std::string key =
                            row_key(kc, static_cast<std::int64_t>(r));
                        auto it = ids_.find(key);
                        if (it == ids_.end())
                            it = ids_.emplace(key, static_cast<std::int32_t>(
                                                       ids_.size()))
                                     .first;
                        gid[r] = it->second;
                    }
                }
            }
            Morsel out;
            out.rows = m->rows;
            out.columns = core_.run(m->columns, m->rows, gid.data(),
                                    starts.empty() ? nullptr : starts.data());
            if (keep_row_) out.columns.push_back(m->columns.back().share());
            co_return out;
        }
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::vector<std::size_t> keys_;
    bool sorted_;
    bool keep_row_;
    GroupTransformCore core_;
    ankerl::unordered_dense::map<std::string, std::int32_t> ids_;
    std::string last_key_;
    bool have_last_ = false;
};

// The native group transform of a plan step. Unbounded budgets stream through
// the hash pass. Under a budget the input is spooled first; when it stays in
// memory and its groups fit a quarter of the budget, the hash pass runs over
// the spool in input order. Otherwise the spool is sorted by (keys, row),
// streamed through a pass that keeps one group's state, and the results are
// sorted back by row, so memory is the budget however many groups there are.
class NativeTransformCursor : public Cursor {
   public:
    NativeTransformCursor(std::unique_ptr<Cursor> in,
                          std::vector<std::string> sch,
                          std::vector<std::size_t> keys, GroupwiseOp kind,
                          std::int64_t shift, std::vector<std::size_t> value,
                          std::vector<TypeId> declared,
                          std::vector<std::string> out_names,
                          std::uint64_t budget)
        : in_(std::move(in)),
          sch_(std::move(sch)),
          keys_(std::move(keys)),
          kind_(kind),
          shift_(shift),
          value_(std::move(value)),
          declared_(std::move(declared)),
          out_names_(std::move(out_names)),
          budget_(budget) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        if (!tail_) co_await build(max_rows);
        co_return co_await tail_->next(max_rows);
    }

    std::optional<std::vector<std::string>> out_names() const override {
        return out_names_;
    }

   private:
    GroupTransformCore core() const {
        return GroupTransformCore(kind_, shift_, value_, declared_);
    }

    coro::CoroTask<void> build(std::int64_t max_rows) {
        const bool bounded = budget_ != NO_SPILL_BUDGET && budget_ > 0;
        if (!bounded || keys_.empty()) {
            tail_ = std::make_unique<TransformPassCursor>(std::move(in_), keys_,
                                                          false, false, core());
            co_return;
        }
        // Per-group state: a cell per value column and a counter, and the
        // ring of a shift, doubled for the table around them.
        const std::uint64_t per_group =
            2 *
            (sizeof(TransformCell) * std::max<std::size_t>(1, value_.size()) +
             sizeof(std::int64_t) +
             static_cast<std::uint64_t>(shift_) * 16 * value_.size() + 48);
        const std::uint64_t cap_groups =
            std::max<std::uint64_t>(1, (budget_ / 4) / per_group);
        spool_ = std::make_shared<spill::Spool>(budget_ / 2);
        spill::Spool& spool = *spool_;
        ankerl::unordered_dense::set<std::string> groups;
        bool too_many = false;
        std::int64_t row = 0;
        while (auto m = co_await in_->next(max_rows)) {
            if (m->rows == 0) continue;
            if (!too_many && !spool.spilled()) {
                std::vector<Series> kc;
                for (std::size_t k : keys_)
                    kc.push_back(m->columns[k].is_flat()
                                     ? m->columns[k].share()
                                     : m->columns[k].materialize());
                for (std::int64_t r = 0; r < m->rows && !too_many; ++r) {
                    groups.insert(row_key(kc, r));
                    too_many = groups.size() > cap_groups;
                }
            }
            std::vector<std::int64_t> idx(static_cast<std::size_t>(m->rows));
            std::iota(idx.begin(), idx.end(), row);
            row += m->rows;
            std::vector<Series> cols = std::move(m->columns);
            cols.push_back(Series::flat_i64(idx.data(), m->rows));
            spool.add(std::move(cols), m->rows);
        }
        in_.reset();
        if (!spool.spilled() && !too_many) {
            tail_ = std::make_unique<TransformPassCursor>(spool.reader(), keys_,
                                                          false, false, core());
            co_return;
        }
        std::vector<std::string> names = sch_;
        names.emplace_back(ROW_COLUMN);
        std::vector<std::string> sort_keys;
        for (std::size_t k : keys_) sort_keys.push_back(sch_[k]);
        sort_keys.emplace_back(ROW_COLUMN);
        auto sorted = std::make_unique<SortMergeCursor>(
            spool.reader(), names, sort_keys,
            std::vector<bool>(sort_keys.size(), false), budget_);
        std::vector<std::string> pass_names = out_names_;
        pass_names.emplace_back(ROW_COLUMN);
        auto pass = std::make_unique<TransformPassCursor>(
            std::move(sorted), keys_, true, true, core());
        auto restored = std::make_unique<SortMergeCursor>(
            std::move(pass), pass_names, std::vector<std::string>{ROW_COLUMN},
            std::vector<bool>{false}, budget_);
        std::vector<std::size_t> pick(out_names_.size());
        std::iota(pick.begin(), pick.end(), std::size_t{0});
        tail_ = std::make_unique<ProjectCursor>(std::move(restored),
                                                std::move(pick));
    }

    static constexpr const char* ROW_COLUMN = "__dftu_trow__";

    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::vector<std::size_t> keys_;
    GroupwiseOp kind_;
    std::int64_t shift_;
    std::vector<std::size_t> value_;
    std::vector<TypeId> declared_;
    std::vector<std::string> out_names_;
    std::uint64_t budget_;
    std::shared_ptr<spill::Spool> spool_;
    std::unique_ptr<Cursor> tail_;
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
struct DropOp {
    std::vector<std::string> names;
};
struct RenameColumnsOp {
    std::vector<std::string> from;
    std::vector<std::string> to;
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
    // The output types when the step knows them; else those of the input
    // columns with the same names.
    std::vector<DataType> out_types;
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
struct HeadByOp {
    std::vector<std::string> keys;
    std::int64_t n;
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
    bool nulls_equal = false;
};
struct ConcatOp {
    LazyFrame other;
};
struct TapOp {
    std::shared_ptr<const detail::Tap> tap;
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
    std::variant<FilterOp, SelectOp, WithColumnOp, RenameOp, DropOp,
                 RenameColumnsOp, SliceOp, TailOp, DropNullsOp, FillNullOp,
                 WithRowIndexOp, NullCountOp, ExplodeOp, UnpivotOp, TopkOp,
                 GroupByOp, SortByOp, UniqueOp, SampleOp, HeadByOp, IsDupOp,
                 GroupByDynamicOp, PivotOp, ToDummiesOp, DescribeOp, ReverseOp,
                 TakeOp, FilterMaskOp, SortByMultiOp, JoinOp, ConcatOp,
                 UnnestOp, FrameOp, NodeOp, TapOp>
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
        case JoinHow::Lookup:
            return "lookup";
        case JoinHow::Nest:
            return "nest";
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

std::unordered_map<std::string, std::string> rename_map(
    const std::vector<std::string>& from, const std::vector<std::string>& to) {
    std::unordered_map<std::string, std::string> map;
    for (std::size_t i = 0; i < from.size(); ++i) map.emplace(from[i], to[i]);
    return map;
}

// The names after a by-name rename. Two columns left sharing a name is an
// error, not a silent overwrite.
std::vector<std::string> renamed_names(
    std::vector<std::string> in,
    const std::unordered_map<std::string, std::string>& map) {
    std::unordered_set<std::string> seen;
    for (std::string& n : in) {
        auto it = map.find(n);
        if (it != map.end()) n = it->second;
        if (!seen.insert(n).second)
            throw std::invalid_argument(
                "rename_columns: column name '" + n +
                "' is used by two columns after the rename");
    }
    return in;
}

std::vector<std::string> dropped_names(std::vector<std::string> in,
                                       const std::vector<std::string>& drop) {
    in.erase(std::remove_if(in.begin(), in.end(),
                            [&](const std::string& n) {
                                return std::find(drop.begin(), drop.end(), n) !=
                                       drop.end();
                            }),
             in.end());
    return in;
}

std::vector<std::string> without_rest(std::vector<std::string> names) {
    names.erase(std::remove(names.begin(), names.end(), REST_COLUMN),
                names.end());
    return names;
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
            [&](const SelectOp& o) { return without_rest(o.names); },
            [&](const RenameOp& o) { return o.names; },
            [&](const DropOp& o) {
                return dropped_names(std::move(in), o.names);
            },
            [&](const RenameColumnsOp& o) {
                return renamed_names(std::move(in), rename_map(o.from, o.to));
            },
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
            [&](const HeadByOp&) { return in; },
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
            [&](const TapOp&) { return in; },
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
        DataType t = agg_output_type(to_agg_op(a.op), vt, a.param);
        if (a.op == Agg::SetUnion && a.param != 0.0 &&
            (vt == TypeId::List || vt == TypeId::LargeList))
            t = list_of(set_union_element_type(
                find_field(in, a.column)->type.fields[0].type.id));
        out.push_back(Field{a.out, std::move(t), true});
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
                for (const std::string& name : without_rest(o.names)) {
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
            [&](const DropOp& o) {
                in.fields.erase(
                    std::remove_if(
                        in.fields.begin(), in.fields.end(),
                        [&](const Field& f) {
                            return std::find(o.names.begin(), o.names.end(),
                                             f.name) != o.names.end();
                        }),
                    in.fields.end());
                return in;
            },
            [&](const RenameColumnsOp& o) {
                std::vector<std::string> names;
                names.reserve(in.fields.size());
                for (const Field& f : in.fields) names.push_back(f.name);
                names =
                    renamed_names(std::move(names), rename_map(o.from, o.to));
                for (std::size_t i = 0; i < names.size(); ++i)
                    in.fields[i].name = std::move(names[i]);
                return in;
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
            [&](const HeadByOp&) { return in; },
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
            [&](const TapOp&) { return in; },
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
                for (std::size_t i = 0; i < o.out_names.size(); ++i) {
                    const std::string& name = o.out_names[i];
                    if (i < o.out_types.size()) {
                        out.fields.push_back(Field{name, o.out_types[i], true});
                        continue;
                    }
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
            [](const DropOp& o) {
                return "drop [" + join_names(o.names) + "]";
            },
            [](const RenameColumnsOp& o) {
                return "rename_columns [" + join_names(o.from) + "] -> [" +
                       join_names(o.to) + "]";
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
            [](const HeadByOp& o) {
                return "head_by [" + join_names(o.keys) + "] " +
                       std::to_string(o.n);
            },
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
                       join_names(o.right_on) + "]" +
                       (o.nulls_equal ? " nulls_equal" : "");
            },
            [](const ConcatOp&) { return std::string("concat"); },
            [](const TapOp&) { return std::string("tap"); },
            [](const UnnestOp& o) {
                return "unnest " + o.column +
                       (o.keep_empty ? " keep_empty" : "");
            },
            [](const FrameOp& o) { return "frame_op " + o.name; },
            [](const NodeOp& o) { return "op " + o.name; }},
        op.node);
}

// What an op does with the rest column. Moves: it keeps the column with the
// rows it keeps, as any other column. Keys: Moves, and whole-row identity
// includes it. Omits: its output has none of its input columns but those it
// names, so the rest column is dropped before it. None: no rule, so a plan
// that carries the rest column into the op is refused.
enum class RestRule : std::uint8_t { Moves, Keys, Omits, None };

RestRule rest_rule(const LazyOp& op, const std::vector<std::string>& in) {
    return std::visit(
        overloaded{[](const FilterOp&) { return RestRule::Moves; },
                   [](const SelectOp& o) {
                       return std::find(o.names.begin(), o.names.end(),
                                        REST_COLUMN) != o.names.end()
                                  ? RestRule::Moves
                                  : RestRule::Omits;
                   },
                   [](const WithColumnOp&) { return RestRule::Moves; },
                   [](const RenameOp&) { return RestRule::Moves; },
                   [](const DropOp&) { return RestRule::Moves; },
                   [](const RenameColumnsOp&) { return RestRule::Moves; },
                   [](const SliceOp&) { return RestRule::Moves; },
                   [](const TailOp&) { return RestRule::Moves; },
                   [](const DropNullsOp&) { return RestRule::Moves; },
                   [](const FillNullOp&) { return RestRule::Moves; },
                   [](const WithRowIndexOp&) { return RestRule::Moves; },
                   [](const NullCountOp&) { return RestRule::Omits; },
                   [](const ExplodeOp&) { return RestRule::Moves; },
                   [](const UnpivotOp&) { return RestRule::Omits; },
                   [](const TopkOp&) { return RestRule::Moves; },
                   [](const GroupByOp&) { return RestRule::Omits; },
                   [](const SortByOp&) { return RestRule::Moves; },
                   [](const UniqueOp& o) {
                       return o.subset.empty() ? RestRule::Keys
                                               : RestRule::Moves;
                   },
                   [](const SampleOp&) { return RestRule::Moves; },
                   [](const HeadByOp&) { return RestRule::Moves; },
                   [](const IsDupOp&) { return RestRule::Keys; },
                   [](const GroupByDynamicOp&) { return RestRule::Omits; },
                   [](const PivotOp&) { return RestRule::Omits; },
                   [](const ToDummiesOp&) { return RestRule::Omits; },
                   [](const DescribeOp&) { return RestRule::Omits; },
                   [](const ReverseOp&) { return RestRule::Moves; },
                   [](const TakeOp&) { return RestRule::Moves; },
                   [](const FilterMaskOp&) { return RestRule::Moves; },
                   [](const SortByMultiOp&) { return RestRule::Moves; },
                   [](const JoinOp&) { return RestRule::Moves; },
                   [](const ConcatOp&) { return RestRule::Moves; },
                   [](const UnnestOp&) { return RestRule::Moves; },
                   // A frame op that returns every input column keeps the rest
                   // column too; one that does not builds its output from what
                   // it names.
                   [&](const FrameOp& o) {
                       for (std::size_t i = 0; i + 1 < in.size(); ++i)
                           if (std::find(o.out_names.begin(), o.out_names.end(),
                                         in[i]) == o.out_names.end())
                               return RestRule::Omits;
                       return RestRule::Moves;
                   },
                   [](const NodeOp&) { return RestRule::None; },
                   [](const TapOp&) { return RestRule::Moves; }},
        op.node);
}

bool can_carry_rest(const LazyFrame& lf) {
    if (detail::PlanAccess::source(lf).undeclared_columns()) return true;
    for (const auto& op : detail::PlanAccess::ops(lf)) {
        if (const auto* j = std::get_if<JoinOp>(&op->node))
            if (can_carry_rest(j->other)) return true;
        if (const auto* c = std::get_if<ConcatOp>(&op->node))
            if (can_carry_rest(c->other)) return true;
    }
    return false;
}

// The plan whose rows `op` adds to its input's columns, when that plan can
// return columns beyond its schema: then the op's output carries a rest
// column even when its input has none.
const LazyFrame* rest_partner(const LazyOp& op) {
    if (const auto* j = std::get_if<JoinOp>(&op.node))
        return j->how != JoinHow::Semi && j->how != JoinHow::Anti &&
                       j->how != JoinHow::Nest && can_carry_rest(j->other)
                   ? &j->other
                   : nullptr;
    if (const auto* c = std::get_if<ConcatOp>(&op.node))
        return can_carry_rest(c->other) ? &c->other : nullptr;
    return nullptr;
}

// The names after `op` over `in`, which ends in the rest column: the op's
// own output, then the rest column unless the op outputs one column per row
// (is_duplicated) or names its output only when it runs.
std::vector<std::string> rest_out_schema(const LazyOp& op,
                                         const std::vector<std::string>& in) {
    std::vector<std::string> out =
        out_schema(op, std::vector<std::string>(in.begin(), in.end() - 1));
    if (!out.empty() && !std::holds_alternative<IsDupOp>(op.node))
        out.emplace_back(REST_COLUMN);
    return out;
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
        // A source's join has the SQL rule for null keys; one that matches
        // nulls is run by the host.
        if (j->nulls_equal) return std::nullopt;
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
            if (sel || rest_rule(*op, {}) != RestRule::Omits)
                return std::nullopt;
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
// `own_rest`: the rest column of `sch` comes from the input, not from a
// partner plan of `op`.
std::unique_ptr<Cursor> make_cursor(const LazyOp& op,
                                    std::unique_ptr<Cursor> in,
                                    const std::vector<std::string>& sch,
                                    std::uint64_t budget, bool own_rest) {
    return std::visit(
        overloaded{
            [&](const FilterOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<FilterCursor>(std::move(in), o.pred);
            },
            [&](const SelectOp& o) -> std::unique_ptr<Cursor> {
                std::vector<int> idx;
                idx.reserve(o.names.size());
                for (const std::string& nm : without_rest(o.names))
                    idx.push_back(col_index(sch, nm));
                if (has_rest(sch) && o.names.size() != idx.size())
                    idx.push_back(static_cast<int>(sch.size()) - 1);
                return std::make_unique<SelectCursor>(std::move(in),
                                                      std::move(idx));
            },
            [&](const WithColumnOp& o) -> std::unique_ptr<Cursor> {
                const bool rest = has_rest(sch);
                return std::make_unique<WithColumnCursor>(
                    std::move(in), o.expr, col_index(sch, o.name),
                    sch.size() - (rest ? 1 : 0), rest);
            },
            [&](const RenameOp&) -> std::unique_ptr<Cursor> {
                return std::move(in);  // names-only; data passes through
            },
            [&](const DropOp& o) -> std::unique_ptr<Cursor> {
                const bool rest = has_rest(sch);
                std::vector<int> idx;
                for (std::size_t i = 0; i + (rest ? 1 : 0) < sch.size(); ++i)
                    if (std::find(o.names.begin(), o.names.end(), sch[i]) ==
                        o.names.end())
                        idx.push_back(static_cast<int>(i));
                if (!rest)
                    return std::make_unique<SelectCursor>(std::move(in),
                                                          std::move(idx));
                idx.push_back(static_cast<int>(sch.size()) - 1);
                return std::make_unique<SelectCursor>(
                    std::move(in), std::move(idx),
                    std::unordered_set<std::string>(o.names.begin(),
                                                    o.names.end()));
            },
            [&](const RenameColumnsOp& o) -> std::unique_ptr<Cursor> {
                if (!has_rest(sch)) return std::move(in);
                auto map = rename_map(o.from, o.to);
                std::vector<std::string> out = renamed_names(
                    std::vector<std::string>(sch.begin(), sch.end() - 1), map);
                return std::make_unique<RenameRestCursor>(
                    std::move(in), std::move(map),
                    std::unordered_set<std::string>(out.begin(), out.end()));
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
                return std::make_unique<FillNullCursor>(std::move(in), o.value,
                                                        has_rest(sch));
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
                    std::move(in), sch, std::vector<std::string>{o.name},
                    std::vector<bool>{o.descending}, budget);
            },
            [&](const SortByMultiOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<SortMergeCursor>(
                    std::move(in), sch, o.by, o.descending, budget);
            },
            [&](const ReverseOp&) -> std::unique_ptr<Cursor> {
                std::vector<std::string> numbered{"row_index"};
                numbered.insert(numbered.end(), sch.begin(), sch.end());
                std::vector<int> keep(sch.size());
                std::iota(keep.begin(), keep.end(), 1);
                return std::make_unique<SelectCursor>(
                    std::make_unique<SortMergeCursor>(
                        std::make_unique<WithRowIndexCursor>(std::move(in)),
                        std::move(numbered),
                        std::vector<std::string>{"row_index"},
                        std::vector<bool>{true}, budget),
                    std::move(keep));
            },
            [&](const TakeOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<TakeCursor>(std::move(in), o.indices);
            },
            [&](const FilterMaskOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<FilterMaskCursor>(std::move(in),
                                                          o.mask.share());
            },
            [&](const JoinOp& o) -> std::unique_ptr<Cursor> {
                // The right plan runs under the budget of this plan unless it
                // sets its own.
                LazyFrame other = o.other;
                if (budget != NO_SPILL_BUDGET && budget > 0 &&
                    detail::PlanAccess::memory_budget(other) == 0)
                    other = other.memory_budget(budget);
                return std::make_unique<JoinCursor>(
                    std::move(in), sch, o.left_fields, std::move(other),
                    o.left_on, o.right_on, o.how, o.suffix, budget,
                    o.nulls_equal, own_rest);
            },
            [&](const ConcatOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<ConcatCursor>(std::move(in), o.other,
                                                      has_rest(sch));
            },
            [&](const TapOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<TapCursor>(std::move(in), sch, o.tap);
            },
            [&](const UnnestOp& o) -> std::unique_ptr<Cursor> {
                return std::make_unique<UnnestCursor>(std::move(in), sch,
                                                      o.column, o.keep_empty);
            },
            [&](const FrameOp& o) -> std::unique_ptr<Cursor> {
                if (o.name == GROUP_TRANSFORM_OP) {
                    std::vector<std::size_t> keys, value;
                    for (const std::string& name : o.args->strings_at(1)) {
                        const int k = col_index(sch, name);
                        if (k < 0)
                            throw std::out_of_range(
                                "group_by: no column named " + name);
                        keys.push_back(static_cast<std::size_t>(k));
                    }
                    const auto kind =
                        static_cast<GroupwiseOp>(o.args->i32_at(2));
                    std::vector<TypeId> declared;
                    for (std::size_t i = 0; i < o.out_names.size(); ++i) {
                        declared.push_back(o.out_types[i].id);
                        if (kind == GroupwiseOp::CumCount) continue;
                        const int k = col_index(sch, o.out_names[i]);
                        if (k < 0)
                            throw std::out_of_range(
                                "group_by: no column named " + o.out_names[i]);
                        value.push_back(static_cast<std::size_t>(k));
                    }
                    return std::make_unique<NativeTransformCursor>(
                        std::move(in), sch, std::move(keys), kind,
                        o.args->i64_at(3), std::move(value),
                        std::move(declared), o.out_names, budget);
                }
                if (o.name == WINDOW_OP && budget != NO_SPILL_BUDGET &&
                    budget > 0) {
                    const std::vector<std::string>& part =
                        o.args->strings_at(1);
                    std::vector<std::string> sort_keys = part;
                    for (const std::string& name : o.args->strings_at(2))
                        sort_keys.push_back(name);
                    std::vector<std::size_t> idx;
                    for (const std::string& name : part) {
                        const int k = col_index(sch, name);
                        if (k < 0)
                            throw std::out_of_range("window: no column named " +
                                                    name);
                        idx.push_back(static_cast<std::size_t>(k));
                    }
                    for (const std::string& name : sort_keys)
                        if (col_index(sch, name) < 0)
                            throw std::out_of_range("window: no column named " +
                                                    name);
                    std::unique_ptr<Cursor> sorted = std::move(in);
                    if (!sort_keys.empty())
                        sorted = std::make_unique<SortMergeCursor>(
                            std::move(sorted), sch, sort_keys,
                            std::vector<bool>(sort_keys.size(), false), budget);
                    return std::make_unique<WindowStreamCursor>(
                        std::move(sorted), sch, std::move(idx),
                        window_specs(*o.args, sch), o.name, o.op, o.args,
                        budget / 4);
                }
                if (o.name == COMPARE_AGG_OP && budget != NO_SPILL_BUDGET &&
                    budget > 0 && o.args->frames().size() == 1) {
                    LazyFrame other = o.args->frames().front();
                    if (detail::PlanAccess::memory_budget(other) == 0)
                        other = other.memory_budget(budget);
                    return std::make_unique<CompareAggCursor>(
                        std::move(in), sch, std::move(other), o.args->i64_at(2),
                        budget);
                }
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
            [&](const HeadByOp& o) -> std::unique_ptr<Cursor> {
                std::vector<std::int64_t> key_idx;
                key_idx.reserve(o.keys.size());
                for (const std::string& name : o.keys) {
                    const int k = col_index(sch, name);
                    if (k < 0)
                        throw std::out_of_range("head_by: no column named " +
                                                name);
                    key_idx.push_back(k);
                }
                return std::make_unique<HeadByCursor>(
                    std::move(in), std::move(key_idx), o.n, budget);
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
                [&](const SelectOp& o) {
                    return df.select(without_rest(o.names));
                },
                [&](const RenameOp& o) { return df.rename(o.names); },
                [&](const DropOp& o) {
                    return df.select(dropped_names(df.names, o.names));
                },
                [&](const RenameColumnsOp& o) {
                    return df.rename(
                        renamed_names(df.names, rename_map(o.from, o.to)));
                },
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

LazyFrame LazyFrame::drop(std::vector<std::string> names) const {
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(LazyOp{DropOp{std::move(names)}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::rename_columns(std::vector<std::string> from,
                                    std::vector<std::string> to) const {
    if (from.size() != to.size())
        throw std::invalid_argument(
            "rename_columns: from and to differ in length");
    auto ops = ops_;
    ops.push_back(std::make_shared<LazyOp>(
        LazyOp{RenameColumnsOp{std::move(from), std::move(to)}}));
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

LazyFrame LazyFrame::head_by(std::vector<std::string> keys,
                             std::int64_t n) const {
    const std::vector<std::string> names = schema();
    if (!names.empty())
        for (const std::string& k : keys)
            if (col_index(names, k) < 0)
                throw std::out_of_range("head_by: no column named " + k);
    auto ops = ops_;
    ops.push_back(
        std::make_shared<LazyOp>(LazyOp{HeadByOp{std::move(keys), n}}));
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
                          std::string suffix, bool nulls_equal) const {
    if (!valid_join_how(how))
        throw std::invalid_argument("join: unknown join kind " +
                                    std::to_string(static_cast<int>(how)));
    if (nulls_equal && (how == JoinHow::Cross || how == JoinHow::Lookup ||
                        how == JoinHow::Nest))
        throw std::invalid_argument(
            std::string("join: nulls_equal does not apply to a ") +
            join_how_name(how) + " join");
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
    ops.push_back(std::make_shared<LazyOp>(LazyOp{
        JoinOp{std::move(other), std::move(left_on), std::move(right_on), how,
               std::move(suffix), std::move(left_fields), nulls_equal}}));
    return with_ops(std::move(ops));
}

LazyFrame LazyFrame::join(LazyFrame other, std::vector<std::string> on,
                          JoinHow how, std::string suffix,
                          bool nulls_equal) const {
    std::vector<std::string> right_on = on;
    return join(std::move(other), std::move(on), std::move(right_on), how,
                std::move(suffix), nulls_equal);
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
    ops.push_back(std::make_shared<LazyOp>(LazyOp{FrameOp{
        std::move(name), op, std::move(owned), std::move(out_names), {}}}));
    return with_ops(std::move(ops));
}

std::optional<LazyFrame> native_group_transform(
    const LazyFrame& plan, const std::vector<std::string>& keys,
    GroupwiseOp kind, std::int64_t n, const Schema& typed) {
    switch (kind) {
        case GroupwiseOp::CumSum:
        case GroupwiseOp::CumMax:
        case GroupwiseOp::CumMin:
        case GroupwiseOp::CumProd:
        case GroupwiseOp::CumCount:
        case GroupwiseOp::Diff:
        case GroupwiseOp::FFill:
            break;
        case GroupwiseOp::Shift:
            if (n < 0 || n > NATIVE_SHIFT_MAX) return std::nullopt;
            break;
        default:
            return std::nullopt;
    }
    const Schema in = plan.output_schema();
    if (in.fields.empty() || typed.fields.empty()) return std::nullopt;
    for (const Field& f : in.fields)
        if (f.type.id == TypeId::Unknown || f.name == REST_COLUMN)
            return std::nullopt;
    for (const std::string& k : keys)
        if (!find_field(in, k)) return std::nullopt;
    std::vector<std::string> out_names;
    std::vector<DataType> out_types;
    for (const Field& f : typed.fields) {
        NumClass cls = NumClass::SIGNED;
        if (!num_class(f.type.id, cls)) return std::nullopt;
        if (kind != GroupwiseOp::CumCount) {
            const Field* src = find_field(in, f.name);
            if (!src || !num_class(src->type.id, cls)) return std::nullopt;
        }
        out_names.push_back(f.name);
        out_types.push_back(f.type);
    }
    const dftu_op_desc* op = dftu_op_find(GROUP_TRANSFORM_OP);
    if (!op) return std::nullopt;
    std::vector<const char*> key_ptrs;
    for (const std::string& k : keys) key_ptrs.push_back(k.c_str());
    OpArgs args;
    args.strlist(1, key_ptrs.data(), static_cast<std::int32_t>(key_ptrs.size()))
        .i32(2, static_cast<std::int32_t>(kind))
        .i64(3, n)
        .i32(4, 0)
        .i32(5, 1);
    auto owned = std::make_shared<const OwnedFrameOpArgs>(
        *op, args, std::vector<LazyFrame>{});
    auto ops = detail::PlanAccess::ops(plan);
    ops.push_back(std::make_shared<LazyOp>(
        LazyOp{FrameOp{GROUP_TRANSFORM_OP, op, std::move(owned),
                       std::move(out_names), std::move(out_types)}}));
    return detail::PlanAccess::make(detail::PlanAccess::source_ptr(plan),
                                    std::move(ops),
                                    detail::PlanAccess::memory_budget(plan));
}

LazyFrame LazyFrame::compare_agg(LazyFrame variant, std::int64_t n_key) const {
    if (n_key < 1) throw std::invalid_argument("compare_agg: n_key < 1");
    const std::vector<Field> lhs = output_schema().fields;
    const std::vector<Field> rhs = variant.output_schema().fields;
    const auto nk = static_cast<std::size_t>(n_key);
    auto typed = [](const std::vector<Field>& fs) {
        return !fs.empty() &&
               std::none_of(fs.begin(), fs.end(), [](const Field& f) {
                   return f.type.id == TypeId::Unknown;
               });
    };
    // A plan whose columns or types are only known after it runs cannot name
    // its delta columns here, so it is collected as the eager op does.
    if (!typed(lhs) || !typed(rhs)) {
        OpArgs args;
        args.i64(2, n_key);
        return frame_op("dftu.frame.compare_agg", args, {std::move(variant)});
    }
    return compose_compare_agg(*this, variant, lhs, rhs, nk);
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
        // String and LargeString concatenate (the eager kernel widens).
        const auto text = [](TypeId x) {
            return x == TypeId::String || x == TypeId::LargeString;
        };
        if (lhs[i].type.id != TypeId::Unknown &&
            rhs[i].type.id != TypeId::Unknown && lhs[i].type != rhs[i].type &&
            !(text(lhs[i].type.id) && text(rhs[i].type.id)))
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
        if (const auto* s = std::get_if<SelectOp>(&ops.front()->node);
            s && rest_rule(*ops.front(), {}) == RestRule::Omits) {
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
    if (!chain.schema.empty() &&
        std::find(drop.begin(), drop.end(), false) != drop.end()) {
        const Schema declared = source.schema();
        std::vector<Field> fields;
        fields.reserve(chain.schema.size());
        for (const std::string& name : chain.schema) {
            const Field* f = find_field(declared, name);
            fields.push_back(f ? *f
                               : Field{name, scalar(TypeId::Unknown), true});
        }
        const bool rest = projection.empty() && source.undeclared_columns();
        chain.cursor = std::make_unique<AlignCursor>(std::move(chain.cursor),
                                                     std::move(fields), rest);
        chain.cursor->attach(chain.registry);
        if (rest) chain.schema.emplace_back(REST_COLUMN);
    }
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (drop[i]) continue;
        const bool own_rest = has_rest(chain.schema);
        if (!chain.schema.empty() && !own_rest && rest_partner(*ops[i])) {
            std::vector<Field> fields;
            for (const std::string& name : chain.schema)
                fields.push_back(Field{name, scalar(TypeId::Unknown), true});
            chain.cursor = std::make_unique<AlignCursor>(
                std::move(chain.cursor), std::move(fields), true);
            chain.cursor->attach(chain.registry);
            chain.schema.emplace_back(REST_COLUMN);
        }
        const bool rest = has_rest(chain.schema);
        const RestRule rule =
            rest ? rest_rule(*ops[i], chain.schema) : RestRule::Moves;
        if (rule == RestRule::None)
            throw std::invalid_argument(
                "'" + describe_op(*ops[i]) +
                "' has no rule for the columns the scan returns beyond the "
                "plan's schema; select the columns it needs first");
        const auto* group = std::get_if<GroupByOp>(&ops[i]->node);
        if (rule == RestRule::Omits && !(group && !group->dyn.empty())) {
            std::vector<int> idx(chain.schema.size() - 1);
            std::iota(idx.begin(), idx.end(), 0);
            chain.cursor = std::make_unique<SelectCursor>(
                std::move(chain.cursor), std::move(idx));
            chain.cursor->attach(chain.registry);
            chain.schema.pop_back();
        }
        chain.cursor = make_cursor(*ops[i], std::move(chain.cursor),
                                   chain.schema, budget, own_rest);
        chain.schema = has_rest(chain.schema)
                           ? rest_out_schema(*ops[i], chain.schema)
                           : out_schema(*ops[i], std::move(chain.schema));
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

namespace {

CursorChain open_chain(const LazyFrame& lf) {
    PlanParts plan = optimize_parts(detail::PlanAccess::source_ptr(lf),
                                    detail::PlanAccess::ops(lf));
    return lower_cursor_chain(
        std::move(plan.source), plan.ops,
        resolve_spill_budget(detail::PlanAccess::memory_budget(lf)));
}

std::unique_ptr<Cursor> without_rest_column(CursorChain chain) {
    if (!has_rest(chain.schema)) return std::move(chain.cursor);
    std::vector<int> idx(chain.schema.size() - 1);
    std::iota(idx.begin(), idx.end(), 0);
    return std::make_unique<SelectCursor>(std::move(chain.cursor),
                                          std::move(idx));
}

std::unique_ptr<Cursor> concat_side(const LazyFrame& other, bool rest) {
    CursorChain chain = open_chain(other);
    if (!rest) return without_rest_column(std::move(chain));
    if (has_rest(chain.schema)) return std::move(chain.cursor);
    return std::make_unique<AlignCursor>(std::move(chain.cursor),
                                         other.output_schema().fields, true);
}

}  // namespace

std::unique_ptr<Cursor> LazyFrame::open_cursor() const {
    return without_rest_column(open_chain(*this));
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
    const std::uint64_t budget = resolve_spill_budget(memory_budget);
    co_return co_await drain_stream(
        stream_plan(std::move(plan), memory_budget, morsel_rows), budget);
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
    OpenedSource(Schema schema, std::unique_ptr<Cursor> cursor, bool undeclared)
        : schema_(std::move(schema)),
          cursor_(std::move(cursor)),
          undeclared_(undeclared) {}

    Schema schema() const override { return schema_; }
    bool undeclared_columns() const override { return undeclared_; }

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
    bool undeclared_;
};

// A frame a source with undeclared columns collected for a batch: the
// source's schema, and morsels named by the frame's columns, so the columns
// past that schema reach the plan as they would from the source's own scan.
class CollectedSource final : public Source {
   public:
    CollectedSource(Schema schema, DataFrame frame)
        : schema_(std::move(schema)),
          frame_(std::make_shared<const DataFrame>(std::move(frame))) {
        for (const std::string& n : frame_->names)
            ids_.push_back(intern_->get_or_insert(n));
    }

    Schema schema() const override { return schema_; }
    bool undeclared_columns() const override { return true; }

    ScanResult scan(const ScanRequest& req) const override {
        ScanResult r;
        r.filters.assign(req.filters.size(), Pushed::No);
        r.cursor = std::make_unique<Named>(
            std::make_unique<InMemoryCursor>(frame_), ids_, intern_);
        return r;
    }

   private:
    class Named final : public Cursor {
       public:
        Named(std::unique_ptr<Cursor> in, std::vector<std::uint32_t> ids,
              std::shared_ptr<dftracer::utils::StringIntern> intern)
            : in_(std::move(in)),
              ids_(std::move(ids)),
              intern_(std::move(intern)) {}

        coro::CoroTask<std::optional<Morsel>> next(
            std::int64_t max_rows) override {
            auto m = co_await in_->next(max_rows);
            if (m) {
                m->dyn_state().name_ids = ids_;
                m->dyn->intern = intern_;
            }
            co_return m;
        }

       private:
        std::unique_ptr<Cursor> in_;
        std::vector<std::uint32_t> ids_;
        std::shared_ptr<dftracer::utils::StringIntern> intern_;
    };

    Schema schema_;
    std::shared_ptr<const DataFrame> frame_;
    std::shared_ptr<dftracer::utils::StringIntern> intern_ =
        std::make_shared<dftracer::utils::StringIntern>();
    std::vector<std::uint32_t> ids_;
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
                        sources[k]->schema(), std::move((*cursors)[k]),
                        sources[k]->undeclared_columns());
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
                sources[k]->undeclared_columns()
                    ? std::shared_ptr<const Source>(
                          std::make_shared<CollectedSource>(
                              sources[k]->schema(), std::move(frames[k])))
                    : std::make_shared<InMemorySource>(std::move(frames[k]));
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

coro::CoroTask<DataFrame> join_chunks(DataFrame f) {
    co_await each_column(f.columns.size(), [&f](std::size_t c) {
        if (f.columns[c].encoding() == Encoding::Chunked)
            f.columns[c] = join_chunks(f.columns[c]);
    });
    co_return f;
}

coro::CoroTask<DataFrame> LazyFrame::collect(std::int64_t morsel_rows) const {
    return collect_owned(*this, memory_budget_, morsel_rows);
}

coro::CoroTask<std::vector<DataFrame>> Source::collect_batch(
    std::vector<std::shared_ptr<const Source>>) const {
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
    std::int64_t rows_seen = 0;
    while (auto df = co_await gen.next()) {
        agg_set_row_base(*state, rows_seen);
        rows_seen += df->num_rows();
        agg_accumulate_chunk(*state, *df, keys, lowered.value_names,
                             dyn.empty() ? std::string() : dyn_prefix);
    }
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
            [&](const DropOp& o) { fp.strs(o.names); },
            [&](const RenameColumnsOp& o) {
                fp.strs(o.from);
                fp.strs(o.to);
            },
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
            [&](const HeadByOp& o) {
                fp.strs(o.keys);
                fp.pod(o.n);
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
                fp.pod(m && m->data ? m->data->data() : nullptr);
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
                fp.pod(o.nulls_equal);
                fp.pod(static_cast<std::uint64_t>(o.left_fields.size()));
                for (const Field& f : o.left_fields) {
                    fp.str(f.name);
                    fp.pod(f.type.id);
                }
            },
            [&](const ConcatOp& o) { fp.pod(plan_fingerprint(o.other)); },
            [&](const TapOp& o) {
                fp.pod(static_cast<const void*>(o.tap.get()));
            },
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

LazyFrame tap(const LazyFrame& lf, std::shared_ptr<const Tap> tap) {
    auto ops = PlanAccess::ops(lf);
    ops.push_back(std::make_shared<LazyOp>(LazyOp{TapOp{std::move(tap)}}));
    return PlanAccess::make(PlanAccess::source_ptr(lf), std::move(ops),
                            PlanAccess::memory_budget(lf));
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
