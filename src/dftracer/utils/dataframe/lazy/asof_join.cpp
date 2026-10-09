#include <dftracer/utils/dataframe/kernels/order.h>
#include <dftracer/utils/dataframe/lazy/cursors.h>
#include <dftracer/utils/dataframe/ops/ops_common.h>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace {

// One sorted morsel of the right side, or a compact copy of the rows an
// interval join admitted from one. The views read the columns of `cols`.
struct Seg {
    std::vector<Series> cols;
    std::vector<ColumnView> keys;
    std::optional<ColumnView> t;   // asof time, or interval lo
    std::optional<ColumnView> hi;  // interval hi
    std::int64_t rows = 0;
    std::int64_t live = 0;
    std::uint64_t bytes = 0;
};
using SegPtr = std::shared_ptr<Seg>;

struct Held {
    SegPtr seg;
    std::int64_t i = -1;
};

struct Ref {
    const Seg* seg = nullptr;
    std::int64_t i = -1;
};

struct Layout {
    std::vector<std::size_t> lby;
    std::vector<std::size_t> rby;
    std::size_t lt = 0;
    std::size_t rt = 0;
    std::size_t rhi = 0;
    bool has_hi = false;
    std::vector<std::size_t> rout;
    std::vector<Field> rfields;
};

struct LeftBlock {
    Morsel m;
    std::vector<ColumnView> by;
    std::optional<ColumnView> t;
};

bool is_bytes_type(TypeId t) {
    const TypeId p = physical_type(t);
    return p == TypeId::String || p == TypeId::Binary ||
           p == TypeId::LargeString || p == TypeId::LargeBinary;
}

const Field* field_named(const std::vector<Field>& fs, const std::string& n) {
    const auto it = std::find_if(fs.begin(), fs.end(),
                                 [&](const Field& f) { return f.name == n; });
    return it == fs.end() ? nullptr : &*it;
}

// The type checks of the eager ops, over the fields both plans declare. A type
// that is not known yet (Unknown) is checked when the first morsels arrive.
void check_field_types(const std::vector<Field>& lf,
                       const std::vector<Field>& rf, const std::string& who,
                       const std::string& lt_name, const std::string& rt_name,
                       const std::vector<std::string>& by,
                       const std::vector<std::string>& right_also) {
    const auto known = [](const Field* f) {
        return f && f->type.id != TypeId::Unknown;
    };
    const Field* lt = field_named(lf, lt_name);
    const Field* rt = field_named(rf, rt_name);
    if (known(lt) && known(rt) && lt->type.id != rt->type.id)
        throw std::invalid_argument(who + ": point/ts column types differ");
    for (const std::string& n : right_also) {
        const Field* f = field_named(rf, n);
        if (known(lt) && known(f) && f->type.id != lt->type.id)
            throw std::invalid_argument(who + ": point/lo/hi types differ");
    }
    for (const std::string& n : by) {
        const Field* l = field_named(lf, n);
        const Field* r = field_named(rf, n);
        if (known(l) && known(r) && l->type.id != r->type.id)
            throw std::invalid_argument(who + ": equi column types differ");
        for (const Field* f : {l, r})
            if (known(f) && !ColumnView::supports(f->type.id))
                throw std::invalid_argument(
                    who + ": column '" + n + "' of type " +
                    std::string(type_name(f->type.id)) + " has no order");
    }
    if (known(lt) &&
        (!ColumnView::supports(lt->type.id) || is_bytes_type(lt->type.id)))
        throw std::invalid_argument(who + ": time columns must be numeric");
}

// The names of the left columns, then of the right columns not in `skip`; a
// right name that is taken gets "_right", and a second clash throws.
std::vector<std::string> joined_names(const std::vector<std::string>& left,
                                      const std::vector<std::string>& right,
                                      const std::vector<std::string>& skip,
                                      const std::string& who) {
    std::vector<std::string> out = left;
    const auto taken = [&](const std::string& n) {
        return std::find(out.begin(), out.end(), n) != out.end();
    };
    for (const std::string& r : right) {
        if (std::find(skip.begin(), skip.end(), r) != skip.end()) continue;
        std::string name = r;
        if (taken(name)) {
            name += "_right";
            if (taken(name))
                throw std::invalid_argument(
                    who + ": output column name collision: " + name);
        }
        out.push_back(std::move(name));
    }
    return out;
}

