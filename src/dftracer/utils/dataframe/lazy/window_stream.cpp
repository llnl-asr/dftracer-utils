#include <dftracer/utils/dataframe/kernels/order.h>
#include <dftracer/utils/dataframe/kernels/running.h>
#include <dftracer/utils/dataframe/lazy/cursors.h>
#include <dftracer/utils/dataframe/lazy/numeric.h>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace {

// The order column as doubles, and whether each value is missing (null or NaN,
// which sort last).
void order_values(const Series& col, std::vector<double>& ov,
                  std::vector<char>& miss) {
    NumClass cls = NumClass::SIGNED;
    if (!num_class(col.type(), cls))
        throw std::logic_error("window: a non-numeric order column");
    const NumCells cells = read_cells(col);
    const std::size_t n = cells.bits.size();
    ov.resize(n);
    miss.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        double v = 0.0;
        switch (cls) {
            case NumClass::SIGNED:
                v = static_cast<double>(
                    static_cast<std::int64_t>(cells.bits[i]));
                break;
            case NumClass::UNSIGNED:
                v = static_cast<double>(cells.bits[i]);
                break;
            case NumClass::FLOAT:
                v = bits_double(cells.bits[i]);
                break;
        }
        ov[i] = v;
        miss[i] = (!cells.ok[i] || v != v) ? 1 : 0;
    }
}

double cell_double(NumClass cls, std::uint64_t bits) {
    return cls == NumClass::FLOAT
               ? bits_double(bits)
               : (cls == NumClass::SIGNED
                      ? static_cast<double>(static_cast<std::int64_t>(bits))
                      : static_cast<double>(bits));
}

// Figures per partition for the functions that need the size, the totals or
// the last row of a whole partition before they can answer for its first row.
// They come from the table `add` fills while the window's sort reads its
// input, or, for the figures that depend on the sorted order (the last row, a
// variance shift) and for a window with more partitions than the table holds,
// from one pass over the sorted stream (SortedFigures).
struct PartitionStats {
    struct Col {
        std::int64_t nonnull = 0;
        steps::Extreme min{}, max{};
        steps::FrameSumInt sum{};
        steps::FrameSumF64 fsum{};  // in the window's order only
        double shift = 0.0;
        // A variance over the whole partition, from a second pass in the
        // window's order once `shift` is known.
        std::int64_t wn = 0;
        std::int64_t wapart = 0;  // values unequal to the one before
        double wprev = 0.0;
        double wmean = 0.0;
        double wm2 = 0.0;
        // The same figures over the rows whose order value is present, for a
        // range frame to the partition end (by_order).
        std::int64_t nm_nonnull = 0;
        steps::FrameSumInt nm_sum{};
        steps::FrameSumF64 nm_fsum{};  // in the window's order only
    };
    struct Part {
        std::int64_t rows = 0;
        std::int64_t nm_rows = 0;  // rows with a present order value (by_order)
        std::vector<Col> cols;
        // The float sum of the frame of a row whose order value is missing,
        // one per tail spec.
        std::vector<double> tail_sum;
        std::vector<Series> last;
    };
    // A float sum or mean over a range frame to the partition end: its value
    // column and the preceding bound of its frame.
    struct Tail {
        std::size_t slot;
        double pre;
    };

    std::vector<std::size_t> part_cols;
    std::vector<std::size_t> value_cols;
    std::vector<std::size_t> last_cols;
    std::uint64_t cap_bytes = 0;
    std::uint64_t bytes = 0;
    bool overflow = false;
    bool sorted = false;
    bool whole_var = false;  // a variance over a whole partition: two passes
    bool by_order = false;   // a range frame to the end: split by order value
    std::size_t order_col = 0;
    std::vector<Tail> tail_specs;
    ankerl::unordered_dense::map<std::string, Part> parts;

    const Part* find(const std::string& key) const {
        const auto it = parts.find(key);
        return it == parts.end() ? nullptr : &it->second;
    }

    int slot_of(std::size_t column) const {
        for (std::size_t j = 0; j < value_cols.size(); ++j)
            if (value_cols[j] == column) return static_cast<int>(j);
        return -1;
    }

    int last_slot_of(std::size_t column) const {
        for (std::size_t j = 0; j < last_cols.size(); ++j)
            if (last_cols[j] == column) return static_cast<int>(j);
        return -1;
    }

    Part fresh() const {
        Part p;
        p.cols.resize(value_cols.size());
        p.tail_sum.assign(tail_specs.size(), 0.0);
        return p;
    }

    // A figure as it is stored in the spool: the rows, the rows with an order
    // value, the columns' figures and the tail sums, as plain bytes.
    std::size_t figure_bytes() const {
        return 2 * sizeof(std::int64_t) + value_cols.size() * sizeof(Col) +
               tail_specs.size() * sizeof(double);
    }

    std::string encode(const Part& p) const {
        std::string blob(figure_bytes(), '\0');
        char* at = blob.data();
        std::memcpy(at, &p.rows, sizeof p.rows);
        at += sizeof p.rows;
        std::memcpy(at, &p.nm_rows, sizeof p.nm_rows);
        at += sizeof p.nm_rows;
        if (!value_cols.empty()) {
            std::memcpy(at, p.cols.data(), value_cols.size() * sizeof(Col));
            at += value_cols.size() * sizeof(Col);
        }
        if (!tail_specs.empty())
            std::memcpy(at, p.tail_sum.data(),
                        tail_specs.size() * sizeof(double));
        return blob;
    }

    Part decode(std::string_view blob) const {
        if (blob.size() != figure_bytes())
            throw std::logic_error("window: a figure of the wrong size");
        Part p = fresh();
        const char* at = blob.data();
        std::memcpy(&p.rows, at, sizeof p.rows);
        at += sizeof p.rows;
        std::memcpy(&p.nm_rows, at, sizeof p.nm_rows);
        at += sizeof p.nm_rows;
        if (!value_cols.empty()) {
            std::memcpy(p.cols.data(), at, value_cols.size() * sizeof(Col));
            at += value_cols.size() * sizeof(Col);
        }
        if (!tail_specs.empty())
            std::memcpy(p.tail_sum.data(), at,
                        tail_specs.size() * sizeof(double));
        return p;
    }

    // The value columns of one morsel read once.
    struct Cells {
        std::vector<NumCells> cells;
        std::vector<NumClass> cls;
        std::vector<char> numeric;
        std::vector<char> omiss;  // the order value is missing (by_order)
    };

    Cells cells_of(const Morsel& m) const {
        const std::size_t nv = value_cols.size();
        Cells c;
        c.cells.resize(nv);
        c.cls.assign(nv, NumClass::SIGNED);
        c.numeric.assign(nv, 0);
        for (std::size_t j = 0; j < nv; ++j) {
            const Series& col = m.columns[value_cols[j]];
            c.numeric[j] = num_class(col.type(), c.cls[j]) ? 1 : 0;
            if (c.numeric[j]) c.cells[j] = read_cells(col);
        }
        NumClass oc = NumClass::SIGNED;
        if (by_order && num_class(m.columns[order_col].type(), oc)) {
            std::vector<double> ov;
            order_values(m.columns[order_col], ov, c.omiss);
        }
        return c;
    }

    // Adds row `r` of `m` to `p`. `in_order` says the rows arrive in the
    // order the window reads them, which the shift of a variance depends on.
    void tally(Part& p, const Cells& c, const Morsel& m, std::int64_t r,
               bool in_order) const {
        const std::size_t nv = value_cols.size();
        const auto z = static_cast<std::size_t>(r);
        const bool by_nm = by_order && !(z < c.omiss.size() && c.omiss[z]);
        ++p.rows;
        if (by_nm) ++p.nm_rows;
        for (std::size_t j = 0; j < nv; ++j) {
            const bool ok = c.numeric[j] ? c.cells[j].ok[z] != 0
                                         : !m.columns[value_cols[j]].is_null(r);
            if (!ok) continue;
            Col& col = p.cols[j];
            ++col.nonnull;
            if (by_nm) ++col.nm_nonnull;
            if (!c.numeric[j]) continue;
            const std::uint64_t b = c.cells[j].bits[z];
            double x = 0.0;
            if (c.cls[j] == NumClass::SIGNED) {
                const auto v = static_cast<std::int64_t>(b);
                col.min.offer_i(v, true);
                col.max.offer_i(v, false);
                col.sum.enter_i(v);
                if (by_nm) col.nm_sum.enter_i(v);
                x = static_cast<double>(v);
            } else if (c.cls[j] == NumClass::UNSIGNED) {
                col.min.offer_u(b, true);
                col.max.offer_u(b, false);
                col.sum.enter_u(b);
                if (by_nm) col.nm_sum.enter_u(b);
                x = static_cast<double>(b);
            } else {
                x = bits_double(b);
            }
            if (in_order) {
                col.shift += (x - col.shift) / static_cast<double>(col.nonnull);
                col.fsum.enter(x);
                if (by_nm) col.nm_fsum.enter(x);
            }
        }
    }

    void add(const Morsel& m) {
        if (overflow) return;
        const std::vector<Series> pk = key_columns(m.columns, part_cols);
        const std::size_t nv = value_cols.size();
        const Cells cells = cells_of(m);
        for (std::int64_t r = 0; r < m.rows; ++r) {
            std::string key = row_key(pk, r);
            auto it = parts.find(key);
            if (it == parts.end()) {
                bytes += key.size() + sizeof(Part) + nv * sizeof(Col) + 64;
                if (bytes > cap_bytes) {
                    overflow = true;
                    parts.clear();
                    return;
                }
                it = parts.emplace(std::move(key), fresh()).first;
            }
            tally(it->second, cells, m, r, false);
        }
    }
};

// Passes morsels through and tallies them into PartitionStats.
// Writes the figures of partitions, in their order, to a spool FIGURE_ROWS at a
// time; decode_figures reads them back.
class FigureWriter {
   public:
    static constexpr std::size_t FIGURE_ROWS = 4096;

    FigureWriter(const PartitionStats& st, std::uint64_t memory)
        : st_(st),
          spool_(std::make_unique<spill::Spool>(memory)),
          lasts_(st.last_cols.size()) {}

    void add(const PartitionStats::Part& p, const std::vector<Series>& last) {
        blobs_.push_back(st_.encode(p));
        for (std::size_t j = 0; j < lasts_.size(); ++j)
            lasts_[j].push_back(last[j].share());
        if (blobs_.size() >= FIGURE_ROWS) flush();
    }

    std::unique_ptr<spill::Spool> finish() {
        flush();
        return std::move(spool_);
    }

   private:
    void flush() {
        if (blobs_.empty()) return;
        std::vector<Series> cols;
        cols.push_back(Series::strings(blobs_));
        for (std::vector<Series>& l : lasts_) {
            cols.push_back(concat_columns(column_ptrs(l)).materialize());
            l.clear();
        }
        spool_->add(std::move(cols), static_cast<std::int64_t>(blobs_.size()));
        blobs_.clear();
    }

    const PartitionStats& st_;
    std::unique_ptr<spill::Spool> spool_;
    std::vector<std::string> blobs_;
    std::vector<std::vector<Series>> lasts_;
};

class StatsCursor : public Cursor {
   public:
    StatsCursor(std::unique_ptr<Cursor> in,
                std::shared_ptr<PartitionStats> stats)
        : in_(std::move(in)), stats_(std::move(stats)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (m) stats_->add(*m);
        co_return m;
    }

    coro::CoroTask<bool> narrow(const Expr&) override { co_return false; }

