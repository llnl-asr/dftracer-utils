#include <dftracer/utils/dataframe/kernels/order.h>
#include <dftracer/utils/dataframe/lazy/cursors.h>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace {

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
        return compare_doubles(floats[a], o.floats[b]);
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

// A k-way merge of sorted runs by the key columns `key_idx` (each ascending or
// descending, a null last in both directions). On a tie the earlier run comes
// first, so the merge is stable. It holds one morsel per run.
class RunMerger {
   public:
    RunMerger(std::vector<int> key_idx, std::vector<bool> descending)
        : key_idx_(std::move(key_idx)), descending_(std::move(descending)) {}

    void add_run(std::unique_ptr<Cursor> run) {
        runs_.push_back(std::move(run));
    }

    // Reads the first morsel of every run.
    coro::CoroTask<void> open(std::int64_t max_rows) {
        cur_.resize(runs_.size());
        pos_.assign(runs_.size(), 0);
        run_keys_.resize(runs_.size());
        for (std::size_t k = 0; k < runs_.size(); ++k)
            load(k, co_await runs_[k]->next(max_rows));
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) {
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

    std::vector<int> key_idx_;
    std::vector<bool> descending_;
    std::vector<std::unique_ptr<Cursor>> runs_;
    std::vector<std::optional<Morsel>> cur_;
    std::vector<std::vector<KeyData>> run_keys_;
    std::vector<std::int64_t> pos_;
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

        if (merger_) co_return co_await merger_->next(max_rows);
        if (empty_) {
            std::optional<Morsel> m = std::move(empty_);
            empty_.reset();
            co_return m;
        }
        auto m = co_await single_->next(max_rows);
        if (m) {
            m->ordering = Ordering::ByColumn;
            m->ordered_column = key_idx_.front();
            m->ordered_descending = descending_.front();
        }
        co_return m;
    }

   private:
    DataFrame sort_rows(DataFrame df) const {
        return keys_.size() == 1
                   ? df.sort_by(keys_.front(), descending_.front())
                   : df.sort_by_multi(keys_, descending_);
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
            run_rows_ = chunk;
        }
        run_morsel_bytes_ =
            std::max(run_morsel_bytes_,
                     spill::write_run(w, sorted.columns, total, chunk));
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
        {
            RunMerger merger(key_idx_, descending_);
            for (int id : ids)
                merger.add_run(
                    std::make_unique<spill::Reader>(dir_.run_path(id)));
            co_await merger.open(max_rows);
            spill::Writer w(dir_.run_path(out_id));
            // The merged run keeps the morsel size of the runs it replaces, so
            // the fan-in chosen for the first pass holds for every later one.
            const std::int64_t rows = run_rows_ > 0 ? run_rows_ : max_rows;
            while (auto m = co_await merger.next(rows)) {
                run_morsel_bytes_ = std::max<std::uint64_t>(
                    run_morsel_bytes_, spill::columns_bytes(m->columns));
                w.write(m->columns, m->rows);
            }
            w.close();
        }
        std::error_code ec;
        for (int id : ids) fs::remove(dir_.run_path(id), ec);
        co_return out_id;
    }

    coro::CoroTask<void> build(std::int64_t max_rows) {
        for (const std::string& key : keys_)
            key_idx_.push_back(require_col(sch_, key, "sort_by"));

        std::vector<std::unique_ptr<Cursor>> runs;
        std::vector<std::vector<Series>> pending;
        std::size_t pend_bytes = 0;
        int run_id = 0;
        while (auto m = co_await in_->next(max_rows)) {
            pend_bytes += spill::columns_bytes(m->columns);
            pending.push_back(std::move(m->columns));
            if (budget_ > 0 && pend_bytes > budget_ / share::SORT_RUN) {
                spill_run(pending, run_id++, max_rows);
                pend_bytes = 0;
            }
        }

        if (run_id == 0) {  // everything fits in memory: one sorted run
            DataFrame buf =
                pending.empty() ? DataFrame{} : concat_pending(pending);
            if (pending.empty()) buf.names = sch_;
            // An input that sent only rowless morsels (a group-by of no rows)
            // still hands its typed columns on.
            if (!pending.empty() && buf.num_rows() == 0) {
                empty_.emplace();
                empty_->columns = std::move(buf.columns);
            }
            DataFrame sorted =
                buf.num_rows() ? sort_rows(std::move(buf)) : std::move(buf);
            runs.push_back(std::make_unique<InMemoryCursor>(
                std::make_shared<const DataFrame>(std::move(sorted))));
        } else {
            if (!pending.empty()) spill_run(pending, run_id++, max_rows);
            std::vector<int> ids(static_cast<std::size_t>(run_id));
            std::iota(ids.begin(), ids.end(), 0);
            while (ids.size() > merge_fan_in()) {
                const std::size_t fan_in = merge_fan_in();
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
                runs.push_back(
                    std::make_unique<spill::Reader>(dir_.run_path(id)));
        }
        if (runs.size() == 1) {
            single_ = std::move(runs.front());
        } else {
            merger_.emplace(key_idx_, descending_);
            for (std::unique_ptr<Cursor>& r : runs)
                merger_->add_run(std::move(r));
            co_await merger_->open(max_rows);
        }
        built_ = true;
    }

    std::uint64_t run_morsel_bytes_ = 0;
    std::int64_t run_rows_ = 0;
    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::vector<std::string> keys_;
    std::vector<bool> descending_;
    std::uint64_t budget_;
    bool built_ = false;
    std::vector<int> key_idx_;
    spill::Dir dir_;
    std::optional<RunMerger> merger_;
    std::unique_ptr<Cursor> single_;
    std::optional<Morsel> empty_;
};

}  // namespace

namespace lazy_internal {

std::unique_ptr<Cursor> make_sort_merge(std::unique_ptr<Cursor> in,
                                        std::vector<std::string> sch,
                                        std::vector<std::string> keys,
                                        std::vector<bool> descending,
                                        std::uint64_t budget) {
    return std::make_unique<SortMergeCursor>(std::move(in), std::move(sch),
                                             std::move(keys),
                                             std::move(descending), budget);
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