// Reads the right side in sorted order, one morsel at a time, as Segs.
class RightFeed {
   public:
    RightFeed(LazyFrame sorted, const Layout& lay,
              std::vector<std::string> rsch)
        : plan_(std::move(sorted)), lay_(lay), rsch_(std::move(rsch)) {}

    coro::CoroTask<bool> fill(std::int64_t max_rows) {
        while (!seg_ || i_ >= seg_->rows) {
            if (done_) co_return false;
            if (!gen_) gen_.emplace(plan_.stream(max_rows));
            auto df = co_await gen_->next();
            if (!df) {
                done_ = true;
                seg_.reset();
                if (proto_.empty()) empty_proto();
                co_return false;
            }
            std::vector<Series> cols = ordered(*df);
            if (proto_.empty()) {
                for (std::size_t c : lay_.rout)
                    proto_.push_back(cols[c].take(std::vector<std::int64_t>{}));
            }
            if (df->num_rows() == 0) continue;
            seg_ = make_seg(std::move(cols), df->num_rows());
            i_ = 0;
        }
        co_return true;
    }

    const SegPtr& seg() const { return seg_; }
    std::int64_t i() const { return i_; }
    void advance() { ++i_; }
    const std::vector<Series>& proto() const { return proto_; }
    std::uint64_t row_bytes() const {
        return seg_ && seg_->rows > 0
                   ? seg_->bytes / static_cast<std::uint64_t>(seg_->rows)
                   : 0;
    }

   private:
    std::vector<Series> ordered(const DataFrame& df) {
        if (map_.empty()) {
            for (const std::string& n : rsch_) {
                const int at = column_index_of(df.names, n);
                if (at < 0)
                    throw std::runtime_error("asof: the right plan has no '" +
                                             n + "' column");
                map_.push_back(static_cast<std::size_t>(at));
            }
        }
        std::vector<Series> cols;
        cols.reserve(map_.size());
        for (std::size_t at : map_)
            cols.push_back(df.columns[at].is_flat()
                               ? df.columns[at].share()
                               : df.columns[at].materialize());
        return cols;
    }

    SegPtr make_seg(std::vector<Series> cols, std::int64_t rows) const {
        auto s = std::make_shared<Seg>();
        s->rows = rows;
        s->bytes = spill::columns_bytes(cols);
        for (std::size_t c : lay_.rby) s->keys.emplace_back(cols[c]);
        s->t.emplace(cols[lay_.rt]);
        if (lay_.has_hi) s->hi.emplace(cols[lay_.rhi]);
        s->cols = std::move(cols);
        return s;
    }

    void empty_proto() {
        for (std::size_t c : lay_.rout)
            proto_.push_back(null_column_of(lay_.rfields[c], 0, "asof",
                                            "the right side has no rows"));
    }

    LazyFrame plan_;
    Layout lay_;
    std::vector<std::string> rsch_;
    std::vector<std::size_t> map_;
    std::optional<coro::AsyncGenerator<DataFrame>> gen_;
    SegPtr seg_;
    std::vector<Series> proto_;
    std::int64_t i_ = 0;
    bool done_ = false;
};