   private:
    std::unique_ptr<Cursor> in_;
    std::shared_ptr<PartitionStats> stats_;
};

// Hands on the rows of one cursor with the columns of another, in the same
// morsels, appended.
class AppendCursor : public Cursor {
   public:
    AppendCursor(std::unique_ptr<Cursor> rows, std::unique_ptr<Cursor> extra)
        : rows_(std::move(rows)), extra_(std::move(extra)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await rows_->next(max_rows);
        auto e = co_await extra_->next(max_rows);
        if (!m && !e) co_return std::nullopt;
        if (!m || !e || m->rows != e->rows)
            throw std::logic_error("window: the suffix columns miss the rows");
        for (Series& c : e->columns) m->columns.push_back(std::move(c));
        co_return m;
    }

   private:
    std::unique_ptr<Cursor> rows_;
    std::unique_ptr<Cursor> extra_;
};

// One window function of a streaming window, as the cursor needs it: the
// columns it reads, its output name, and how much of the rows around a row it
// reads.
struct WindowSpecInfo {
    dftu_window_func func = DFTU_WINDOW_ROW_NUMBER;
    int value = -1;
    std::string out;
    std::int64_t before = 0;  // rows behind (a lag, a row frame)
    std::int64_t after = 0;   // rows ahead (a lead, a row frame)
    bool unbounded = false;   // a bound the cursor cannot cut inside
    bool whole = false;       // a frame over the whole partition
    bool start = false;       // a frame from the partition start
    bool to_end = false;      // a row or range frame to the partition end
    int tail = -1;            // its slot in PartitionStats::tail_specs
    std::int64_t nth = 0;     // nth_value
    bool range = false;       // a RANGE frame, bounded by `pre` and `fol`
    double pre = 0.0;
    double fol = 0.0;
    std::int64_t min_count = 0;
    std::int64_t buckets = 0;  // ntile
    int time = -1;             // sessionize
    int end = -1;
    double gap = 0.0;
    double span = 0.0;
};

