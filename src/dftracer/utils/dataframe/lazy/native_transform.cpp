#include <dftracer/utils/core/common/hash/splitmix64.h>
#include <dftracer/utils/dataframe/kernels/running.h>
#include <dftracer/utils/dataframe/lazy/cursors.h>
#include <dftracer/utils/dataframe/lazy/numeric.h>

#include <array>
#include <atomic>
#include <memory>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace {

// A value of a group's column and whether it is present.
struct RingCell {
    std::uint64_t bits = 0;
    std::uint8_t ok = 0;
};

// The state of one value column in one group: the step of the transform's
// kind (the kind and the column's class say which member is live) and what the
// steps that look at the row before need.
struct TransformCell {
    union Step {
        Step() {}  // start() constructs the member a kind and class use
        steps::Extreme ext;
        steps::SumI64 sum_i;
        steps::SumU64 sum_u;
        steps::SumF64 sum_f;
        steps::Prod prod;
        steps::FillForward<std::uint64_t> fill;
    } step;
    std::uint64_t last = 0;  // the previous row's value
    std::int64_t seen = 0;   // rows of the group so far
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
        classes_.resize(in_.size());
        for (std::size_t c = 0; c < in_.size(); ++c) {
            if (!num_class(cols[in_[c]].type(), classes_[c]))
                throw std::logic_error("group transform: a non-numeric column");
        }
        for (std::size_t c = 0; c < in_.size(); ++c) {
            const NumClass cls = classes_[c];
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
                    start(st, cls);
                    if (shift_ > 0) ring_of(g, c) = steps::Lag<RingCell>{};
                }
                produced = step(st, g, c, cls, cells.bits[r], cells.ok[r] != 0,
                                ob[r], ook[r]);
            }
            out.push_back(make_numeric(ob, ook, produced, declared_[c]));
        }
        return out;
    }

   private:
    void ensure(std::size_t g) {
        if (g < counts_.size()) return;
        grow(g);
    }

    // The slow path of ensure, kept out of line so the per-row check stays
    // small: a new group gets every cell's step member constructed once, so a
    // row never checks.
    [[gnu::noinline]] void grow(std::size_t g) {
        const std::size_t before = cells_.size();
        counts_.resize(g + 1, 0);
        cells_.resize((g + 1) * in_.size());
        for (std::size_t i = before; i < cells_.size(); ++i)
            start(cells_[i], classes_[i % in_.size()]);
        if (shift_ > 0) rings_.resize((g + 1) * in_.size());
    }
    TransformCell& cell(std::size_t g, std::size_t c) {
        return cells_[g * in_.size() + c];
    }
    steps::Lag<RingCell>& ring_of(std::size_t g, std::size_t c) {
        return rings_[g * in_.size() + c];
    }

    // Constructs the member of `st.step` that this kind and class use.
    void start(TransformCell& st, NumClass cls) const {
        TransformCell::Step& s = st.step;
        switch (kind_) {
            case GroupwiseOp::CumSum:
                if (cls == NumClass::SIGNED)
                    std::construct_at(&s.sum_i);
                else if (cls == NumClass::UNSIGNED)
                    std::construct_at(&s.sum_u);
                else
                    std::construct_at(&s.sum_f);
                break;
            case GroupwiseOp::CumProd:
                std::construct_at(&s.prod);
                break;
            case GroupwiseOp::CumMax:
            case GroupwiseOp::CumMin:
                std::construct_at(&s.ext);
                break;
            case GroupwiseOp::FFill:
                std::construct_at(&s.fill);
                break;
            default:
                break;
        }
    }

    // One row of one column; returns the class of the value it produced. The
    // kernels are the shared steps; what stays here is the window plan's
    // post-step: a null input gives a null output for the running ops, and
    // `+ (x - x)` turns an infinite or NaN float into NaN.
    NumClass step(TransformCell& st, std::size_t g, std::size_t c, NumClass cls,
                  std::uint64_t b, bool ok, std::uint64_t& out,
                  std::uint8_t& out_ok) {
        const double x = cls == NumClass::FLOAT ? bits_double(b) : 0.0;
        TransformCell::Step& s = st.step;
        switch (kind_) {
            case GroupwiseOp::CumSum: {
                if (!ok) {
                    out_ok = 0;
                    return cls;
                }
                if (cls == NumClass::SIGNED) {
                    s.sum_i.add(static_cast<std::int64_t>(b));
                    out = static_cast<std::uint64_t>(s.sum_i.acc);
                } else if (cls == NumClass::UNSIGNED) {
                    s.sum_u.add(b);
                    out = s.sum_u.acc;
                } else {
                    s.sum_f.add(x);
                    out = double_bits(s.sum_f.acc + (x - x));
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
                s.prod.mul(v);
                const double d = cls == NumClass::FLOAT ? (x - x) : 0.0;
                out = double_bits(s.prod.acc + d);
                return NumClass::FLOAT;
            }
            case GroupwiseOp::CumMax:
            case GroupwiseOp::CumMin: {
                if (!ok) {
                    out_ok = 0;
                    return cls;
                }
                const bool smallest = kind_ == GroupwiseOp::CumMin;
                if (cls == NumClass::SIGNED) {
                    s.ext.offer_i(static_cast<std::int64_t>(b), smallest);
                    out = static_cast<std::uint64_t>(s.ext.value.i);
                } else if (cls == NumClass::UNSIGNED) {
                    s.ext.offer_u(b, smallest);
                    out = s.ext.value.u;
                } else {
                    s.ext.offer_f(x, smallest);
                    out = double_bits(s.ext.value.d + (x - x));
                }
                return cls;
            }
            case GroupwiseOp::Shift: {
                if (shift_ == 0) {
                    out = b;
                    out_ok = ok ? 1 : 0;
                    return cls;
                }
                RingCell back;
                const RingCell in{b, static_cast<std::uint8_t>(ok ? 1 : 0)};
                if (ring_of(g, c).step(shift_, in, back) && back.ok)
                    out = back.bits;
                else
                    out_ok = 0;
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
                    out = double_bits(steps::delta_f64(x, bits_double(prev)));
                    return NumClass::FLOAT;
                }
                out = static_cast<std::uint64_t>(
                    cls == NumClass::SIGNED
                        ? steps::delta_i64(static_cast<std::int64_t>(b),
                                           static_cast<std::int64_t>(prev))
                        : steps::delta_u64(b, prev));
                return NumClass::SIGNED;
            }
            case GroupwiseOp::FFill: {
                if (!s.fill.step(ok, b, out)) out_ok = 0;
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
    std::vector<NumClass> classes_;
    std::vector<TransformCell> cells_;
    std::vector<steps::Lag<RingCell>> rings_;
};

// A float key groups by value: negative zero is zero and every NaN is one
// value. `bits` are the bits of the double.
inline std::uint64_t canonical_float_bits(std::uint64_t bits) {
    double v = bits_double(bits);
    if (v == 0) v = 0.0;
    if (std::isnan(v)) v = std::numeric_limits<double>::quiet_NaN();
    return double_bits(v);
}

// Rows grouped on one numeric key column with no key string, for tests to see
// that the fast path ran.
std::atomic<std::uint64_t> g_single_numeric_key_rows{0};

// Gives each row of the key columns a group id (hash mode) or marks the rows
// that start a run of equal keys (sorted mode). Up to four numeric key columns
// and one string key column are grouped on the cell values with no key string;
// any other keys build one. Floats group by value: negative zero is zero and
// every NaN is one value. A null is its own value.
class GroupKeys {
   public:
    explicit GroupKeys(bool sorted) : sorted_(sorted) {}

    std::size_t groups() const { return static_cast<std::size_t>(groups_); }

    // `gid` gets the id of each row (hash mode), `starts` a 1 for each row
    // that starts a run (sorted mode). Each has `rows` entries.
    void add(const std::vector<Series>& cols, std::int64_t rows,
             std::int32_t* gid, std::uint8_t* starts) {
        const auto n = static_cast<std::size_t>(rows);
        if (cols.size() == 1 && numeric(cols))
            add_numeric1(cols[0], n, gid, starts);
        else if (cols.size() <= MAX_NUMERIC && numeric(cols))
            add_numeric(cols, n, gid, starts);
        else if (cols.size() == 1 &&
                 value_domain(cols[0].type()) == ValueDomain::Bytes)
            add_bytes(cols[0], n, gid, starts);
        else
            add_text(cols, n, gid, starts);
    }

   private:
    static constexpr std::size_t MAX_NUMERIC = 4;

    struct Wide {
        std::array<std::uint64_t, MAX_NUMERIC + 1> w{};
        bool operator==(const Wide& o) const { return w == o.w; }
    };
    struct WideHash {
        using is_avalanching = void;
        std::size_t operator()(const Wide& k) const noexcept {
            std::uint64_t h = 0;
            for (std::uint64_t x : k.w) h = hash::splitmix64(h ^ x);
            return static_cast<std::size_t>(h);
        }
    };

    static bool numeric(const std::vector<Series>& cols) {
        NumClass cls = NumClass::SIGNED;
        for (const Series& c : cols)
            if (!num_class(c.type(), cls)) return false;
        return !cols.empty();
    }

    void start(bool is_start, std::size_t r, std::uint8_t* starts) {
        starts[r] = is_start ? 1 : 0;
        have_last_ = true;
    }

    // One numeric key column: the key is its 64-bit value, with a flag for a
    // null.
    void add_numeric1(const Series& col, std::size_t n, std::int32_t* gid,
                      std::uint8_t* starts) {
        NumClass cls = NumClass::SIGNED;
        num_class(col.type(), cls);
        const NumCells cells = read_cells(col);
        const bool is_float = cls == NumClass::FLOAT;
        g_single_numeric_key_rows.fetch_add(n, std::memory_order_relaxed);
        for (std::size_t r = 0; r < n; ++r) {
            const bool ok = cells.ok[r] != 0;
            std::uint64_t bits = cells.bits[r];
            if (is_float) bits = canonical_float_bits(bits);
            if (sorted_) {
                start(
                    !have_last_ || ok != last_ok_ || (ok && bits != last_bits_),
                    r, starts);
                last_ok_ = ok;
                last_bits_ = bits;
            } else if (!ok) {
                if (null_id_ < 0) null_id_ = groups_++;
                gid[r] = null_id_;
            } else {
                auto it = word_ids_.find(bits);
                if (it == word_ids_.end())
                    it = word_ids_.emplace(bits, groups_++).first;
                gid[r] = it->second;
            }
        }
    }

    void add_numeric(const std::vector<Series>& cols, std::size_t n,
                     std::int32_t* gid, std::uint8_t* starts) {
        std::vector<NumCells> cells;
        std::vector<NumClass> cls(cols.size(), NumClass::SIGNED);
        cells.reserve(cols.size());
        for (std::size_t c = 0; c < cols.size(); ++c) {
            num_class(cols[c].type(), cls[c]);
            cells.push_back(read_cells(cols[c]));
        }
        for (std::size_t r = 0; r < n; ++r) {
            Wide k;
            for (std::size_t c = 0; c < cols.size(); ++c) {
                if (!cells[c].ok[r]) {
                    k.w[MAX_NUMERIC] |= std::uint64_t{1} << c;
                    continue;
                }
                std::uint64_t bits = cells[c].bits[r];
                if (cls[c] == NumClass::FLOAT)
                    bits = canonical_float_bits(bits);
                k.w[c] = bits;
            }
            if (sorted_) {
                start(!have_last_ || !(k == last_wide_), r, starts);
                last_wide_ = k;
            } else {
                auto it = wide_ids_.find(k);
                if (it == wide_ids_.end())
                    it = wide_ids_.emplace(k, groups_++).first;
                gid[r] = it->second;
            }
        }
    }

    void add_bytes(const Series& col, std::size_t n, std::int32_t* gid,
                   std::uint8_t* starts) {
        for (std::size_t r = 0; r < n; ++r) {
            const auto i = static_cast<std::int64_t>(r);
            const bool null = col.is_null(i);
            const std::string_view v =
                null ? std::string_view() : read_bytes(col, i);
            if (sorted_) {
                const bool same = have_last_ && null == last_null_ &&
                                  (null || v == last_bytes_);
                start(!same, r, starts);
                last_null_ = null;
                if (!null && !same) last_bytes_.assign(v);
            } else if (null) {
                if (null_id_ < 0) null_id_ = groups_++;
                gid[r] = null_id_;
            } else {
                auto it = byte_ids_.find(v);
                if (it == byte_ids_.end())
                    it = byte_ids_.emplace(std::string(v), groups_++).first;
                gid[r] = it->second;
            }
        }
    }

    void add_text(const std::vector<Series>& cols, std::size_t n,
                  std::int32_t* gid, std::uint8_t* starts) {
        for (std::size_t r = 0; r < n; ++r) {
            std::string key = row_key(cols, static_cast<std::int64_t>(r));
            if (sorted_) {
                start(!have_last_ || key != last_text_, r, starts);
                last_text_ = std::move(key);
            } else {
                auto it = text_ids_.find(key);
                if (it == text_ids_.end())
                    it = text_ids_.emplace(std::move(key), groups_++).first;
                gid[r] = it->second;
            }
        }
    }

    bool sorted_;
    std::int32_t groups_ = 0;
    std::int32_t null_id_ = -1;
    bool have_last_ = false;
    bool last_null_ = false;
    bool last_ok_ = false;
    std::uint64_t last_bits_ = 0;
    Wide last_wide_;
    std::string last_bytes_;
    std::string last_text_;
    ankerl::unordered_dense::map<std::uint64_t, std::int32_t> word_ids_;
    ankerl::unordered_dense::map<Wide, std::int32_t, WideHash> wide_ids_;
    StringViewMap<std::int32_t> byte_ids_;
    ankerl::unordered_dense::map<std::string, std::int32_t> text_ids_;
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
          core_(std::move(core)),
          keys_state_(sorted) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        while (true) {
            auto m = co_await in_->next(max_rows);
            if (!m) co_return std::nullopt;
            if (m->rows == 0) continue;
            const auto n = static_cast<std::size_t>(m->rows);
            std::vector<std::int32_t> gid(n, 0);
            std::vector<std::uint8_t> starts;
            if (!keys_.empty()) {
                starts.assign(n, 0);
                keys_state_.add(key_columns(m->columns, keys_), m->rows,
                                gid.data(), starts.data());
                if (!sorted_) starts.clear();
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
    GroupKeys keys_state_;
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
            std::max<std::uint64_t>(1, (budget_ / share::CHUNK) / per_group);
        spool_ = std::make_shared<spill::Spool>(budget_ / share::SPOOL);
        spill::Spool& spool = *spool_;
        GroupKeys groups(false);
        bool too_many = false;
        std::int64_t row = 0;
        while (auto m = co_await in_->next(max_rows)) {
            if (m->rows == 0) continue;
            if (!too_many && !spool.spilled()) {
                std::vector<std::int32_t> scratch(
                    static_cast<std::size_t>(m->rows));
                groups.add(key_columns(m->columns, keys_), m->rows,
                           scratch.data(), nullptr);
                too_many = groups.groups() > cap_groups;
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
        auto sorted = make_sort_merge(
            spool.reader(), names, sort_keys,
            std::vector<bool>(sort_keys.size(), false), budget_);
        std::vector<std::string> pass_names = out_names_;
        pass_names.emplace_back(ROW_COLUMN);
        auto pass = std::make_unique<TransformPassCursor>(
            std::move(sorted), keys_, true, true, core());
        auto restored = make_sort_merge(std::move(pass), pass_names,
                                        std::vector<std::string>{ROW_COLUMN},
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

}  // namespace

std::uint64_t native_transform_single_key_rows() {
    return g_single_numeric_key_rows.load(std::memory_order_relaxed);
}

namespace lazy_internal {

std::unique_ptr<Cursor> make_native_transform(
    std::unique_ptr<Cursor> in, const std::vector<std::string>& sch,
    const OwnedFrameOpArgs& args, const std::vector<std::string>& out_names,
    const std::vector<DataType>& out_types, std::uint64_t budget) {
    std::vector<std::size_t> keys, value;
    for (const std::string& name : args.strings_at(1))
        keys.push_back(
            static_cast<std::size_t>(require_col(sch, name, "group_by")));
    const auto kind = static_cast<GroupwiseOp>(args.i32_at(2));
    std::vector<TypeId> declared;
    for (std::size_t i = 0; i < out_names.size(); ++i) {
        declared.push_back(out_types[i].id);
        if (kind == GroupwiseOp::CumCount) continue;
        value.push_back(static_cast<std::size_t>(
            require_col(sch, out_names[i], "group_by")));
    }
    return std::make_unique<NativeTransformCursor>(
        std::move(in), sch, std::move(keys), kind, args.i64_at(3),
        std::move(value), std::move(declared), out_names, budget);
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