// The shared frame of the two joins: both sides arrive sorted by (by, time),
// the left row by row, the right through RightFeed. A subclass decides, for
// each left row, which right rows pair with it.
class OrderedJoin : public Cursor {
   public:
    OrderedJoin(std::unique_ptr<Cursor> left, LazyFrame right_sorted,
                const Layout& lay, std::vector<std::string> rsch,
                std::vector<std::string> names, std::uint64_t budget,
                std::string who)
        : left_(std::move(left)),
          feed_(std::move(right_sorted), lay, std::move(rsch)),
          lay_(lay),
          names_(std::move(names)),
          budget_(budget),
          who_(std::move(who)) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        max_rows_ = max_rows;
        for (;;) {
            if (lb_ && (busy() || li_ < lb_->m.rows) && !full()) {
                co_await step();
                continue;
            }
            if (!out_left_.empty()) co_return flush();
            if (lb_) {
                lb_.reset();
                continue;
            }
            if (!co_await load_left()) co_return std::nullopt;
        }
    }

    std::optional<std::vector<std::string>> out_names() const override {
        return names_;
    }

   protected:
    virtual coro::CoroTask<void> step() = 0;
    virtual bool busy() const { return false; }
    virtual bool full() const { return out_left_.size() >= row_cap_; }
    virtual void after_flush() {}

    // Whether left row `r` starts a new by-group; if so the right side moves
    // to the first row whose group is not before it.
    coro::CoroTask<bool> enter_group(std::int64_t r) {
        const LeftBlock& L = *lb_;
        if (rep_ && ops::compare_keys(L.by, r, rep_->by, rep_row_) == 0)
            co_return false;
        rep_ = lb_;
        rep_row_ = r;
        while (co_await feed_.fill(max_rows_)) {
            const Seg& s = *feed_.seg();
            if (ops::compare_keys(L.by, r, s.keys, feed_.i()) <= 0) break;
            feed_.advance();
        }
        co_return true;
    }

    bool same_group(const LeftBlock& L, std::int64_t r, const Seg& s,
                    std::int64_t j) const {
        return ops::compare_keys(L.by, r, s.keys, j) == 0;
    }

    void push_row(std::int64_t r, Ref ref) {
        if (left_contig_ && !out_left_.empty() && out_left_.back() + 1 != r)
            left_contig_ = false;
        out_left_.push_back(r);
        out_ref_.push_back(ref);
    }

    void pin(const SegPtr& seg) {
        if (!pinned_.empty() && pinned_.back() == seg) return;
        if (std::find(pinned_.begin(), pinned_.end(), seg) != pinned_.end())
            return;
        pinned_.push_back(seg);
        pinned_bytes_ += seg->bytes;
    }

    coro::CoroTask<bool> load_left() {
        for (;;) {
            auto m = co_await left_->next(max_rows_);
            if (!m) co_return false;
            if (m->rows == 0) continue;
            auto b = std::make_shared<LeftBlock>();
            for (std::size_t c : lay_.lby) b->by.emplace_back(m->columns[c]);
            b->t.emplace(m->columns[lay_.lt]);
            b->m = std::move(*m);
            lb_ = std::move(b);
            li_ = 0;
            if (!checked_ && co_await feed_.fill(max_rows_)) {
                check_types(*lb_, *feed_.seg());
                checked_ = true;
            }
            const std::uint64_t lrow = spill::columns_bytes(lb_->m.columns) /
                                       static_cast<std::uint64_t>(lb_->m.rows);
            row_cap_ = static_cast<std::size_t>(rows_within(
                output_share(budget_),
                lrow + std::max<std::uint64_t>(feed_.row_bytes(), 16),
                max_rows_));
            pin_cap_ = output_share(budget_);
            co_return true;
        }
    }

    void check_types(const LeftBlock& L, const Seg& s) const {
        if (L.t->type() != s.t->type())
            throw std::invalid_argument(who_ +
                                        ": point/ts column types differ");
        for (std::size_t k = 0; k < L.by.size(); ++k)
            if (L.by[k].type() != s.keys[k].type())
                throw std::invalid_argument(who_ +
                                            ": equi column types differ");
        if (L.t->is_bytes())
            throw std::invalid_argument(who_ +
                                        ": time columns must be numeric");
    }

    Morsel flush() {
        const std::size_t n = out_left_.size();
        Morsel out;
        out.rows = static_cast<std::int64_t>(n);
        const Morsel& lm = lb_->m;
        if (left_contig_) {
            Morsel part = slice_morsel(lm, out_left_.front(), out.rows);
            out.columns = std::move(part.columns);
        } else {
            for (const Series& c : lm.columns)
                out.columns.push_back(c.take(out_left_));
        }
        for (Series& c : gather()) out.columns.push_back(std::move(c));
        out_left_.clear();
        out_ref_.clear();
        pinned_.clear();
        pinned_bytes_ = 0;
        left_contig_ = true;
        after_flush();
        return out;
    }

    std::vector<Series> gather() const {
        const std::size_t n = out_ref_.size();
        std::vector<const Seg*> segs;
        std::vector<std::int64_t> which(n, -1);
        const Seg* last = nullptr;
        std::int64_t last_id = -1;
        for (std::size_t j = 0; j < n; ++j) {
            const Seg* s = out_ref_[j].seg;
            if (!s) continue;
            if (s != last) {
                const auto it = std::find(segs.begin(), segs.end(), s);
                last_id = it - segs.begin();
                if (it == segs.end()) segs.push_back(s);
                last = s;
            }
            which[j] = last_id;
        }
        std::vector<Series> out;
        out.reserve(lay_.rout.size());
        if (segs.size() <= 1) {
            std::vector<std::int64_t> idx(n);
            for (std::size_t j = 0; j < n; ++j)
                idx[j] = which[j] < 0 ? -1 : out_ref_[j].i;
            for (std::size_t k = 0; k < lay_.rout.size(); ++k)
                out.push_back(segs.empty()
                                  ? feed_.proto()[k].take(idx)
                                  : segs[0]->cols[lay_.rout[k]].take(idx));
            return out;
        }
        std::vector<std::vector<std::int64_t>> per(segs.size());
        std::vector<std::int64_t> base(segs.size(), 0);
        for (std::size_t j = 0; j < n; ++j)
            if (which[j] >= 0) ++base[static_cast<std::size_t>(which[j])];
        std::int64_t acc = 0;
        for (std::size_t s = 0; s < segs.size(); ++s) {
            per[s].reserve(static_cast<std::size_t>(base[s]));
            const std::int64_t cnt = base[s];
            base[s] = acc;
            acc += cnt;
        }
        std::vector<std::int64_t> perm(n, -1);
        for (std::size_t j = 0; j < n; ++j) {
            if (which[j] < 0) continue;
            const auto s = static_cast<std::size_t>(which[j]);
            perm[j] = base[s] + static_cast<std::int64_t>(per[s].size());
            per[s].push_back(out_ref_[j].i);
        }
        for (std::size_t k = 0; k < lay_.rout.size(); ++k) {
            std::vector<Series> pieces;
            pieces.reserve(segs.size());
            for (std::size_t s = 0; s < segs.size(); ++s)
                pieces.push_back(segs[s]->cols[lay_.rout[k]].take(per[s]));
            std::vector<const Series*> ptrs;
            for (const Series& p : pieces) ptrs.push_back(&p);
            out.push_back(concat_columns(ptrs).take(perm));
        }
        return out;
    }

    std::unique_ptr<Cursor> left_;
    RightFeed feed_;
    Layout lay_;
    std::vector<std::string> names_;
    std::uint64_t budget_;
    std::string who_;
    std::int64_t max_rows_ = 0;

    std::shared_ptr<LeftBlock> lb_;
    std::int64_t li_ = 0;
    std::shared_ptr<LeftBlock> rep_;
    std::int64_t rep_row_ = 0;
    bool checked_ = false;

    std::vector<std::int64_t> out_left_;
    std::vector<Ref> out_ref_;
    bool left_contig_ = true;
    std::vector<SegPtr> pinned_;
    std::uint64_t pinned_bytes_ = 0;
    std::uint64_t pin_cap_ = 0;
    std::size_t row_cap_ = 1;
};