std::vector<WindowSpecInfo> window_specs(const OwnedFrameOpArgs& args,
                                         const std::vector<std::string>& sch) {
    std::vector<std::vector<const char*>> cstrs;
    std::vector<std::vector<dftu_window_spec>> wins;
    std::vector<std::vector<dftu_group_agg>> aggs;
    const dftu_op_arg bag = args.bind(cstrs, wins, aggs);
    const auto& list = bag.args[3].winlist;
    const auto index_of = [&](const char* name) {
        if (!name) return -1;
        const auto it = std::find(sch.begin(), sch.end(), name);
        return it == sch.end() ? -1 : static_cast<int>(it - sch.begin());
    };
    std::vector<WindowSpecInfo> out;
    for (std::int32_t k = 0; k < list.n; ++k) {
        const dftu_window_spec& w = list.items[k];
        WindowSpecInfo i;
        i.func = w.func;
        i.out = w.out ? w.out : "";
        i.value = index_of(w.value);
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
            case DFTU_WINDOW_RATE:
                i.before = 1;
                break;
            case DFTU_WINDOW_NTILE:
                i.buckets = w.param.offset;
                break;
            case DFTU_WINDOW_NTH_VALUE:
                i.nth = w.param.offset;
                break;
            case DFTU_WINDOW_SESSIONIZE:
                i.time = index_of(w.param.session.time);
                i.end = index_of(w.param.session.end);
                i.gap = w.param.session.gap;
                i.span = w.param.session.span;
                i.unbounded = i.time < 0 || !(i.gap >= 0.0) || !(i.span >= 0.0);
                break;
            case DFTU_WINDOW_FRAME_SUM:
            case DFTU_WINDOW_FRAME_MIN:
            case DFTU_WINDOW_FRAME_MAX:
            case DFTU_WINDOW_FRAME_COUNT:
            case DFTU_WINDOW_FRAME_MEAN:
            case DFTU_WINDOW_FRAME_VAR:
            case DFTU_WINDOW_FRAME_STD:
            case DFTU_WINDOW_FRAME_QUANTILE:
            case DFTU_WINDOW_FRAME_COUNT_DISTINCT:
            case DFTU_WINDOW_FRAME_ARG_MAX:
            case DFTU_WINDOW_FRAME_ARG_MIN:
            case DFTU_WINDOW_FRAME_COLLECT: {
                const auto& f = w.param.frame;
                const bool up = f.preceding == DFTU_WINDOW_UNBOUNDED;
                const bool uf = f.following == DFTU_WINDOW_UNBOUNDED;
                i.min_count = f.min_count;
                if ((f.preceding < 0 && !up) || (f.following < 0 && !uf) ||
                    f.min_count < 0) {
                    i.unbounded = true;
                } else if (up && uf) {
                    i.whole = true;
                } else if (uf) {
                    // A frame to the partition end starts from the
                    // partition's figures and only drops rows behind it.
                    if (f.mode == DFTU_WINDOW_FRAME_RANGE) {
                        if (w.func == DFTU_WINDOW_FRAME_COUNT ||
                            w.func == DFTU_WINDOW_FRAME_SUM ||
                            w.func == DFTU_WINDOW_FRAME_MEAN) {
                            i.to_end = true;
                            i.range = true;
                            i.pre = static_cast<double>(f.preceding);
                        } else {
                            i.unbounded = true;
                        }
                    } else {
                        i.to_end = true;
                        i.before = f.preceding;
                    }
                } else {
                    i.start = up;
                    if (f.mode == DFTU_WINDOW_FRAME_RANGE) {
                        i.range = true;
                        i.pre = up ? 0.0 : static_cast<double>(f.preceding);
                        i.fol = static_cast<double>(f.following);
                    } else {
                        i.before = up ? 0 : f.preceding;
                        i.after = f.following;
                    }
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

// Whether a window needs the figures of PartitionStats, and for which columns.
bool window_needs_stats(std::vector<WindowSpecInfo>& specs, PartitionStats& st,
                        const std::vector<std::string>& sch,
                        const std::vector<Field>& fields) {
    bool need = false;
    // A column the plan does not type may be float.
    const auto maybe_float = [&](int column) {
        const std::string& name = sch[static_cast<std::size_t>(column)];
        for (const Field& f : fields)
            if (f.name == name)
                return f.type.id == TypeId::Unknown ||
                       f.type.id == TypeId::Float64 ||
                       f.type.id == TypeId::Float32 ||
                       f.type.id == TypeId::Float16;
        return true;
    };
    const auto add = [](std::vector<std::size_t>& cols, int column) {
        const auto c = static_cast<std::size_t>(column);
        if (std::find(cols.begin(), cols.end(), c) == cols.end())
            cols.push_back(c);
    };
    for (WindowSpecInfo& w : specs) {
        switch (w.func) {
            case DFTU_WINDOW_NTILE:
            case DFTU_WINDOW_PERCENT_RANK:
            case DFTU_WINDOW_CUME_DIST:
                need = true;
                break;
            case DFTU_WINDOW_FRAME_SUM:
            case DFTU_WINDOW_FRAME_MIN:
            case DFTU_WINDOW_FRAME_MAX:
            case DFTU_WINDOW_FRAME_COUNT:
            case DFTU_WINDOW_FRAME_MEAN:
                if (w.to_end && w.value < 0) need = true;
                if (w.to_end && w.range) st.by_order = true;
                if ((w.whole || w.to_end) && w.value >= 0) {
                    need = true;
                    add(st.value_cols, w.value);
                    // A float sum and a mean add in the window's order, and a
                    // min or max to the end reads the sorted spool backwards.
                    const bool in_order = w.func == DFTU_WINDOW_FRAME_MEAN ||
                                          (w.func == DFTU_WINDOW_FRAME_SUM &&
                                           maybe_float(w.value));
                    if (in_order ||
                        (w.to_end && (w.func == DFTU_WINDOW_FRAME_MIN ||
                                      w.func == DFTU_WINDOW_FRAME_MAX)))
                        st.sorted = true;
                    if (in_order && w.to_end && w.range) {
                        w.tail = static_cast<int>(st.tail_specs.size());
                        st.tail_specs.push_back(
                            {static_cast<std::size_t>(st.slot_of(w.value)),
                             w.pre});
                    }
                }
                break;
            case DFTU_WINDOW_FRAME_VAR:
            case DFTU_WINDOW_FRAME_STD:
                if (!w.unbounded && w.value >= 0) {
                    need = true;
                    st.sorted = true;
                    st.whole_var = st.whole_var || w.whole || w.to_end;
                    add(st.value_cols, w.value);
                }
                break;
            case DFTU_WINDOW_LAST_VALUE:
                if (w.value >= 0) {
                    need = true;
                    st.sorted = true;
                    add(st.last_cols, w.value);
                }
                break;
            default:
                break;
        }
    }
    return need;
}

// Runs the window over a stream sorted by (partition, order), a chunk of about
// `chunk_bytes` at a time. When every function can continue across a cut, a
// chunk may end inside a partition and the next one is given what the
// functions need from the rows before it, so memory is a chunk and its
// result, whatever the size of a partition:
//   running functions (sum, product, min, max, forward fill) get a seed row
//   that holds their value so far; counts and ranks are shifted by the rows
//   already emitted; lag, lead, delta, rate and the frames that are a function
//   of their rows alone (min, max, count, integer sums, quantiles, distinct
//   counts, arg min and max), over a bounded number of rows or a
//   bounded range of the order value, get the rows around the cut as context,
//   and a chunk holds back the rows that still lack their following context.
// Some functions are answered here and not by the kernel: ntile, percent_rank,
// cume_dist, the frames over a whole partition, last_value and the shift of a
// variance from figures per partition; sessionize, first and nth value, the
// frames from the partition start, the float sums and means of a frame, and
// variance, from state carried across the cut (the kernel adds and subtracts as
// the frame slides, so only the same sequence of operations gives the same
// bits). The figures come from the table the sort fills while it reads its
// input, or, when they depend on the sorted order or the table overflowed, from
// one pass over the sorted stream that spools it (sort_pass).
// A frame that ends at the partition end starts from those figures and drops
// the rows behind it. What the figures cannot say comes from more passes over
// the spool: replay_pass (a variance state, and the sum of the frame of the
// rows whose order value is missing, which must follow the kernel's float
// steps), and suffix_pass, which reads the spool backwards and writes, in the
// morsels of the rows, the extreme (min and max) or the count of unequal runs
// (variance) of the rows from each row on; a chunk carries those columns.
// Any other function (a quantile, a distinct count, arg min or max or a collect
// from the partition start or to the partition end, a min or max over a range
// to the partition end, a variance over a range, a float sum over a range from
// the partition start, running functions that share a column or that mix with
// ranks or look-around) needs its whole partition, so a chunk then ends at a
// partition boundary and a partition larger than a chunk is held whole.
// The window kernel is the same in every case: the chunk's rows leave in the
// kernel's own (partition, order) order, which the sort has already given.
class WindowStreamCursor : public Cursor {
   public:
    WindowStreamCursor(
        std::unique_ptr<Cursor> sorted, std::vector<std::string> sch,
        std::vector<std::size_t> part, std::vector<std::size_t> order,
        std::vector<WindowSpecInfo> specs, std::string name,
        const dftu_op_desc* op, std::shared_ptr<const OwnedFrameOpArgs> args,
        std::shared_ptr<PartitionStats> stats, std::uint64_t budget)
        : in_(std::move(sorted)),
          sch_(std::move(sch)),
          part_(std::move(part)),
          order_(std::move(order)),
          name_(std::move(name)),
          op_(op),
          args_(std::move(args)),
          stats_(std::move(stats)),
          budget_(budget),
          chunk_bytes_(budget / share::CHUNK) {
        for (WindowSpecInfo& w : specs) {
            SpecState s;
            s.info = std::move(w);
            specs_.push_back(std::move(s));
        }
    }

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        while (true) {
            while (!eof_ &&
                   (held_rows_ <= after_ || held_bytes_ < chunk_bytes_ ||
                    held_bytes_ < grow_to_ || need_more_)) {
                need_more_ = false;
                auto m = co_await in_->next(max_rows);
                if (!m) {
                    eof_ = true;
                    break;
                }
                if (!planned_) {
                    plan(m->columns);
                    planned_ = true;
                    if (mode_ == Carry::ALIGNED)
                        DFTRACER_UTILS_LOG_DEBUG(
                            "window: whole partitions, because of %s",
                            why_.c_str());
                    else
                        DFTRACER_UTILS_LOG_DEBUG(
                            "window: chunks cut inside partitions (%s)",
                            mode_ == Carry::SEED ? "seed row" : "context rows");
                    if (mode_ != Carry::ALIGNED && seq_) {
                        co_await sort_pass(std::move(*m), max_rows);
                        continue;
                    }
                }
                held_bytes_ += spill::columns_bytes(m->columns);
                held_rows_ += m->rows;
                held_.push_back(std::move(*m));
            }
            if (held_rows_ == 0) co_return std::nullopt;

            const std::size_t ncols = sch_.size();
            const std::size_t wide = ncols + suffix_.size();
            std::vector<Series> cols;
            cols.reserve(wide);
            for (std::size_t c = 0; c < wide; ++c) {
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
            const std::vector<Series> pk = key_columns(cols, part_);
            const auto key = [&](std::int64_t i) { return row_key(pk, i); };

            const std::int64_t emit = emit_count(cols, n, key);
            if (emit <= 0) {
                // One partition (or tie group) spans everything held: read as
                // much again before looking for its end, so the rows are
                // concatenated O(log) times and not once per morsel.
                Morsel all;
                all.rows = n;
                all.columns = std::move(cols);
                held_.push_back(std::move(all));
                grow_to_ = held_bytes_ * 2;
                need_more_ = true;
                continue;
            }
            grow_to_ = 0;

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
            if (seq_) co_await fetch_figures(emit, key);
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
                for (std::size_t c = 0; c < wide; ++c)
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
    enum class Kind {
        KERNEL,
        COUNT,
        RANK,
        DENSE,
        PERCENT,
        CUME,
        NTILE,
        WHOLE,
        SESSION,
        FRAME,
        PICK,
        LAST,
        EXTREME
    };

    struct SpecState {
        WindowSpecInfo info;
        Acc acc = Acc::NONE;
        Kind kind = Kind::KERNEL;
        int slot = -1;  // the column's slot in PartitionStats
        std::int64_t last_i = 0;
        std::uint64_t last_u = 0;
        // percent_rank
        std::int64_t rank_pos = 0;
        std::int64_t rank_last = 1;
        // sessionize
        std::int64_t session_id = 0;
        double session_first = 0.0;
        double session_end = 0.0;
        // frames answered from state
        struct Cell {
            double v = 0.0;
            double ov = 0.0;
            std::uint64_t bits = 0;
            bool present = false;
            bool miss = false;
            bool next_diff = false;
        };
        std::deque<Cell> win;
        std::int64_t win_lo = 0;
        std::int64_t win_hi = -1;
        std::int64_t win_count = 0;
        std::int64_t first_missing = -1;
        steps::FrameSumF64 win_sum;
        steps::FrameSumInt win_isum;
        double shift = 0.0;
        double mean = 0.0;
        double m2 = 0.0;
        double prev_v = 0.0;
        std::int64_t apart = 0;
        std::int64_t last_pos = -1;
        bool has_best = false;
        std::uint64_t best = 0;
        // first_value and nth_value: the row carried across the cut
        Series pick;
        bool have_pick = false;
        // EXTREME: the best value so far of the partition, and its rows.
        Series ext;
        bool have_ext = false;
        std::int64_t ext_count = 0;
        // A frame to the partition end that reads a column of suffix_pass:
        // its place among the columns of a chunk, and for a min or max the
        // suffix extremes of the rows behind the cut (a null row first).
        std::size_t ext_at = 0;
        Series hist;
        bool have_hist = false;
    };

    // A column suffix_pass adds to the sorted rows, for a frame to the
    // partition end: the number of runs of unequal values among the rows from
    // each row on (a variance), or the extreme of the values from each row on.
    struct Suffix {
        int value;
        bool apart;
        bool smallest;
    };

    static bool narrow_int(TypeId t) {
        return t == TypeId::Int8 || t == TypeId::Int16 || t == TypeId::Int32 ||
               t == TypeId::Uint8 || t == TypeId::Uint16 || t == TypeId::Uint32;
    }
    static bool any_int(TypeId t) {
        return narrow_int(t) || t == TypeId::Int64 || t == TypeId::Uint64;
    }

    // What the functions of the spec list ask of a cut.
    struct Needs {
        bool acc = false, halo = false, rank = false, rows = false;
        bool need_stats = false, need_sorted = false, range = false;
        bool tie = false;
        bool order = false;  // reads the order value without a context range
        std::int64_t before = 0, after = 0;
        double rpre = 0.0, rfol = 0.0;
        std::vector<int> acc_cols;
    };

    // The rows or the range of the order value a frame reads around a row.
    static void frame_reach(const WindowSpecInfo& w, Needs& n) {
        if (w.range) {
            n.range = true;
            n.rpre = std::max(n.rpre, w.pre);
            n.rfol = std::max(n.rfol, w.fol);
        } else {
            n.rows = true;
            n.before = std::max(n.before, w.before);
            n.after = std::max(n.after, w.after);
        }
    }

    // A frame this cursor answers from state carried across the cut. Only the
    // rows ahead of a row are read, so the chunk holds those back. A frame
    // from the partition start never drops the rows behind it, so it keeps
    // none, and a range frame that starts there would have to drop them all at
    // the first null order value, in float order: that stays whole.
    static bool frame_native(const WindowSpecInfo& w, Needs& n, bool exact) {
        if (w.start && w.range && !exact) return false;
        if (w.range) {
            n.range = true;
            n.rfol = std::max(n.rfol, w.fol);
        } else {
            n.after = std::max(n.after, w.after);
        }
        return true;
    }

    // Whether one function can stream across a cut, and what it needs. Sets
    // `why_` to the reason it cannot.
    bool classify(SpecState& s, const std::vector<Series>& cols, Needs& n) {
        const WindowSpecInfo& w = s.info;
        why_ = "function " + std::to_string(static_cast<int>(w.func)) +
               " of '" + w.out + "'";
        const TypeId vt = w.value >= 0
                              ? cols[static_cast<std::size_t>(w.value)].type()
                              : TypeId::Unknown;
        NumClass cls = NumClass::SIGNED;
        const bool numeric = w.value >= 0 && num_class(vt, cls);
        if (w.unbounded) return false;
        switch (w.func) {
            case DFTU_WINDOW_ROW_NUMBER:
            case DFTU_WINDOW_RUNNING_COUNT:
                s.kind = Kind::COUNT;
                break;
            case DFTU_WINDOW_RANK:
                s.kind = Kind::RANK;
                n.rank = true;
                n.before = std::max<std::int64_t>(n.before, 1);
                break;
            case DFTU_WINDOW_DENSE_RANK:
                s.kind = Kind::DENSE;
                n.rank = true;
                n.before = std::max<std::int64_t>(n.before, 1);
                break;
            case DFTU_WINDOW_PERCENT_RANK:
                s.kind = Kind::PERCENT;
                n.need_stats = true;
                n.tie = true;
                break;
            case DFTU_WINDOW_CUME_DIST:
                s.kind = Kind::CUME;
                n.need_stats = true;
                n.tie = true;
                tie_cut_ = true;
                break;
            case DFTU_WINDOW_NTILE:
                s.kind = Kind::NTILE;
                n.need_stats = true;
                break;
            case DFTU_WINDOW_SESSIONIZE: {
                NumClass tc = NumClass::SIGNED;
                if (!num_class(cols[static_cast<std::size_t>(w.time)].type(),
                               tc))
                    return false;
                if (w.end >= 0 &&
                    !num_class(cols[static_cast<std::size_t>(w.end)].type(),
                               tc))
                    return false;
                s.kind = Kind::SESSION;
                break;
            }
            case DFTU_WINDOW_RUNNING_SUM:
                if (vt == TypeId::Int64 || vt == TypeId::Uint64 ||
                    vt == TypeId::Float64)
                    s.acc = Acc::SEED;
                else if (narrow_int(vt))
                    s.acc = Acc::POST_ADD;
                else
                    return false;
                n.acc = true;
                n.acc_cols.push_back(w.value);
                break;
            case DFTU_WINDOW_RUNNING_PROD:
                if (vt != TypeId::Float64) return false;
                s.acc = Acc::SEED;
                n.acc = true;
                n.acc_cols.push_back(w.value);
                break;
            case DFTU_WINDOW_RUNNING_MIN:
            case DFTU_WINDOW_RUNNING_MAX:
            case DFTU_WINDOW_FILL_FORWARD:
                s.acc = Acc::SEED;
                n.acc = true;
                n.acc_cols.push_back(w.value);
                break;
            case DFTU_WINDOW_LAG:
            case DFTU_WINDOW_LEAD:
            case DFTU_WINDOW_DELTA:
            case DFTU_WINDOW_RATE:
                n.halo = true;
                n.rows = true;
                n.before = std::max(n.before, w.before);
                n.after = std::max(n.after, w.after);
                break;
            case DFTU_WINDOW_FRAME_COUNT:
            case DFTU_WINDOW_FRAME_MIN:
            case DFTU_WINDOW_FRAME_MAX:
            case DFTU_WINDOW_FRAME_SUM:
            case DFTU_WINDOW_FRAME_MEAN: {
                const bool is_count = w.func == DFTU_WINDOW_FRAME_COUNT;
                const bool is_sum = w.func == DFTU_WINDOW_FRAME_SUM;
                const bool is_mean = w.func == DFTU_WINDOW_FRAME_MEAN;
                if (w.to_end) {
                    if (!is_count && !is_sum && !is_mean) {
                        // The best of the rows still ahead of the frame's
                        // first row: the sorted spool read backwards gives it
                        // for every row (suffix_pass).
                        if (w.range || w.value < 0 ||
                            !ColumnView::supports(vt) || !stats_ ||
                            !stats_->sorted)
                            return false;
                    } else if (!is_count && !numeric) {
                        return false;
                    } else if (is_mean || (is_sum && cls == NumClass::FLOAT)) {
                        if (!stats_ || !stats_->sorted) return false;
                        // The float sum of the rows with a missing order
                        // value comes from replay_pass.
                        if (w.range && w.tail < 0) return false;
                        n.need_sorted = true;
                    }
                    if (!is_count && !is_sum && !is_mean) n.need_sorted = true;
                    if (w.range) n.order = true;
                    n.need_stats = true;
                    s.kind = Kind::FRAME;
                    break;
                }
                if (w.whole) {
                    const bool in_order = is_mean || (is_sum && numeric &&
                                                      cls == NumClass::FLOAT);
                    if (in_order) {
                        if (!numeric || !stats_ || !stats_->sorted)
                            return false;
                        n.need_sorted = true;
                    } else if (!is_count &&
                               (!numeric || cls == NumClass::FLOAT)) {
                        return false;
                    }
                    s.kind = Kind::WHOLE;
                    n.need_stats = true;
                    break;
                }
                const bool floating = numeric && cls == NumClass::FLOAT;
                const bool exact = !is_mean && !(is_sum && floating);
                if (!exact) {
                    if (!numeric) return false;
                } else if (w.start) {
                    if (is_sum ? !any_int(vt) : (!is_count && !numeric)) {
                        // A text min or max up to the row, or to a few rows
                        // after it, carries its best value across the cut.
                        if (is_sum || w.range || w.value < 0 ||
                            !ColumnView::supports(vt))
                            return false;
                        n.after = std::max(n.after, w.after);
                        s.kind = Kind::EXTREME;
                        break;
                    }
                } else {
                    if (is_sum && !any_int(vt)) return false;
                    n.halo = true;
                    frame_reach(w, n);
                    break;
                }
                if (!frame_native(w, n, exact)) return false;
                s.kind = Kind::FRAME;
                break;
            }
            case DFTU_WINDOW_FRAME_VAR:
            case DFTU_WINDOW_FRAME_STD:
                if (w.whole) {
                    if (!numeric || !stats_ || !stats_->whole_var) return false;
                    s.kind = Kind::WHOLE;
                    n.need_stats = true;
                    n.need_sorted = true;
                    break;
                }
                if (!numeric) return false;
                if (w.to_end) {
                    // Starts from the whole partition's Welford state and
                    // leaves rows behind it; whether the rows left are all
                    // equal comes from suffix_pass.
                    if (!stats_ || !stats_->whole_var) return false;
                    n.need_stats = true;
                } else if (!frame_native(w, n, false)) {
                    return false;
                }
                s.kind = Kind::FRAME;
                n.need_sorted = true;
                break;
            case DFTU_WINDOW_FRAME_QUANTILE:
            case DFTU_WINDOW_FRAME_COUNT_DISTINCT:
            case DFTU_WINDOW_FRAME_ARG_MAX:
            case DFTU_WINDOW_FRAME_ARG_MIN:
            case DFTU_WINDOW_FRAME_COLLECT:
                if (w.whole || w.start || w.to_end) return false;
                n.halo = true;
                frame_reach(w, n);
                break;
            case DFTU_WINDOW_FIRST_VALUE:
            case DFTU_WINDOW_NTH_VALUE:
                if (w.value < 0) return false;
                s.kind = Kind::PICK;
                if (w.func == DFTU_WINDOW_NTH_VALUE && w.nth > 1)
                    n.after = std::max(n.after, w.nth - 1);
                break;
            case DFTU_WINDOW_LAST_VALUE:
                if (w.value < 0) return false;
                s.kind = Kind::LAST;
                n.need_sorted = true;
                break;
            default:
                return false;
        }
        return true;
    }

    // Chooses how a cut may fall from the functions and the column types.
    void plan(const std::vector<Series>& cols) {
        mode_ = Carry::ALIGNED;
        Needs n;
        for (SpecState& s : specs_)
            if (!classify(s, cols, n)) return;
        why_ = "running functions mixed with ranks or look-around";
        if (n.acc && (n.halo || n.rank)) return;
        why_ = "rank with a look-around of more than one row";
        if (n.rank && n.before > 1) return;
        why_ = "a range frame needs one numeric order column";
        if (n.range || n.order) {
            if (order_.size() != 1) return;
            NumClass oc = NumClass::SIGNED;
            if (!num_class(cols[order_.front()].type(), oc)) return;
        }
        why_ = "figures for a whole-partition function are missing";
        if ((n.need_stats || n.need_sorted) && !stats_) return;
        why_ = "two running functions read one column";
        std::sort(n.acc_cols.begin(), n.acc_cols.end());
        if (std::adjacent_find(n.acc_cols.begin(), n.acc_cols.end()) !=
            n.acc_cols.end())
            return;
        why_ = "a whole-partition function without figures";
        for (SpecState& s : specs_) {
            const auto value = static_cast<std::size_t>(s.info.value);
            const bool var = s.info.func == DFTU_WINDOW_FRAME_VAR ||
                             s.info.func == DFTU_WINDOW_FRAME_STD;
            if (s.kind == Kind::WHOLE ||
                (s.kind == Kind::FRAME && (var || s.info.to_end)))
                s.slot = stats_->slot_of(value);
            else if (s.kind == Kind::LAST)
                s.slot = stats_->last_slot_of(value);
            else
                continue;
            // A row count to the partition end reads the partition's rows.
            if (s.slot < 0 && !(s.info.to_end && s.info.value < 0)) return;
        }
        seq_ = n.need_sorted ||
               (n.need_stats && (stats_->overflow || stats_->sorted));
        for (SpecState& s : specs_) {
            const WindowSpecInfo& w = s.info;
            if (s.kind != Kind::FRAME || !w.to_end) continue;
            const bool apart = w.func == DFTU_WINDOW_FRAME_VAR ||
                               w.func == DFTU_WINDOW_FRAME_STD;
            if (!apart && w.func != DFTU_WINDOW_FRAME_MIN &&
                w.func != DFTU_WINDOW_FRAME_MAX)
                continue;
            s.ext_at = sch_.size() + suffix_.size();
            suffix_.push_back(
                {w.value, apart, w.func == DFTU_WINDOW_FRAME_MIN});
        }
        mode_ = n.acc ? Carry::SEED : Carry::HALO;
        before_ = n.before;
        after_ = n.after;
        range_ = n.range;
        rpre_ = n.rpre;
        rfol_ = n.rfol;
        need_tie_ = n.tie;
    }

    // Reads the rest of the sorted stream into a spool, tallying the figures
    // of each partition in sorted order into a second spool, and then reads
    // both back in step. `first` is the morsel already read.
    coro::CoroTask<void> sort_pass(Morsel first, std::int64_t max_rows) {
        static_assert(std::is_trivially_copyable_v<PartitionStats::Col>);
        body_ = std::make_unique<spill::Spool>(budget_ / share::SPOOL);
        const PartitionStats& st = *stats_;
        const std::size_t nl = st.last_cols.size();
        FigureWriter figures(st, budget_ / share::SMALL);
        PartitionStats::Part cur;
        bool open = false;
        std::string cur_key;
        std::vector<Series> prev_last;
        const auto close = [&](const std::vector<Series>& last) {
            figures.add(cur, last);
        };
        std::optional<Morsel> m = std::move(first);
        while (m) {
            if (m->rows > 0) {
                const std::vector<Series> pk =
                    key_columns(m->columns, st.part_cols);
                const PartitionStats::Cells cells = st.cells_of(*m);
                for (std::int64_t r = 0; r < m->rows; ++r) {
                    std::string key = row_key(pk, r);
                    if (!open || key != cur_key) {
                        if (open) {
                            std::vector<Series> last;
                            for (std::size_t j = 0; j < nl; ++j)
                                last.push_back(r > 0
                                                   ? m->columns[st.last_cols[j]]
                                                         .take({r - 1})
                                                         .materialize()
                                                   : prev_last[j].share());
                            close(last);
                        }
                        cur = st.fresh();
                        cur_key = std::move(key);
                        open = true;
                    }
                    st.tally(cur, cells, *m, r, true);
                }
                prev_last.clear();
                for (std::size_t j = 0; j < nl; ++j)
                    prev_last.push_back(m->columns[st.last_cols[j]]
                                            .take({m->rows - 1})
                                            .materialize());
                body_->add(std::move(m->columns), m->rows);
            }
            m = co_await in_->next(max_rows);
        }
        if (open) close(prev_last);
        figures_ = figures.finish();
        if (st.whole_var || !st.tail_specs.empty())
            co_await replay_pass(max_rows);
        if (suffix_.empty()) {
            in_ = body_->reader();
        } else {
            co_await suffix_pass(max_rows);
            in_ = std::make_unique<AppendCursor>(
                body_->reader(), suffix_spool_->reverse_reader());
        }
        figures_reader_ = figures_->reader();
        co_return;
    }

    // The second pass over the sorted rows, with the figures of the first,
    // which it writes again with what it adds:
    //   a variance over a whole partition, or a row frame to its end, starts
    //   its Welford steps from the shift the first pass ends with, so the
    //   state of all the rows is only known after it;
    //   a float sum or mean over a range frame to the partition end slides
    //   as the kernel does over the rows with an order value, which leave in
    //   the order they entered, and then adds the rows whose order value is
    //   missing before the rest of the first ones leave: the sum of the frame
    //   of those rows is a figure of the partition (tail_sum).
    coro::CoroTask<void> replay_pass(std::int64_t max_rows) {
        const PartitionStats& st = *stats_;
        const std::size_t nv = st.value_cols.size();
        const std::size_t nt = st.tail_specs.size();
        auto body = body_->reader();
        auto old = figures_->reader();
        FigureWriter out(st, budget_ / share::SMALL);
        std::deque<PartitionStats::Part> pending;
        struct Slide {
            std::deque<SpecState::Cell> win;
            steps::FrameSumF64 sum;
            bool tail = false;
        };
        std::vector<Slide> slides(nt);
        PartitionStats::Part cur;
        bool open = false;
        std::string cur_key;
        const auto close = [&]() {
            for (std::size_t t = 0; t < nt; ++t) {
                Slide& sl = slides[t];
                if (!sl.tail) continue;
                while (!sl.win.empty()) {
                    const SpecState::Cell c = sl.win.front();
                    sl.win.pop_front();
                    if (c.present) sl.sum.leave(c.v);
                }
                cur.tail_sum[t] = sl.sum.sum;
            }
            out.add(cur, cur.last);
        };
        while (auto m = co_await body->next(max_rows)) {
            const std::vector<Series> pk =
                key_columns(m->columns, st.part_cols);
            const PartitionStats::Cells cells = st.cells_of(*m);
            std::vector<double> ov;
            std::vector<char> miss;
            if (nt > 0) order_values(m->columns[st.order_col], ov, miss);
            for (std::int64_t r = 0; r < m->rows; ++r) {
                std::string key = row_key(pk, r);
                if (!open || key != cur_key) {
                    if (open) close();
                    while (pending.empty()) {
                        auto f = co_await old->next(1 << 20);
                        if (!f)
                            throw std::logic_error(
                                "window: the figures end before the rows do");
                        decode_figures(*f, pending);
                    }
                    cur = std::move(pending.front());
                    pending.pop_front();
                    cur_key = std::move(key);
                    open = true;
                    for (std::size_t t = 0; t < nt; ++t) {
                        slides[t] = Slide{};
                        slides[t].sum = cur.cols[st.tail_specs[t].slot].nm_fsum;
                    }
                }
                const auto z = static_cast<std::size_t>(r);
                if (st.whole_var) {
                    for (std::size_t j = 0; j < nv; ++j) {
                        if (!cells.numeric[j] || !cells.cells[j].ok[z])
                            continue;
                        const double x =
                            cell_double(cells.cls[j], cells.cells[j].bits[z]);
                        PartitionStats::Col& c = cur.cols[j];
                        if (c.wn > 0 && x != c.wprev) ++c.wapart;
                        c.wprev = x;
                        const double y = x - c.shift;
                        const double d = y - c.wmean;
                        c.wmean += d / static_cast<double>(c.wn + 1);
                        c.wm2 += d * (y - c.wmean);
                        ++c.wn;
                    }
                }
                for (std::size_t t = 0; t < nt; ++t) {
                    const PartitionStats::Tail& ts = st.tail_specs[t];
                    Slide& sl = slides[t];
                    SpecState::Cell c;
                    c.present =
                        cells.numeric[ts.slot] && cells.cells[ts.slot].ok[z];
                    if (c.present)
                        c.v = cell_double(cells.cls[ts.slot],
                                          cells.cells[ts.slot].bits[z]);
                    if (miss[z]) {
                        sl.tail = true;
                        if (c.present) sl.sum.enter(c.v);
                        continue;
                    }
                    c.ov = ov[z];
                    sl.win.push_back(c);
                    const double lo_val = c.ov - ts.pre;
                    while (!sl.win.empty() && sl.win.front().ov < lo_val) {
                        const SpecState::Cell f = sl.win.front();
                        sl.win.pop_front();
                        if (f.present) sl.sum.leave(f.v);
                    }
                }
            }
        }
        if (open) close();
        figures_ = out.finish();
    }

    // The columns of `suffix_`, from the sorted rows read backwards: for each
    // row, what the rows from it to the end of its partition give. They are
    // written in the morsels of the rows, last first, and read back from the
    // end, so they arrive with the rows. For a min or max, the best value
    // among the present rows from the row on, an equal value that comes later
    // winning as in the window kernel; for a variance, the number of places
    // where one present value differs from the next present one.
    coro::CoroTask<void> suffix_pass(std::int64_t max_rows) {
        struct Back {
            Series best;        // one row: the best of the rows after
            bool have = false;
            double prev = 0.0;  // the nearest present value after
            std::int64_t runs = 0;
        };
        struct Work {
            NumCells cells;
            NumClass cls = NumClass::SIGNED;
            std::optional<ColumnView> view;
            std::optional<ColumnView> carried_view;
            Series carried;
            std::vector<std::int64_t> out;
            std::int64_t best = -1;  // the row of the best, -1 the carried
        };
        suffix_spool_ = std::make_unique<spill::Spool>(budget_ / share::SPOOL);
        std::vector<Back> back(suffix_.size());
        const std::vector<std::int64_t> null_row{-1};
        auto rev = body_->reverse_reader();
        std::string cur_key;
        bool open = false;
        while (auto m = co_await rev->next(max_rows)) {
            const std::vector<Series> pk = key_columns(m->columns, part_);
            const auto rows = static_cast<std::size_t>(m->rows);
            std::vector<Work> work(suffix_.size());
            for (std::size_t j = 0; j < suffix_.size(); ++j) {
                const Series& col =
                    m->columns[static_cast<std::size_t>(suffix_[j].value)];
                Work& wk = work[j];
                wk.out.assign(rows, -1);
                if (suffix_[j].apart) {
                    num_class(col.type(), wk.cls);
                    wk.cells = read_cells(col);
                } else {
                    wk.view.emplace(col);
                    wk.carried = back[j].have ? back[j].best.share()
                                              : col.take(null_row);
                    wk.carried_view.emplace(wk.carried);
                }
            }
            for (std::int64_t i = m->rows - 1; i >= 0; --i) {
                std::string key = row_key(pk, i);
                if (!open || key != cur_key) {
                    for (Back& b : back) {
                        b.have = false;
                        b.runs = 0;
                    }
                    for (Work& wk : work) wk.best = -1;
                    cur_key = std::move(key);
                    open = true;
                }
                const auto z = static_cast<std::size_t>(i);
                for (std::size_t j = 0; j < suffix_.size(); ++j) {
                    Back& b = back[j];
                    Work& wk = work[j];
                    if (suffix_[j].apart) {
                        if (wk.cells.ok[z]) {
                            const double x =
                                cell_double(wk.cls, wk.cells.bits[z]);
                            if (b.have && x != b.prev) ++b.runs;
                            b.prev = x;
                            b.have = true;
                        }
                        wk.out[z] = b.runs;
                        continue;
                    }
                    const Series& col =
                        m->columns[static_cast<std::size_t>(suffix_[j].value)];
                    if (!col.is_null(i)) {
                        const int c =
                            !b.have ? 0
                            : wk.best >= 0
                                ? wk.view->compare(i, wk.best)
                                : wk.view->compare(i, *wk.carried_view, 0);
                        if (!b.have || (suffix_[j].smallest ? c < 0 : c > 0)) {
                            wk.best = i;
                            b.have = true;
                        }
                    }
                    if (b.have) wk.out[z] = wk.best >= 0 ? 1 + wk.best : 0;
                }
            }
            std::vector<Series> cols;
            for (std::size_t j = 0; j < suffix_.size(); ++j) {
                const Series& col =
                    m->columns[static_cast<std::size_t>(suffix_[j].value)];
                Work& wk = work[j];
                if (suffix_[j].apart) {
                    cols.push_back(Series::flat_i64(wk.out.data(), m->rows));
                    continue;
                }
                cols.push_back(concat_columns({&wk.carried, &col})
                                   .materialize()
                                   .take(wk.out)
                                   .materialize());
                if (wk.best >= 0)
                    back[j].best = col.take(std::vector<std::int64_t>{wk.best})
                                       .materialize();
            }
            suffix_spool_->add(std::move(cols), m->rows);
        }
    }

    void decode_figures(const Morsel& m) { decode_figures(m, figs_buf_); }

    void decode_figures(const Morsel& m,
                        std::deque<PartitionStats::Part>& into) const {
        const std::size_t nl = stats_->last_cols.size();
        for (std::int64_t i = 0; i < m.rows; ++i) {
            PartitionStats::Part p =
                stats_->decode(read_bytes(m.columns[0], i));
            for (std::size_t j = 0; j < nl; ++j)
                p.last.push_back(m.columns[1 + j].take({i}).materialize());
            into.push_back(std::move(p));
        }
    }

    // Reads the figures of the partitions that start among the first `emit`
    // rows of the held chunk.
    template <class Key>
    coro::CoroTask<void> fetch_figures(std::int64_t emit, const Key& key) {
        std::int64_t starts = E_ > 0 && key(0) == last_key_ ? 0 : 1;
        std::string prev = key(0);
        for (std::int64_t r = 1; r < emit; ++r) {
            std::string k = key(r);
            if (k != prev) {
                ++starts;
                prev = std::move(k);
            }
        }
        while (static_cast<std::int64_t>(figs_buf_.size()) < starts) {
            auto m = co_await figures_reader_->next(1 << 20);
            if (!m)
                throw std::logic_error(
                    "window: the figures end before the rows do");
            decode_figures(*m);
        }
        figs_need_ = starts;
    }

    // The rows of the held chunk that can leave now.
    template <class Key>
    std::int64_t emit_count(const std::vector<Series>& cols, std::int64_t n,
                            const Key& key) {
        if (mode_ == Carry::ALIGNED) {
            if (eof_) return n;
            const std::string last = key(n - 1);
            std::int64_t emit = n - 1;
            while (emit > 0 && key(emit - 1) == last) --emit;
            return emit;
        }
        if (eof_) return n;
        std::int64_t emit = n - after_;
        if (range_) {
            std::vector<double> ov;
            std::vector<char> miss;
            order_values(cols[order_.front()], ov, miss);
            const std::string last = key(n - 1);
            std::int64_t i = n - 1;
            while (i > 0 && key(i - 1) == last) --i;
            std::int64_t done = i;
            const bool last_missing =
                miss[static_cast<std::size_t>(n - 1)] != 0;
            while (done < n && !miss[static_cast<std::size_t>(done)] &&
                   (last_missing || ov[static_cast<std::size_t>(done)] + rfol_ <
                                        ov[static_cast<std::size_t>(n - 1)]))
                ++done;
            emit = std::min(emit, done);
        }
        if (tie_cut_) {
            std::vector<Series> tk = key_columns(cols, part_);
            for (Series& c : key_columns(cols, order_))
                tk.push_back(std::move(c));
            const std::string last = row_key(tk, n - 1);
            std::int64_t t = n - 1;
            while (t > 0 && row_key(tk, t - 1) == last) --t;
            emit = std::min(emit, t);
        }
        return emit;
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

    // The column of a native result, with the type the kernel gives it.
    static void put(DataFrame& res, const std::string& name, Series col) {
        const int oc = column_index_of(res.names, name);
        if (oc >= 0) res.columns[static_cast<std::size_t>(oc)] = std::move(col);
    }

    // The functions this cursor answers itself, over the emitted rows of the
    // chunk. `cols` holds all `n` rows held (the ones after `emit` are only
    // read ahead), `continuing` says the first row continues the partition
    // emitted before.
    // Per-row facts of one chunk that the native functions read.
    struct RowInfo {
        std::vector<char> new_part, tie_prev;
        std::vector<std::int64_t> pos, group_end, run_last;
        std::vector<const PartitionStats::Part*> part_of;
        std::vector<double> ov;
        std::vector<char> miss;
    };

    void native_ntile(SpecState& s, DataFrame& res, std::size_t ue,
                      const RowInfo& row) {
        const WindowSpecInfo& w = s.info;
        std::vector<std::int64_t> v(ue, 0);
        std::vector<std::uint8_t> okv(ue, 1);
        for (std::size_t r = 0; r < ue; ++r) {
            if (w.buckets <= 0) {
                okv[r] = 0;
                continue;
            }
            const std::int64_t sz = row.part_of[r]->rows;
            const std::int64_t local = row.pos[r] - 1;
            const std::int64_t base = sz / w.buckets;
            const std::int64_t rem = sz % w.buckets;
            const std::int64_t big = rem * (base + 1);
            v[r] = local < big ? local / (base + 1) + 1
                               : rem + (local - big) / base + 1;
        }
        std::vector<std::uint64_t> bits(ue);
        for (std::size_t r = 0; r < ue; ++r)
            bits[r] = static_cast<std::uint64_t>(v[r]);
        put(res, w.out,
            make_numeric(bits, okv, NumClass::SIGNED, TypeId::Int64));
    }

    void native_distribution(SpecState& s, DataFrame& res, std::size_t ue,
                             const RowInfo& row) {
        const WindowSpecInfo& w = s.info;
        std::vector<std::uint64_t> bits(ue);
        const std::vector<std::uint8_t> okv(ue, 1);
        for (std::size_t r = 0; r < ue; ++r) {
            const std::int64_t sz = row.part_of[r]->rows;
            double out = 0.0;
            if (s.kind == Kind::PERCENT) {
                if (row.new_part[r] || !row.tie_prev[r]) {
                    s.rank_pos = row.pos[r];
                    s.rank_last = row.pos[r];
                }
                out = sz > 1 ? static_cast<double>(s.rank_last - 1) /
                                   static_cast<double>(sz - 1)
                             : 0.0;
            } else {
                out = static_cast<double>(row.group_end[r]) /
                      static_cast<double>(sz);
            }
            bits[r] = double_bits(out);
        }
        put(res, w.out,
            make_numeric(bits, okv, NumClass::FLOAT, TypeId::Float64));
    }

    void native_whole(SpecState& s, const std::vector<Series>& cols,
                      DataFrame& res, std::size_t ue, const RowInfo& row) {
        const WindowSpecInfo& w = s.info;
        const TypeId vt = cols[static_cast<std::size_t>(w.value)].type();
        NumClass cls = NumClass::SIGNED;
        const bool numeric = num_class(vt, cls);
        std::vector<std::uint64_t> bits(ue, 0);
        std::vector<std::uint8_t> okv(ue, 1);
        TypeId out_type = TypeId::Int64;
        NumClass out_cls = NumClass::SIGNED;
        const bool is_mean = w.func == DFTU_WINDOW_FRAME_MEAN;
        const bool var_fn =
            w.func == DFTU_WINDOW_FRAME_VAR || w.func == DFTU_WINDOW_FRAME_STD;
        if (var_fn) {
            for (std::size_t r = 0; r < ue; ++r) {
                const PartitionStats::Col& c =
                    row.part_of[r]->cols[static_cast<std::size_t>(s.slot)];
                if (c.wn < 2 || c.wn < w.min_count) {
                    okv[r] = 0;
                    continue;
                }
                const double var =
                    c.wapart == 0
                        ? 0.0
                        : std::max(c.wm2, 0.0) / static_cast<double>(c.wn - 1);
                bits[r] = std::bit_cast<std::uint64_t>(
                    w.func == DFTU_WINDOW_FRAME_STD ? std::sqrt(var) : var);
            }
            put(res, w.out,
                make_numeric(bits, okv, NumClass::FLOAT, TypeId::Float64));
            return;
        }
        const bool as_double = is_mean || (w.func == DFTU_WINDOW_FRAME_SUM &&
                                           cls == NumClass::FLOAT);
        for (std::size_t r = 0; r < ue; ++r) {
            const PartitionStats::Col& c =
                row.part_of[r]->cols[static_cast<std::size_t>(s.slot)];
            if (w.func == DFTU_WINDOW_FRAME_COUNT) {
                bits[r] = static_cast<std::uint64_t>(c.nonnull);
                continue;
            }
            if (c.nonnull == 0 || c.nonnull < w.min_count) {
                okv[r] = 0;
                continue;
            }
            if (as_double) {
                bits[r] = std::bit_cast<std::uint64_t>(
                    is_mean ? c.fsum.mean(c.nonnull) : c.fsum.sum);
            } else if (w.func == DFTU_WINDOW_FRAME_SUM) {
                bits[r] = cls == NumClass::UNSIGNED
                              ? c.sum.u64()
                              : static_cast<std::uint64_t>(c.sum.i64());
            } else {
                const steps::Extreme& e =
                    w.func == DFTU_WINDOW_FRAME_MIN ? c.min : c.max;
                bits[r] = cls == NumClass::UNSIGNED
                              ? e.value.u
                              : static_cast<std::uint64_t>(e.value.i);
            }
        }
        if (as_double) {
            out_cls = NumClass::FLOAT;
            out_type = TypeId::Float64;
        } else if (w.func == DFTU_WINDOW_FRAME_SUM) {
            out_cls = cls;
            out_type =
                cls == NumClass::UNSIGNED ? TypeId::Uint64 : TypeId::Int64;
        } else if (w.func != DFTU_WINDOW_FRAME_COUNT && numeric) {
            out_cls = cls;
            out_type = vt;
        }
        put(res, w.out, make_numeric(bits, okv, out_cls, out_type));
    }

    template <class Key>
    void native(const std::vector<Series>& cols, DataFrame& res, std::int64_t n,
                std::int64_t emit, bool continuing, const Key& key) {
        bool any = false;
        for (const SpecState& s : specs_) any = any || s.kind >= Kind::PERCENT;
        if (!any) return;
        const auto un = static_cast<std::size_t>(n);
        RowInfo row;
        std::vector<char>& new_part = row.new_part;
        std::vector<char>& tie_prev = row.tie_prev;
        std::vector<std::int64_t>& pos = row.pos;
        new_part.assign(un, 0);
        tie_prev.assign(un, 0);
        pos.assign(un, 0);
        const std::vector<Series> ok = key_columns(cols, order_);
        std::string prev_key, prev_order;
        for (std::int64_t r = 0; r < n; ++r) {
            const auto z = static_cast<std::size_t>(r);
            std::string k = key(r);
            std::string o = need_tie_ ? row_key(ok, r) : std::string();
            if (r == 0) {
                new_part[z] = continuing ? 0 : 1;
                pos[z] = continuing ? E_ + 1 : 1;
                tie_prev[z] = continuing && o == last_order_key_ ? 1 : 0;
            } else {
                new_part[z] = k != prev_key ? 1 : 0;
                pos[z] = new_part[z] ? 1 : pos[z - 1] + 1;
                tie_prev[z] = !new_part[z] && o == prev_order ? 1 : 0;
            }
            prev_key = std::move(k);
            prev_order = std::move(o);
        }
        // The position of the last row of each row's tie group.
        std::vector<std::int64_t>& group_end = row.group_end;
        group_end.assign(un, 0);
        for (std::int64_t r = n - 1; r >= 0; --r) {
            const auto z = static_cast<std::size_t>(r);
            group_end[z] = (r + 1 < n && !new_part[z + 1] && tie_prev[z + 1])
                               ? group_end[z + 1]
                               : pos[z];
        }
        row.run_last.assign(un, n - 1);
        for (std::int64_t r = n - 2; r >= 0; --r) {
            const auto z = static_cast<std::size_t>(r);
            row.run_last[z] = new_part[z + 1] ? r : row.run_last[z + 1];
        }
        for (const SpecState& s : specs_)
            if (s.kind == Kind::FRAME && s.info.range) {
                order_values(cols[order_.front()], row.ov, row.miss);
                break;
            }
        // The stats of the partition of each row.
        std::vector<const PartitionStats::Part*>& part_of = row.part_of;
        part_of.assign(un, nullptr);
        const auto ue = static_cast<std::size_t>(emit);
        if (seq_) {
            seq_parts_.clear();
            seq_parts_.reserve(static_cast<std::size_t>(figs_need_) + 1);
            if (continuing) seq_parts_.push_back(std::move(carry_part_));
            for (std::size_t z = 0; z < ue; ++z) {
                if (new_part[z]) {
                    seq_parts_.push_back(std::move(figs_buf_.front()));
                    figs_buf_.pop_front();
                }
                part_of[z] = &seq_parts_.back();
            }
            const PartitionStats::Part& tail = seq_parts_.back();
            carry_part_.rows = tail.rows;
            carry_part_.nm_rows = tail.nm_rows;
            carry_part_.cols = tail.cols;
            carry_part_.tail_sum = tail.tail_sum;
            carry_part_.last.clear();
            for (const Series& l : tail.last)
                carry_part_.last.push_back(l.share());
        } else if (stats_) {
            const PartitionStats::Part* cur = nullptr;
            for (std::int64_t r = 0; r < n; ++r) {
                const auto z = static_cast<std::size_t>(r);
                if (r == 0 || new_part[z]) cur = stats_->find(key(r));
                part_of[z] = cur;
            }
        }
        for (SpecState& s : specs_) {
            switch (s.kind) {
                case Kind::NTILE:
                    native_ntile(s, res, ue, row);
                    break;
                case Kind::PERCENT:
                case Kind::CUME:
                    native_distribution(s, res, ue, row);
                    break;
                case Kind::WHOLE:
                    native_whole(s, cols, res, ue, row);
                    break;
                case Kind::SESSION:
                    native_session(s, cols, res, ue, new_part);
                    break;
                case Kind::FRAME:
                    native_frame(s, cols, res, ue, row);
                    break;
                case Kind::PICK:
                    native_pick(s, cols, res, ue, row);
                    break;
                case Kind::EXTREME:
                    native_extreme(s, cols, res, ue, row);
                    break;
                case Kind::LAST:
                    native_last(s, cols, res, ue, row);
                    break;
                default:
                    break;
            }
        }
        if (need_tie_) {
            const std::vector<Series> okc = key_columns(cols, order_);
            last_order_key_ = row_key(okc, emit - 1);
        }
    }

    void native_session(SpecState& s, const std::vector<Series>& cols,
                        DataFrame& res, std::size_t emit,
                        const std::vector<char>& new_part) {
        const WindowSpecInfo& w = s.info;
        const NumCells t = read_cells(cols[static_cast<std::size_t>(w.time)]);
        NumClass tc = NumClass::SIGNED;
        num_class(cols[static_cast<std::size_t>(w.time)].type(), tc);
        NumCells e;
        NumClass ec = NumClass::SIGNED;
        if (w.end >= 0) {
            e = read_cells(cols[static_cast<std::size_t>(w.end)]);
            num_class(cols[static_cast<std::size_t>(w.end)].type(), ec);
        }
        std::vector<std::uint64_t> bits(emit, 0);
        std::vector<std::uint8_t> okv(emit, 1);
        for (std::size_t r = 0; r < emit; ++r) {
            if (new_part[r]) {
                s.session_id = 0;
                s.session_first = 0.0;
                s.session_end = 0.0;
            }
            if (!t.ok[r]) {
                okv[r] = 0;
                continue;
            }
            const double at = cell_double(tc, t.bits[r]);
            double end = at;
            if (w.end >= 0 && e.ok[r])
                end = std::max(at, cell_double(ec, e.bits[r]));
            if (s.session_id == 0 || at - s.session_end > w.gap ||
                (w.span > 0.0 && at - s.session_first > w.span)) {
                ++s.session_id;
                s.session_first = at;
                s.session_end = end;
            } else {
                s.session_end = std::max(s.session_end, end);
            }
            bits[r] = static_cast<std::uint64_t>(s.session_id);
        }
        put(res, w.out,
            make_numeric(bits, okv, NumClass::SIGNED, TypeId::Int64));
    }

    // A frame answered by the kernel's own steps, from the state of the rows
    // that entered it before the cut. Rows enter at the front of the frame and
    // leave at the back in the order the kernel does it, so a float sum, a
    // mean and a variance give the same bits. A frame from the partition start
    // never drops a row, so it keeps none of them.
    void native_frame(SpecState& s, const std::vector<Series>& cols,
                      DataFrame& res, std::size_t emit, const RowInfo& row) {
        const WindowSpecInfo& w = s.info;
        const Series& vcol = cols[static_cast<std::size_t>(w.value)];
        NumClass cls = NumClass::SIGNED;
        const bool numeric = num_class(vcol.type(), cls);
        NumCells cells;
        if (numeric) {
            cells = read_cells(vcol);
        } else {
            const auto len = static_cast<std::size_t>(vcol.length());
            cells.bits.assign(len, 0);
            cells.ok.resize(len);
            for (std::size_t i = 0; i < len; ++i)
                cells.ok[i] =
                    vcol.is_null(static_cast<std::int64_t>(i)) ? 0 : 1;
        }
        const bool count_fn = w.func == DFTU_WINDOW_FRAME_COUNT;
        const bool var_fn =
            w.func == DFTU_WINDOW_FRAME_VAR || w.func == DFTU_WINDOW_FRAME_STD;
        const bool mean_fn = w.func == DFTU_WINDOW_FRAME_MEAN;
        const bool sum_fn = w.func == DFTU_WINDOW_FRAME_SUM;
        const bool float_acc = mean_fn || (sum_fn && cls == NumClass::FLOAT);
        const bool smallest = w.func == DFTU_WINDOW_FRAME_MIN;
        const bool extreme_fn = smallest || w.func == DFTU_WINDOW_FRAME_MAX;
        const bool keep = !w.start;
        const auto order_of = [&](std::uint64_t a, std::uint64_t b) {
            switch (cls) {
                case NumClass::SIGNED: {
                    const auto x = static_cast<std::int64_t>(a);
                    const auto y = static_cast<std::int64_t>(b);
                    return x < y ? -1 : (x > y ? 1 : 0);
                }
                case NumClass::UNSIGNED:
                    return a < b ? -1 : (a > b ? 1 : 0);
                case NumClass::FLOAT:
                    break;
            }
            return compare_doubles(bits_double(a), bits_double(b));
        };
        const bool suffix = w.to_end && extreme_fn;
        NumCells apart_cells;
        if (var_fn && w.to_end) apart_cells = read_cells(cols[s.ext_at]);
        Series hist;
        std::int64_t hist_len = 0;
        std::vector<std::int64_t> src;
        if (suffix) {
            hist = s.have_hist ? s.hist.share()
                               : cols[s.ext_at]
                                     .take(std::vector<std::int64_t>{-1})
                                     .materialize();
            hist_len = hist.length();
            src.assign(emit, -1);
        }
        const auto cell_at = [&](std::size_t i) {
            SpecState::Cell c;
            c.present = cells.ok[i] != 0;
            c.bits = cells.bits[i];
            if (c.present && (float_acc || var_fn))
                c.v = cell_double(cls, c.bits);
            if (var_fn && w.to_end) c.bits = apart_cells.bits[i];
            if (w.range) {
                c.ov = row.ov[i];
                c.miss = row.miss[i] != 0;
            }
            return c;
        };
        const auto enter = [&](std::int64_t q, const SpecState::Cell& c) {
            if (c.present) {
                if (var_fn) {
                    if (s.win_count > 0 && c.v != s.prev_v) {
                        ++s.apart;
                        if (keep)
                            s.win[static_cast<std::size_t>(s.last_pos -
                                                           s.win_lo)]
                                .next_diff = true;
                    }
                    s.prev_v = c.v;
                    s.last_pos = q;
                    const double x = c.v - s.shift;
                    const double d = x - s.mean;
                    s.mean += d / static_cast<double>(s.win_count + 1);
                    s.m2 += d * (x - s.mean);
                } else if (float_acc) {
                    s.win_sum.enter(c.v);
                } else if (sum_fn) {
                    if (cls == NumClass::UNSIGNED)
                        s.win_isum.enter_u(c.bits);
                    else
                        s.win_isum.enter_i(static_cast<std::int64_t>(c.bits));
                } else if (extreme_fn) {
                    const int o = s.has_best ? order_of(c.bits, s.best) : 0;
                    if (!s.has_best || (smallest ? o <= 0 : o >= 0)) {
                        s.best = c.bits;
                        s.has_best = true;
                    }
                }
                ++s.win_count;
            }
            if (keep) s.win.push_back(c);
        };
        const auto leave = [&]() {
            const SpecState::Cell c = s.win.front();
            s.win.pop_front();
            if (!c.present) return;
            --s.win_count;
            if (var_fn) {
                if (s.win_count == 0) {
                    s.mean = 0.0;
                    s.m2 = 0.0;
                } else {
                    const double x = c.v - s.shift;
                    const double d = x - s.mean;
                    s.mean -= d / static_cast<double>(s.win_count);
                    s.m2 -= d * (x - s.mean);
                }
                if (c.next_diff) --s.apart;
            } else if (float_acc) {
                s.win_sum.leave(c.v);
            } else if (sum_fn) {
                if (cls == NumClass::UNSIGNED)
                    s.win_isum.leave_u(c.bits);
                else
                    s.win_isum.leave_i(static_cast<std::int64_t>(c.bits));
            }
        };

        std::vector<std::uint64_t> bits(emit, 0);
        std::vector<std::uint8_t> okv(emit, 1);
        const double inf = std::numeric_limits<double>::infinity();
        for (std::size_t r = 0; r < emit; ++r) {
            if (row.new_part[r]) {
                s.win.clear();
                s.win_lo = 0;
                s.win_hi = -1;
                s.win_count = 0;
                s.first_missing = -1;
                s.win_sum = steps::FrameSumF64{};
                s.win_isum = steps::FrameSumInt{};
                s.mean = 0.0;
                s.m2 = 0.0;
                s.apart = 0;
                s.last_pos = -1;
                s.has_best = false;
                if (var_fn)
                    s.shift = row.part_of[r]
                                  ->cols[static_cast<std::size_t>(s.slot)]
                                  .shift;
                if (w.to_end) {
                    // Every row of the partition (of a range frame, every row
                    // with an order value) is in the frame of its first row,
                    // entered in the window's order.
                    const PartitionStats::Part& p = *row.part_of[r];
                    const std::int64_t in_frame = w.range ? p.nm_rows : p.rows;
                    if (s.slot >= 0) {
                        const PartitionStats::Col& c =
                            p.cols[static_cast<std::size_t>(s.slot)];
                        s.win_count = w.range ? c.nm_nonnull : c.nonnull;
                        s.win_isum = w.range ? c.nm_sum : c.sum;
                        s.win_sum = w.range ? c.nm_fsum : c.fsum;
                        if (var_fn) {
                            s.mean = c.wmean;
                            s.m2 = c.wm2;
                        }
                    } else {
                        s.win_count = in_frame;
                    }
                    s.win_hi = in_frame - 1;
                }
            }
            const auto ri = static_cast<std::int64_t>(r);
            const std::int64_t local = row.pos[r] - 1;
            const std::int64_t end = local + (row.run_last[r] - ri);
            const auto at = [&](std::int64_t q) {
                return static_cast<std::size_t>(ri + (q - local));
            };
            std::int64_t lo = 0;
            std::int64_t hi = 0;
            if (w.to_end && w.range && row.miss[r]) {
                // The rows whose order value is missing are peers of each
                // other: the frame of each is those rows, all entered at the
                // first of them, after which the rest of the rows with an
                // order value leave.
                if (s.first_missing < 0) {
                    const PartitionStats::Part& p = *row.part_of[r];
                    s.first_missing = p.nm_rows;
                    s.win.clear();
                    if (s.slot >= 0) {
                        const PartitionStats::Col& c =
                            p.cols[static_cast<std::size_t>(s.slot)];
                        s.win_count = c.nonnull - c.nm_nonnull;
                        s.win_isum.sum = c.sum.sum - c.nm_sum.sum;
                        if (w.tail >= 0)
                            s.win_sum.sum =
                                p.tail_sum[static_cast<std::size_t>(w.tail)];
                    } else {
                        s.win_count = p.rows - p.nm_rows;
                    }
                    s.win_lo = p.nm_rows;
                    s.win_hi = p.rows - 1;
                }
                lo = s.win_lo;
                hi = s.win_hi;
            } else if (w.to_end) {
                s.win.push_back(cell_at(r));
                lo = std::max<std::int64_t>(0, local - w.before);
                if (w.range) {
                    const double lo_val = row.ov[r] - w.pre;
                    lo = s.win_lo;
                    while (lo < local &&
                           s.win[static_cast<std::size_t>(lo - s.win_lo)].ov <
                               lo_val)
                        ++lo;
                }
                hi = s.win_hi;
            } else if (!w.range) {
                lo = w.start ? 0 : std::max<std::int64_t>(0, local - w.before);
                hi = std::min(local + w.after, end);
            } else if (row.miss[r]) {
                if (s.first_missing < 0) {
                    s.first_missing = local;
                    if (!keep) {
                        s.win_count = 0;
                        s.win_isum = steps::FrameSumInt{};
                        s.has_best = false;
                    }
                }
                lo = std::max(s.win_lo, s.first_missing);
                hi = end;
            } else {
                const double ov = row.ov[r];
                const double lo_val = w.start ? -inf : ov - w.pre;
                const double hi_val = ov + w.fol;
                lo = s.win_lo;
                if (keep)
                    while (lo < local) {
                        const SpecState::Cell& c =
                            s.win[static_cast<std::size_t>(lo - s.win_lo)];
                        if (c.miss || !(c.ov < lo_val)) break;
                        ++lo;
                    }
                hi = std::max(s.win_hi, local);
                while (hi + 1 <= end && !row.miss[at(hi + 1)] &&
                       row.ov[at(hi + 1)] <= hi_val)
                    ++hi;
            }
            for (std::int64_t q = s.win_hi + 1; q <= hi; ++q)
                enter(q, cell_at(at(q)));
            if (keep)
                for (std::int64_t q = s.win_lo; q < lo; ++q) leave();
            s.win_lo = lo;
            s.win_hi = hi;

            const std::int64_t cnt = s.win_count;
            if (count_fn) {
                bits[r] = static_cast<std::uint64_t>(cnt);
            } else if (var_fn) {
                if (cnt < 2 || cnt < w.min_count) {
                    okv[r] = 0;
                } else {
                    const bool constant =
                        w.to_end ? s.win.front().bits == 0 : s.apart == 0;
                    const double var = constant
                                           ? 0.0
                                           : std::max(s.m2, 0.0) /
                                                 static_cast<double>(cnt - 1);
                    bits[r] = double_bits(
                        w.func == DFTU_WINDOW_FRAME_STD ? std::sqrt(var) : var);
                }
            } else if (cnt == 0 || cnt < w.min_count) {
                okv[r] = 0;
            } else if (suffix) {
                src[r] = hist_len + ri - std::min(w.before, local);
            } else if (mean_fn) {
                bits[r] = double_bits(s.win_sum.mean(cnt));
            } else if (float_acc) {
                bits[r] = double_bits(s.win_sum.sum);
            } else if (sum_fn) {
                bits[r] = cls == NumClass::UNSIGNED
                              ? s.win_isum.u64()
                              : static_cast<std::uint64_t>(s.win_isum.i64());
            } else {
                bits[r] = s.best;
            }
        }
        if (suffix) {
            const Series all =
                concat_columns({&hist, &cols[s.ext_at]}).materialize();
            put(res, w.out, all.take(src).materialize());
            const auto rows = static_cast<std::int64_t>(emit);
            const std::int64_t keep_n = std::min(w.before, hist_len - 1 + rows);
            std::vector<std::int64_t> idx{-1};
            for (std::int64_t i = 0; i < keep_n; ++i)
                idx.push_back(hist_len + rows - keep_n + i);
            s.hist = all.take(idx).materialize();
            s.have_hist = true;
            return;
        }
        if (count_fn)
            put(res, w.out,
                make_numeric(bits, okv, NumClass::SIGNED, TypeId::Int64));
        else if (var_fn || float_acc)
            put(res, w.out,
                make_numeric(bits, okv, NumClass::FLOAT, TypeId::Float64));
        else if (sum_fn)
            put(res, w.out,
                cls == NumClass::UNSIGNED
                    ? make_numeric(bits, okv, NumClass::UNSIGNED,
                                   TypeId::Uint64)
                    : make_numeric(bits, okv, NumClass::SIGNED, TypeId::Int64));
        else
            put(res, w.out, make_numeric(bits, okv, cls, vcol.type()));
    }

    // A min or max of a text column from the partition start to the row, or
    // to a few rows after it: the best row entered so far, a later equal value
    // taking its place as in the window kernel. A row of this chunk is read
    // from it, and the best of the rows before the cut from the copy carried
    // across it. A row that a frame reaches ahead of the emitted ones is
    // entered now and not again in the next chunk (`win_hi` is the position of
    // the last one entered).
    void native_extreme(SpecState& s, const std::vector<Series>& cols,
                        DataFrame& res, std::size_t emit, const RowInfo& row) {
        const WindowSpecInfo& w = s.info;
        const Series& vcol = cols[static_cast<std::size_t>(w.value)];
        const bool smallest = w.func == DFTU_WINDOW_FRAME_MIN;
        const ColumnView view(vcol);
        const std::vector<std::int64_t> null_row{-1};
        const Series carried = s.have_ext ? s.ext.share() : vcol.take(null_row);
        const ColumnView carried_view(carried);
        // 0 is the carried row, 1 + i the row i of the chunk, -1 a null.
        std::vector<std::int64_t> src(emit, -1);
        bool have = s.have_ext;
        std::int64_t best = -1;  // the chunk row of the best, -1 the carried
        std::int64_t count = s.ext_count;
        std::int64_t hi = s.win_hi;
        for (std::size_t r = 0; r < emit; ++r) {
            const auto ri = static_cast<std::int64_t>(r);
            if (row.new_part[r]) {
                have = false;
                best = -1;
                count = 0;
                hi = -1;
            }
            const std::int64_t local = row.pos[r] - 1;
            const std::int64_t to =
                local + std::min(w.after, row.run_last[r] - ri);
            // Rows (hi, to]; written without hi + 1, which GCC 12 reports
            // under -Wstrict-overflow when it folds the bound with the max.
            for (; hi < to;) {
                ++hi;
                const std::int64_t at = ri + (hi - local);
                if (vcol.is_null(at)) continue;
                ++count;
                const int c = !have       ? 0
                              : best >= 0 ? view.compare(at, best)
                                          : view.compare(at, carried_view, 0);
                if (!have || (smallest ? c <= 0 : c >= 0)) {
                    best = at;
                    have = true;
                }
            }
            if (have && count >= w.min_count) src[r] = best >= 0 ? 1 + best : 0;
        }
        const Series all = concat_columns({&carried, &vcol}).materialize();
        put(res, w.out, all.take(src).materialize());
        s.have_ext = have;
        s.ext_count = count;
        s.win_hi = hi;
        if (best >= 0)
            s.ext = vcol.take(std::vector<std::int64_t>{best}).materialize();
    }

    // first_value and nth_value: the row at one position of the partition. A
    // row of this chunk is read from it, and an earlier one from the copy
    // carried across the cut.
    void native_pick(SpecState& s, const std::vector<Series>& cols,
                     DataFrame& res, std::size_t emit, const RowInfo& row) {
        const WindowSpecInfo& w = s.info;
        const Series& vcol = cols[static_cast<std::size_t>(w.value)];
        const std::int64_t k =
            w.func == DFTU_WINDOW_FIRST_VALUE ? std::int64_t{1} : w.nth;
        const std::vector<std::int64_t> null_row{-1};
        const Series carried =
            s.have_pick ? s.pick.share() : vcol.take(null_row);
        // 0 is the carried row, 1 + i the row i of the chunk, -1 a null.
        std::vector<std::int64_t> src(emit, -1);
        std::int64_t pick_at = -1;
        for (std::size_t r = 0; r < emit && k >= 1; ++r) {
            const auto ri = static_cast<std::int64_t>(r);
            const std::int64_t at = ri + (k - 1) - (row.pos[r] - 1);
            if (at < 0)
                src[r] = s.have_pick ? 0 : -1;
            else if (at <= row.run_last[r])
                src[r] = 1 + at;
            if (r + 1 == emit) pick_at = at;
        }
        const Series all = concat_columns({&carried, &vcol}).materialize();
        put(res, w.out, all.take(src).materialize());
        if (emit == 0) return;
        const auto last = static_cast<std::int64_t>(emit) - 1;
        if (row.pos[emit - 1] - 1 <= last) s.have_pick = false;
        if (k >= 1 && pick_at >= 0 && pick_at <= last) {
            s.pick =
                vcol.take(std::vector<std::int64_t>{pick_at}).materialize();
            s.have_pick = true;
        }
    }

    // last_value: the last row of the partition, which the figures collected
    // in sorted order hold.
    void native_last(SpecState& s, const std::vector<Series>& cols,
                     DataFrame& res, std::size_t emit, const RowInfo& row) {
        (void)cols;
        const WindowSpecInfo& w = s.info;
        std::vector<const Series*> pieces;
        std::vector<std::int64_t> src(emit, 0);
        const PartitionStats::Part* prev = nullptr;
        for (std::size_t r = 0; r < emit; ++r) {
            if (row.part_of[r] != prev) {
                prev = row.part_of[r];
                pieces.push_back(&prev->last[static_cast<std::size_t>(s.slot)]);
            }
            src[r] = static_cast<std::int64_t>(pieces.size()) - 1;
        }
        if (pieces.empty()) return;
        put(res, w.out, concat_columns(pieces).take(src).materialize());
    }

    // The state after a chunk: the rows of the continuing partition are
    // corrected for the rows before the cut, then the context for the next
    // chunk is taken. `cols` are the chunk's rows, `fcols` the kernel input
    // (context first), `res` the emitted result.
    template <class Key>
    void carry(const std::vector<Series>& cols,
               const std::vector<Series>& fcols, DataFrame& res, const Key& key,
               std::int64_t n, std::int64_t emit) {
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
                const std::vector<Series> ck = key_columns(ctx_, part_);
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
            const bool is_rank = s.kind == Kind::RANK;
            const bool is_dense = s.kind == Kind::DENSE;
            if (continuing && run > 0) {
                if (s.kind == Kind::COUNT && shift != 0) {
                    col = patched(col, run,
                                  [&](std::int64_t v) { return v + shift; });
                } else if (is_rank) {
                    col = patched(col, run, [&](std::int64_t v) {
                        return v == 1 ? s.last_i : v + shift;
                    });
                } else if (is_dense) {
                    col = patched(col, run, [&](std::int64_t v) {
                        return v - 1 + s.last_i;
                    });
                } else if (s.acc == Acc::POST_ADD) {
                    if (col.type() == TypeId::Uint64)
                        col = patched(col, run, [&](std::uint64_t v) {
                            steps::SumU64 sum{s.last_u};
                            sum.add(v);
                            return sum.acc;
                        });
                    else
                        col = patched(col, run, [&](std::int64_t v) {
                            steps::SumI64 sum{s.last_i};
                            sum.add(v);
                            return sum.acc;
                        });
                }
            }
            if (is_rank || is_dense || s.acc == Acc::POST_ADD) {
                if (col.type() == TypeId::Uint64)
                    s.last_u = col.data<std::uint64_t>()[emit - 1];
                else
                    s.last_i = col.data<std::int64_t>()[emit - 1];
            }
        }

        native(cols, res, n, emit, continuing, key);

        const std::vector<std::int64_t> last_row{emit - 1};
        const std::vector<std::int64_t> null_row{-1};
        ctx_.clear();
        if (mode_ == Carry::SEED) {
            for (std::size_t c = 0; c < sch_.size(); ++c) {
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
        } else if (range_) {
            range_context(fcols, ctx_n, emit, n);
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

    // The context of a range frame: the emitted rows of the partition the next
    // row belongs to whose order value is within the preceding bound of it.
    void range_context(const std::vector<Series>& fcols, std::int64_t ctx_n,
                       std::int64_t emit, std::int64_t n) {
        const std::int64_t total = ctx_n + emit;
        std::vector<double> ov;
        std::vector<char> miss;
        order_values(fcols[order_.front()], ov, miss);
        const std::vector<Series> pk = key_columns(fcols, part_);
        const std::int64_t target = emit < n ? total : total - 1;
        const std::string tkey = row_key(pk, target);
        std::int64_t keep = 0;
        if (row_key(pk, total - 1) == tkey) {
            const auto zt = static_cast<std::size_t>(target);
            const bool tmiss = miss[zt] != 0;
            const double thr = ov[zt] - rpre_;
            std::int64_t i = total - 1;
            while (i >= 0 && row_key(pk, i) == tkey) {
                const auto z = static_cast<std::size_t>(i);
                if (tmiss ? !miss[z] : (!miss[z] && ov[z] < thr)) break;
                --i;
            }
            keep = total - 1 - i;
        }
        // Row frames in the same window read the rows behind the cut by count.
        keep = std::max(keep, std::min(before_, total));
        ctx_rows_ = keep;
        if (keep > 0) {
            std::vector<std::int64_t> idx(static_cast<std::size_t>(keep));
            std::iota(idx.begin(), idx.end(), total - keep);
            for (const Series& c : fcols)
                ctx_.push_back(c.take(idx).materialize());
        }
    }

    std::unique_ptr<Cursor> in_;
    std::vector<std::string> sch_;
    std::vector<std::size_t> part_;
    std::vector<std::size_t> order_;
    std::string name_;
    const dftu_op_desc* op_;
    std::shared_ptr<const OwnedFrameOpArgs> args_;
    std::shared_ptr<PartitionStats> stats_;
    std::uint64_t budget_;
    std::uint64_t chunk_bytes_;
    std::vector<SpecState> specs_;
    std::vector<Suffix> suffix_;
    bool seq_ = false;
    std::unique_ptr<spill::Spool> body_;
    std::unique_ptr<spill::Spool> suffix_spool_;
    std::unique_ptr<spill::Spool> figures_;
    std::unique_ptr<Cursor> figures_reader_;
    std::deque<PartitionStats::Part> figs_buf_;
    std::vector<PartitionStats::Part> seq_parts_;
    PartitionStats::Part carry_part_;
    std::int64_t figs_need_ = 0;
    Carry mode_ = Carry::ALIGNED;
    std::int64_t before_ = 0;
    std::int64_t after_ = 0;
    bool range_ = false;
    double rpre_ = 0.0;
    double rfol_ = 0.0;
    bool need_tie_ = false;
    bool tie_cut_ = false;
    bool planned_ = false;
    std::vector<Morsel> held_;
    std::int64_t held_rows_ = 0;
    std::uint64_t held_bytes_ = 0;
    std::vector<Series> ctx_;
    std::int64_t ctx_rows_ = 0;
    std::int64_t E_ = 0;
    std::string last_key_;
    std::string last_order_key_;
    std::string why_;
    bool eof_ = false;
    bool need_more_ = false;
    std::uint64_t grow_to_ = 0;
    std::optional<std::vector<std::string>> out_names_;
};

// ---- native group-wise transforms ------------------------------------------

}  // namespace

namespace lazy_internal {

std::unique_ptr<Cursor> make_window(
    std::unique_ptr<Cursor> in, const std::vector<std::string>& sch,
    const std::vector<Field>& in_fields, const std::string& name,
    const dftu_op_desc* op, std::shared_ptr<const OwnedFrameOpArgs> args,
    std::uint64_t budget) {
    const std::vector<std::string>& part = args->strings_at(1);
    const std::vector<std::string>& order = args->strings_at(2);
    std::vector<std::string> sort_keys = part;
    for (const std::string& n : order) sort_keys.push_back(n);
    std::vector<std::size_t> part_idx, order_idx;
    for (const std::string& n : part)
        part_idx.push_back(
            static_cast<std::size_t>(require_col(sch, n, "window")));
    for (const std::string& n : order)
        order_idx.push_back(
            static_cast<std::size_t>(require_col(sch, n, "window")));
    std::vector<WindowSpecInfo> specs = window_specs(*args, sch);
    std::unique_ptr<Cursor> source = std::move(in);
    std::shared_ptr<PartitionStats> stats = std::make_shared<PartitionStats>();
    if (window_needs_stats(specs, *stats, sch, in_fields)) {
        stats->part_cols = part_idx;
        if (stats->by_order) {
            if (order_idx.size() == 1)
                stats->order_col = order_idx.front();
            else
                stats->by_order = false;
        }
        stats->cap_bytes =
            std::max<std::uint64_t>(budget / share::SMALL, 1 << 20);
        if (!stats->sorted)
            source = std::make_unique<StatsCursor>(std::move(source), stats);
    } else {
        stats.reset();
    }
    std::unique_ptr<Cursor> sorted = std::move(source);
    if (!sort_keys.empty())
        sorted =
            make_sort_merge(std::move(sorted), sch, sort_keys,
                            std::vector<bool>(sort_keys.size(), false), budget);
    return std::make_unique<WindowStreamCursor>(
        std::move(sorted), sch, std::move(part_idx), std::move(order_idx),
        std::move(specs), name, op, std::move(args), std::move(stats), budget);
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
