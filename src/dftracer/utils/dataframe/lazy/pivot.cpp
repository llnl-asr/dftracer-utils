#include <dftracer/utils/dataframe/lazy/cursors.h>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace {

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
          spool_(budget / share::SPOOL),
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
            budget_ == 0
                ? std::int64_t{1} << 20
                : std::max<std::int64_t>(
                      1, static_cast<std::int64_t>(budget_ / share::CHUNK) /
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
        const int ii = require_col(sch_, index_, "pivot");
        const int ci = require_col(sch_, on_, "pivot");
        const int vi = require_col(sch_, values_, "pivot");

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
        auto grouped = make_group_by(
            spool_.reader(), std::vector<std::string>{i_name, o_name, v_name},
            std::vector<std::string>{i_name, o_name},
            std::vector<GroupAgg>{GroupAgg{agg_from_string(agg_), v_name, "v"}},
            budget_ / share::SPOOL);
        sorted_ = make_sort_merge(
            std::move(grouped), std::vector<std::string>{i_name, o_name, "v"},
            std::vector<std::string>{i_name}, std::vector<bool>{false},
            budget_ / share::SPOOL);
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

}  // namespace

namespace lazy_internal {

std::unique_ptr<Cursor> make_pivot(std::unique_ptr<Cursor> in,
                                   std::uint64_t budget,
                                   std::vector<std::string> sch,
                                   std::string index, std::string on,
                                   std::string values, std::string agg) {
    return std::make_unique<PivotCursor>(std::move(in), budget, std::move(sch),
                                         std::move(index), std::move(on),
                                         std::move(values), std::move(agg));
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