// Each left row takes the one right row nearest to it in time within its
// by-group. The right rows at or before the left time are consumed as the left
// time grows, and only the first and the last row of the latest equal-time run
// are kept.
class AsofCursor : public OrderedJoin {
   public:
    AsofCursor(std::unique_ptr<Cursor> left, LazyFrame right_sorted,
               const Layout& lay, std::vector<std::string> rsch,
               std::vector<std::string> names, std::uint64_t budget,
               AsofDirection direction, std::optional<double> tolerance)
        : OrderedJoin(std::move(left), std::move(right_sorted), lay,
                      std::move(rsch), std::move(names), budget, "asof"),
          direction_(direction),
          tolerance_(tolerance) {}

   protected:
    bool full() const override {
        return out_left_.size() >= row_cap_ || pinned_bytes_ > pin_cap_;
    }

    coro::CoroTask<void> step() override {
        const LeftBlock& L = *lb_;
        const std::int64_t r = li_++;
        if (co_await enter_group(r)) {
            have_back_ = false;
            back_first_ = back_last_ = Held{};
        }
        if (L.t->is_missing(r)) {
            push_row(r, Ref{});
            co_return;
        }
        const ColumnView& lt = *L.t;
        for (;;) {
            if (!co_await feed_.fill(max_rows_)) break;
            const SegPtr& s = feed_.seg();
            const std::int64_t j = feed_.i();
            if (!same_group(L, r, *s, j) || s->t->is_missing(j) ||
                s->t->compare(j, lt, r) > 0)
                break;
            if (!have_back_ ||
                back_last_.seg->t->compare(back_last_.i, *s->t, j) != 0)
                back_first_ = Held{s, j};
            back_last_ = Held{s, j};
            have_back_ = true;
            feed_.advance();
        }
        Held fwd;
        if (direction_ != AsofDirection::Backward) {
            if (have_back_ &&
                back_last_.seg->t->compare(back_last_.i, lt, r) == 0) {
                fwd = back_first_;
            } else if (co_await feed_.fill(max_rows_)) {
                const SegPtr& s = feed_.seg();
                const std::int64_t j = feed_.i();
                if (same_group(L, r, *s, j) && !s->t->is_missing(j))
                    fwd = Held{s, j};
            }
        }
        const Held* cand = nullptr;
        if (direction_ == AsofDirection::Backward) {
            if (have_back_) cand = &back_last_;
        } else if (direction_ == AsofDirection::Forward) {
            if (fwd.seg) cand = &fwd;
        } else if (!have_back_) {
            if (fwd.seg) cand = &fwd;
        } else if (!fwd.seg) {
            cand = &back_last_;
        } else {
            cand = ops::closer_or_equal(lt, r, *back_last_.seg->t, back_last_.i,
                                        *fwd.seg->t, fwd.i)
                       ? &back_last_
                       : &fwd;
        }
        if (cand && (!tolerance_ ||
                     ops::within(lt, r, *cand->seg->t, cand->i, *tolerance_))) {
            pin(cand->seg);
            push_row(r, Ref{cand->seg.get(), cand->i});
        } else {
            push_row(r, Ref{});
        }
    }

