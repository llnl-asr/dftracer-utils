#ifndef DFTRACER_UTILS_DATAFRAME_LAZY_COMMON_H
#define DFTRACER_UTILS_DATAFRAME_LAZY_COMMON_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/hash/hash.h>
#include <dftracer/utils/core/common/hash/partition.h>
#include <dftracer/utils/core/common/int128.h>
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
#include <dftracer/utils/dataframe/internal/budget_share.h>
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

namespace dftracer::utils::dataframe::lazy_internal {

inline std::vector<const Series*> column_ptrs(const std::vector<Series>& cols) {
    std::vector<const Series*> in;
    in.reserve(cols.size());
    for (const Series& c : cols) in.push_back(&c);
    return in;
}

inline int column_index_of(const std::vector<std::string>& names,
                           const std::string& name) {
    auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? -1 : static_cast<int>(it - names.begin());
}

// The index of column `name` of `sch`. A name that is not there is the error of
// the op `op`: std::out_of_range "<op>: no column named <name>".
inline int require_col(const std::vector<std::string>& sch,
                       const std::string& name, const char* op) {
    const int at = column_index_of(sch, name);
    if (at < 0)
        throw std::out_of_range(std::string(op) + ": no column named " + name);
    return at;
}

// An all-null column of `rows` rows of the type of `f`, with the type's
// parameters. Refuses a type it cannot rebuild rather than emit a column that
// has silently lost them; `why` says what has no rows to take the type from.
inline Series null_column_of(const Field& f, std::int64_t rows, const char* who,
                             const char* why) {
    const DataType& dt = f.type;
    const bool rebuildable =
        dt.fields.empty() &&
        (byte_width(dt.id).has_value() || dt.id == TypeId::String ||
         dt.id == TypeId::Binary);
    if (!rebuildable)
        throw std::runtime_error(std::string(who) +
                                 ": cannot build a null column of type " +
                                 std::string(type_name(dt.id)) +
                                 " for column '" + f.name + "' when " + why);
    Series s = Series::nulls(dt.id, rows);
    dftu_series* h = s.handle();
    h->set_time_unit(dt.time_unit());
    h->set_timezone(dt.timezone());
    h->set_decimal(dt.decimal_precision(), dt.decimal_scale());
    h->set_fixed_size(dt.fixed_size());
    return s;
}

// A frame over the columns of a morsel, named `names` or, with none, unnamed.
inline DataFrame frame_of(std::vector<Series>&& columns,
                          std::vector<std::string> names = {}) {
    DataFrame out;
    out.names = names.empty() ? std::vector<std::string>(columns.size())
                              : std::move(names);
    out.columns = std::move(columns);
    return out;
}

// The rows [off, off + len) of a morsel, also for list and struct columns.
inline Morsel slice_morsel(const Morsel& m, std::int64_t off,
                           std::int64_t len) {
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

// ---- output morsels within the budget ----

// Whether `budget` limits anything: it does not when it is 0 (unset) or
// NO_SPILL_BUDGET.
inline bool has_cap(std::uint64_t budget) {
    return budget != NO_SPILL_BUDGET && budget > 0;
}

// The bytes one output morsel may take of `budget`.
inline std::uint64_t output_share(std::uint64_t budget) {
    return has_cap(budget) ? std::max<std::uint64_t>(budget / share::OUTPUT, 1)
                           : NO_SPILL_BUDGET;
}

// How many rows of `bytes_per_row` bytes fit `byte_share`, between 1 and
// `max_rows`. A row wider than the share still gets one row.
inline std::int64_t rows_within(std::uint64_t byte_share,
                                std::uint64_t bytes_per_row,
                                std::int64_t max_rows) {
    if (byte_share == NO_SPILL_BUDGET) return max_rows;
    const std::uint64_t fit =
        byte_share / std::max<std::uint64_t>(bytes_per_row, 1);
    return std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::min<std::uint64_t>(fit, max_rows)), 1,
        max_rows);
}

// Hands an input morsel to an op in slices, so the op decides how many rows
// to take at once. A slice shares the morsel's buffers.
class MorselFeed {
   public:
    // True when a row is left to take; reads the next morsel when the last
    // one is used up.
    coro::CoroTask<bool> fill(Cursor& in, std::int64_t max_rows) {
        while (!cur_ || pos_ >= cur_->rows) {
            cur_ = co_await in.next(max_rows);
            pos_ = 0;
            if (!cur_) co_return false;
        }
        co_return true;
    }
    const Morsel& current() const { return *cur_; }
    std::int64_t position() const { return pos_; }
    std::int64_t remaining() const { return cur_ ? cur_->rows - pos_ : 0; }