   private:
    AsofDirection direction_;
    std::optional<double> tolerance_;
    bool have_back_ = false;
    Held back_first_;
    Held back_last_;
};

// Each left point pairs with every right interval [lo, hi] that holds it in
// its by-group, ordered by (lo, hi, right position). The intervals that have
// started and not ended stay as the active set: a min-heap on hi, whose rows
// are copied compactly out of the right morsels so a long interval does not
// keep its whole morsel.
class IntervalCursor : public OrderedJoin {
   public:
    IntervalCursor(std::unique_ptr<Cursor> left, LazyFrame right_sorted,
                   const Layout& lay, std::vector<std::string> rsch,
                   std::vector<std::string> names, std::uint64_t budget,
                   bool outer)
        : OrderedJoin(std::move(left), std::move(right_sorted), lay,
                      std::move(rsch), std::move(names), budget, "interval"),
          outer_(outer) {}

   protected:
    bool busy() const override { return emitting_; }

    void after_flush() override { sweep(); }

    coro::CoroTask<void> step() override {
        if (busy()) {
            emit_more();
            co_return;
        }
        if (out_left_.empty() && chunks_.size() > sweep_at_) {
            sweep();
            sweep_at_ = 2 * chunks_.size() + 64;
        }
        const LeftBlock& L = *lb_;
        const std::int64_t r = li_++;
        if (co_await enter_group(r)) {
            for (const Entry& e : active_) --e.seg->live;
            active_.clear();
            ordered_.clear();
            dirty_ = true;
        }
        const bool has_point = !L.t->is_missing(r);
        if (has_point) {
            co_await admit(L, r);
            while (!active_.empty() && active_.front().seg->hi->compare(
                                           active_.front().i, *L.t, r) < 0) {
                std::pop_heap(active_.begin(), active_.end(), hi_greater);
                --active_.back().seg->live;
                active_.pop_back();
                dirty_ = true;
            }
        }
        if (!has_point || active_.empty()) {
            if (outer_) push_row(r, Ref{});
            co_return;
        }
        if (dirty_) {
            ordered_ = active_;
            std::sort(ordered_.begin(), ordered_.end(),
                      [](const Entry& x, const Entry& y) {
                          int c = x.seg->t->compare(x.i, *y.seg->t, y.i);
                          if (c != 0) return c < 0;
                          c = x.seg->hi->compare(x.i, *y.seg->hi, y.i);
                          if (c != 0) return c < 0;
                          return x.pos < y.pos;
                      });
            dirty_ = false;
        }
        emit_row_ = r;
        emit_pos_ = 0;
        emitting_ = true;
        emit_more();
    }

   private:
    struct Entry {
        Seg* seg;
        std::int64_t i;
        std::int64_t pos;
    };

    static bool hi_greater(const Entry& x, const Entry& y) {
        return x.seg->hi->compare(x.i, *y.seg->hi, y.i) > 0;
    }

    void emit_more() {
        while (emit_pos_ < ordered_.size() && !full()) {
            const Entry& e = ordered_[emit_pos_++];
            push_row(emit_row_, Ref{e.seg, e.i});
        }
        if (emit_pos_ >= ordered_.size()) emitting_ = false;
    }

    void sweep() {
        chunks_.erase(
            std::remove_if(chunks_.begin(), chunks_.end(),
                           [](const SegPtr& c) { return c->live == 0; }),
            chunks_.end());
    }

    // Moves every right row with lo <= the point into the active set.
    coro::CoroTask<void> admit(const LeftBlock& L, std::int64_t r) {
        std::vector<std::int64_t> adm;
        SegPtr from;
        while (co_await feed_.fill(max_rows_)) {
            const SegPtr& s = feed_.seg();
            const std::int64_t j = feed_.i();
            if (!same_group(L, r, *s, j) || s->t->is_missing(j) ||
                s->t->compare(j, *L.t, r) > 0)
                break;
            if (!s->hi->is_missing(j)) {
                if (from && from != s) {
                    admit_rows(*from, adm);
                    adm.clear();
                }
                from = s;
                adm.push_back(j);
            }
            feed_.advance();
        }
        if (!adm.empty()) admit_rows(*from, adm);
    }

    void admit_rows(const Seg& from, const std::vector<std::int64_t>& rows) {
        auto c = std::make_shared<Seg>();
        c->cols.resize(from.cols.size());
        for (std::size_t col : lay_.rout)
            c->cols[col] = from.cols[col].take(rows);
        c->cols[lay_.rt] = from.cols[lay_.rt].take(rows);
        c->cols[lay_.rhi] = from.cols[lay_.rhi].take(rows);
        c->t.emplace(c->cols[lay_.rt]);
        c->hi.emplace(c->cols[lay_.rhi]);
        c->rows = static_cast<std::int64_t>(rows.size());
        c->live = c->rows;
        for (std::int64_t k = 0; k < c->rows; ++k) {
            active_.push_back(Entry{c.get(), k, seq_++});
            std::push_heap(active_.begin(), active_.end(), hi_greater);
        }
        chunks_.push_back(std::move(c));
        dirty_ = true;
    }

    bool outer_;
    std::vector<Entry> active_;
    std::vector<Entry> ordered_;
    std::vector<SegPtr> chunks_;
    std::size_t emit_pos_ = 0;
    std::size_t sweep_at_ = 64;
    bool emitting_ = false;
    std::int64_t emit_row_ = 0;
    std::int64_t seq_ = 0;
    bool dirty_ = true;
};

constexpr const char* ASOF_WHO = "asof";
constexpr const char* INTERVAL_WHO = "interval";

}  // namespace