    // The next `n` rows (at most the rows left).
    Morsel take(std::int64_t n) {
        n = std::min(n, remaining());
        Morsel out = slice_morsel(*cur_, pos_, n);
        if (cur_->dyn) out.dyn = std::make_unique<MorselDyn>(*cur_->dyn);
        out.ordering = cur_->ordering;
        out.ordered_column = cur_->ordered_column;
        out.ordered_descending = cur_->ordered_descending;
        pos_ += n;
        return out;
    }

   private:
    std::optional<Morsel> cur_;
    std::int64_t pos_ = 0;
};

// Cuts one finished result into morsels of at most `max_rows` rows and
// `byte_share` bytes. The slices share the result's buffers. A result with no
// rows still leaves as one morsel, because its columns carry the names and
// types that a consumer builds the empty table from.
class ResultChunks {
   public:
    void set(Morsel result) {
        rest_ = std::move(result);
        pos_ = 0;
        pending_ = true;
        bytes_per_row_ = rest_.rows > 0
                             ? spill::columns_bytes(rest_.columns) /
                                   static_cast<std::uint64_t>(rest_.rows)
                             : 0;
    }
    bool empty() const { return !pending_; }
    Morsel next(std::int64_t max_rows, std::uint64_t byte_share) {
        const std::int64_t n =
            std::min(rest_.rows - pos_,
                     rows_within(byte_share, bytes_per_row_, max_rows));
        if (pos_ == 0 && n == rest_.rows) {
            pos_ = n;
            pending_ = false;
            return std::move(rest_);
        }
        Morsel out = slice_morsel(rest_, pos_, n);
        // The names of a dynamic schema and the order claim go with each slice.
        if (rest_.dyn) out.dyn = std::make_unique<MorselDyn>(*rest_.dyn);
        out.ordering = rest_.ordering;
        out.ordered_column = rest_.ordered_column;
        out.ordered_descending = rest_.ordered_descending;
        pos_ += n;
        pending_ = pos_ < rest_.rows;
        return out;
    }

   private:
    Morsel rest_;
    std::int64_t pos_ = 0;
    std::uint64_t bytes_per_row_ = 0;
    bool pending_ = false;
};

// How many input rows to hand an op whose output can be larger than its
// input, from what the last calls made: output rows per input row, and bytes
// per output row. It starts small, so the first call of an op that multiplies
// its input cannot be a large one.
class ExpansionGovernor {
   public:
    explicit ExpansionGovernor(std::uint64_t byte_share) : share_(byte_share) {}

    std::int64_t rows(std::int64_t max_rows) const {
        if (share_ == NO_SPILL_BUDGET) return max_rows;
        const double out_rows =
            static_cast<double>(share_) / static_cast<double>(row_bytes_);
        const double in_rows = out_rows / std::max(expansion_, 1.0);
        const std::int64_t k = static_cast<std::int64_t>(
            std::min<double>(in_rows, static_cast<double>(max_rows)));
        return std::clamp<std::int64_t>(
            seen_ ? k : std::min<std::int64_t>(k, FIRST_ROWS), 1, max_rows);
    }

    // What an op is known to make before it has run: `expansion` output rows
    // of `row_bytes` bytes per input row.
    void seed(double expansion, std::uint64_t row_bytes) {
        expansion_ = expansion;
        row_bytes_ = std::max<std::uint64_t>(row_bytes, 1);
        seen_ = true;
    }

    void observe(std::int64_t in_rows, std::int64_t out_rows,
                 std::uint64_t out_bytes) {
        if (in_rows <= 0) return;
        const double e =
            static_cast<double>(out_rows) / static_cast<double>(in_rows);
        expansion_ = seen_ ? std::max(e, expansion_ * 0.5) : e;
        if (out_rows > 0)
            row_bytes_ = std::max<std::uint64_t>(
                out_bytes / static_cast<std::uint64_t>(out_rows), 1);
        seen_ = true;
    }

   private:
    static constexpr std::int64_t FIRST_ROWS = 8;
    std::uint64_t share_;
    double expansion_ = 1.0;
    std::uint64_t row_bytes_ = 64;
    bool seen_ = false;
};

// The columns `idx` of `cols`, flat, for row keys and comparisons.
inline std::vector<Series> key_columns(const std::vector<Series>& cols,
                                       const std::vector<std::size_t>& idx) {
    std::vector<Series> out;
    out.reserve(idx.size());
    for (std::size_t c : idx)
        out.push_back(cols[c].is_flat() ? cols[c].share()
                                        : cols[c].materialize());
    return out;
}

// Default scan chunk when the caller does not set one (morsel_rows <= 0).
inline constexpr std::int64_t DEFAULT_MORSEL_ROWS = 65536;

inline bool has_rest(const std::vector<std::string>& sch) {
    return !sch.empty() && sch.back() == REST_COLUMN;
}

// The fields of a rest column as named columns, flat.
inline DataFrame rest_fields(const Series& rest) {
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
inline DataFrame rest_to_back(DataFrame f) {
    const int at = column_index_of(f.names, std::string(REST_COLUMN));
    if (at < 0 || static_cast<std::size_t>(at) + 1 == f.names.size()) return f;
    std::rotate(f.names.begin() + at, f.names.begin() + at + 1, f.names.end());
    std::rotate(f.columns.begin() + at, f.columns.begin() + at + 1,
                f.columns.end());
    return f;
}

inline Morsel morsel_of(DataFrame&& f) {
    Morsel out;
    out.rows = f.num_rows();
    out.columns.reserve(f.columns.size());
    for (const Series& c : f.columns) out.columns.push_back(c.materialize());
    return out;
}

// ---- cursors ----------------------------------------------------------------

inline constexpr std::size_t MAX_ACCUMULATE_BATCH = 32;

// How many morsels to accumulate at once into separate partial states: the
// partials together stay within a quarter of the budget. `partial_bytes` is the
// measured size of one (agg_approx_bytes leaves out the group index, so it is
// doubled); 0 means not measured yet.
inline std::size_t accumulate_batch(std::uint64_t budget,
                                    std::uint64_t partial_bytes) {
    if (partial_bytes == 0) return 2;
    if (budget == NO_SPILL_BUDGET) return MAX_ACCUMULATE_BATCH;
    return std::clamp<std::size_t>(budget / share::CHUNK / (2 * partial_bytes),
                                   1, MAX_ACCUMULATE_BATCH);
}

// Pulls morsels in batches (a bounded parallel sink), folds each batch into
// `state`, and calls `after(batch, state)` once per batch. A batch of one folds
// straight into `state`; a larger one is folded morsel by morsel into partial
// states in parallel (the mergeable agg IR) that are then merged, so the batch
// size follows the size of a partial and memory stays near the budget.
// `accumulate(state, morsel, row_base)` folds one morsel, `make()` makes a
// fresh state, and `active()` says whether folding is still wanted (a caller
// that gave up on `state` only wants its batches).
template <class Make, class Accumulate, class After, class Active>
inline coro::CoroTask<void> accumulate_batches(
    Cursor& in, std::int64_t max_rows, std::uint64_t budget, AggStatePtr& state,
    Make make, Accumulate accumulate, After after, Active active) {
    std::size_t cap = accumulate_batch(budget, 0);
    std::vector<Morsel> batch;
    batch.reserve(MAX_ACCUMULATE_BATCH);
    std::vector<std::int64_t> row_base;
    std::int64_t rows_seen = 0;
    bool eof = false;
    while (!eof) {
        batch.clear();
        for (std::size_t b = 0; b < cap; ++b) {
            auto m = co_await in.next(max_rows);
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
        if (active()) {
            if (batch.size() == 1) {
                accumulate(*state, batch[0], row_base[0]);
            } else {
                std::vector<AggStatePtr> partials(batch.size());
                parallel_for(static_cast<std::int64_t>(batch.size()), 1,
                             [&](std::int64_t bi, std::int64_t ei) {
                                 for (std::int64_t j = bi; j < ei; ++j) {
                                     const auto z = static_cast<std::size_t>(j);
                                     AggStatePtr st = make();
                                     accumulate(*st, batch[z], row_base[z]);
                                     partials[z] = std::move(st);
                                 }
                             });
                std::uint64_t partial_bytes = 0;
                for (auto& p : partials)
                    if (p) partial_bytes += agg_approx_bytes(*p);
                cap = accumulate_batch(budget, partial_bytes / partials.size());
                for (auto& p : partials)
                    if (p) agg_merge(*state, *p);
            }
        }
        after(batch, state);
    }
}

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

}  // namespace dftracer::utils::dataframe::lazy_internal

#endif  // DFTRACER_UTILS_DATAFRAME_LAZY_COMMON_H