namespace lazy_internal {

std::unique_ptr<Cursor> make_ordered_join(std::unique_ptr<Cursor>& in,
                                          const std::vector<std::string>& sch,
                                          const std::string& name,
                                          const OwnedFrameOpArgs& args_ref,
                                          const LazyFrame& other,
                                          std::uint64_t budget) {
    const OwnedFrameOpArgs* args = &args_ref;
    if (sch.empty() || has_rest(sch)) return nullptr;
    const std::vector<std::string> rsch = other.schema();
    if (rsch.empty() || has_rest(rsch)) return nullptr;

    const bool asof = name == ASOF_OP;
    const char* who = asof ? ASOF_WHO : INTERVAL_WHO;
    Layout lay;
    std::vector<std::string> by;
    std::vector<std::string> lkeys, rkeys, skip;
    std::string ltime, rtime;
    std::string rhi;
    if (asof) {
        ltime = rtime = args->strings_at(2).front();
        by = args->strings_at(3);
        skip = by;
        skip.push_back(rtime);
    } else {
        ltime = args->strings_at(2).front();
        rtime = args->strings_at(3).front();
        rhi = args->strings_at(4).front();
        by = args->strings_at(5);
        skip = by;
        skip.push_back(rtime);
        skip.push_back(rhi);
        lay.has_hi = true;
    }
    for (const std::string& n : by) {
        lay.lby.push_back(static_cast<std::size_t>(require_col(sch, n, who)));
        lay.rby.push_back(static_cast<std::size_t>(require_col(rsch, n, who)));
    }
    lay.lt = static_cast<std::size_t>(require_col(sch, ltime, who));
    lay.rt = static_cast<std::size_t>(require_col(rsch, rtime, who));
    if (lay.has_hi)
        lay.rhi = static_cast<std::size_t>(require_col(rsch, rhi, who));
    for (std::size_t c = 0; c < rsch.size(); ++c)
        if (std::find(skip.begin(), skip.end(), rsch[c]) == skip.end())
            lay.rout.push_back(c);
    lay.rfields = other.output_schema().fields;
    if (lay.rfields.size() != rsch.size()) return nullptr;
    const std::vector<std::string> names = joined_names(sch, rsch, skip, who);

    lkeys = by;
    lkeys.push_back(ltime);
    rkeys = by;
    rkeys.push_back(rtime);
    std::unique_ptr<Cursor> sorted_left =
        make_sort_merge(std::move(in), sch, lkeys,
                        std::vector<bool>(lkeys.size(), false), budget);
    LazyFrame sorted_right =
        other.sort_by_multi(rkeys, std::vector<bool>(rkeys.size(), false));
    if (asof) {
        const auto direction = static_cast<AsofDirection>(args->i32_at(4));
        std::optional<double> tol;
        const double t = args->f64_at(5);
        if (t >= 0) tol = t;
        return std::make_unique<AsofCursor>(std::move(sorted_left),
                                            std::move(sorted_right), lay, rsch,
                                            names, budget, direction, tol);
    }
    return std::make_unique<IntervalCursor>(
        std::move(sorted_left), std::move(sorted_right), lay, rsch, names,
        budget, args->i32_at(6) != 0);
}

LazyFrame compose_asof(const LazyFrame& left, LazyFrame other,
                       const std::string& on,
                       const std::vector<std::string>& by,
                       AsofDirection direction,
                       std::optional<double> tolerance) {
    if (tolerance && !(*tolerance >= 0))
        throw std::invalid_argument("asof: tolerance must not be negative");
    const std::vector<std::string> lnames = left.schema();
    const std::vector<std::string> rnames = other.schema();
    std::vector<std::string> skip = by;
    skip.push_back(on);
    std::vector<std::string> names;
    if (!lnames.empty() && !rnames.empty()) {
        require_col(lnames, on, ASOF_WHO);
        require_col(rnames, on, ASOF_WHO);
        for (const std::string& n : by) {
            require_col(lnames, n, ASOF_WHO);
            require_col(rnames, n, ASOF_WHO);
        }
        check_field_types(left.output_schema().fields,
                          other.output_schema().fields, ASOF_WHO, on, on, by,
                          {});
        names = joined_names(lnames, rnames, skip, ASOF_WHO);
    }
    std::vector<const char*> by_ptrs;
    for (const std::string& n : by) by_ptrs.push_back(n.c_str());
    OpArgs args;
    args.str(2, on.c_str())
        .strlist(3, by_ptrs.data(), static_cast<std::int32_t>(by_ptrs.size()))
        .i32(4, static_cast<std::int32_t>(direction))
        .f64(5, tolerance ? *tolerance : -1.0);
    return left.frame_op(ASOF_OP, std::move(args), {std::move(other)},
                         std::move(names));
}

LazyFrame compose_interval(const LazyFrame& left, LazyFrame other,
                           const std::string& point, const std::string& lo,
                           const std::string& hi,
                           const std::vector<std::string>& by, bool outer) {
    const std::vector<std::string> lnames = left.schema();
    const std::vector<std::string> rnames = other.schema();
    std::vector<std::string> skip = by;
    skip.push_back(lo);
    skip.push_back(hi);
    std::vector<std::string> names;
    if (!lnames.empty() && !rnames.empty()) {
        require_col(lnames, point, INTERVAL_WHO);
        require_col(rnames, lo, INTERVAL_WHO);
        require_col(rnames, hi, INTERVAL_WHO);
        for (const std::string& n : by) {
            require_col(lnames, n, INTERVAL_WHO);
            require_col(rnames, n, INTERVAL_WHO);
        }
        check_field_types(left.output_schema().fields,
                          other.output_schema().fields, INTERVAL_WHO, point, lo,
                          by, {hi});
        names = joined_names(lnames, rnames, skip, INTERVAL_WHO);
    }
    std::vector<const char*> by_ptrs;
    for (const std::string& n : by) by_ptrs.push_back(n.c_str());
    OpArgs args;
    args.str(2, point.c_str())
        .str(3, lo.c_str())
        .str(4, hi.c_str())
        .strlist(5, by_ptrs.data(), static_cast<std::int32_t>(by_ptrs.size()))
        .i32(6, outer ? 1 : 0);
    return left.frame_op(INTERVAL_OP, std::move(args), {std::move(other)},
                         std::move(names));
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
