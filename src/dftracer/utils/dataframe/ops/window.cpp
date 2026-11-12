// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/internal/errors.h>
#include <dftracer/utils/dataframe/kernels/order.h>
#include <dftracer/utils/dataframe/kernels/running.h>
#include <dftracer/utils/dataframe/ops/ops_common.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace dftracer::utils::dataframe {

namespace {

using Kind = ColumnView::Kind;

// The rows in (partition, order) sorted order and the partitions in it.
struct Layout {
    std::int64_t n = 0;
    dstd::Vector<std::int64_t> idx;
    dstd::Vector<dstd::Pair<std::int64_t, std::int64_t>> parts;
    const dstd::Vector<ColumnView>* order_keys = nullptr;

    // Whether the rows at sorted positions `a` and `b` tie on the order keys.
    bool tie(std::int64_t a, std::int64_t b) const {
        return same_keys(*order_keys, idx[static_cast<std::size_t>(a)],
                         idx[static_cast<std::size_t>(b)]);
    }
    std::int64_t row(std::int64_t pos) const {
        return idx[static_cast<std::size_t>(pos)];
    }
};

// A nullable output column built by position.
template <class T>
struct Column {
    dstd::Vector<T> value;
    dstd::Vector<std::uint8_t> valid;

    explicit Column(std::int64_t n)
        : value(static_cast<std::size_t>(n)),
          valid(ops::valid_bits(static_cast<std::size_t>(n))) {}

    void set(std::int64_t at, T v) { value[static_cast<std::size_t>(at)] = v; }
    void null(std::int64_t at) {
        ops::set_null(valid, static_cast<std::size_t>(at));
    }
    Series series(TypeId type) const {
        return Series::flat(type, value.data(),
                            static_cast<std::int64_t>(value.size()),
                            valid.data());
    }
};

Series pass_through(const Series& column,
                    const dstd::Vector<std::int64_t>& source) {
    return column.take(source);
}

dstd::Result<void> numeric(const ColumnView& v, const char* what) {
    if (v.is_bytes())
        return invalid_argument<"window: {} over a non-numeric column">(what);
    return {};
}

dstd::Result<ColumnView> view_of(const Series& column, const char* what) {
    if (!ColumnView::supports(column.type()))
        return invalid_argument<"window: {} over an unsupported column type">(
            what);
    return ColumnView(column);
}

dstd::Result<const Series*> value_column(const DataFrame& df,
                                         const WindowColumn& w) {
    if (!w.value())
        return invalid_argument<"window: '{}' needs a value column">(w.out);
    const dstd::Result<std::size_t> at =
        ops::column_named(df, *w.value(), "window");
    if (!at) return at.error();
    return &df.columns[*at];
}

dstd::Result<const Series*> named_column(const DataFrame& df,
                                         const dstd::String* name,
                                         const WindowColumn& w,
                                         const char* role) {
    if (!name)
        return invalid_argument<"window: '{}' needs a {} column">(w.out, role);
    const dstd::Result<std::size_t> at = ops::column_named(df, *name, "window");
    if (!at) return at.error();
    return &df.columns[*at];
}

// The error a running or frame sum reports when `step` overflowed `type`.
dstd::Unexpected<dstd::Error> overflow_error(const char* func,
                                             const char* type) {
    return common::make_error(common::ErrorCode::InvalidArgument,
                              steps::overflow_message(func, type));
}

// The extreme (min when `smallest`) present row of a running or frame scan.
bool better(const ColumnView& v, std::int64_t candidate, std::int64_t current,
            bool smallest) {
    const int c = v.compare(candidate, current);
    return smallest ? c < 0 : c > 0;
}

Series ranking(const Layout& lay, WindowFunc func) {
    if (func == WindowFunc::PercentRank || func == WindowFunc::CumeDist) {
        Column<double> out(lay.n);
        for (const auto& [p, q] : lay.parts) {
            const std::int64_t sz = q - p;
            std::int64_t rank = 0;
            std::int64_t peer_end = 0;
            for (std::int64_t r = p; r < q; ++r) {
                const std::int64_t local = r - p;
                if (func == WindowFunc::PercentRank) {
                    if (local == 0 || !lay.tie(r, r - 1)) rank = local + 1;
                    out.set(r, sz > 1 ? static_cast<double>(rank - 1) /
                                            static_cast<double>(sz - 1)
                                      : 0.0);
                } else {
                    if (local >= peer_end) {
                        std::int64_t e = r + 1;
                        while (e < q && lay.tie(r, e)) ++e;
                        peer_end = e - p;
                    }
                    out.set(r, static_cast<double>(peer_end) /
                                   static_cast<double>(sz));
                }
            }
        }
        return out.series(TypeId::Float64);
    }
    Column<std::int64_t> out(lay.n);
    for (const auto& [p, q] : lay.parts) {
        std::int64_t rank = 0;
        for (std::int64_t r = p; r < q; ++r) {
            const std::int64_t local = r - p;
            switch (func) {
                case WindowFunc::Rank:
                    if (local == 0 || !lay.tie(r, r - 1)) rank = local + 1;
                    out.set(r, rank);
                    break;
                case WindowFunc::DenseRank:
                    if (local == 0)
                        rank = 1;
                    else if (!lay.tie(r, r - 1))
                        ++rank;
                    out.set(r, rank);
                    break;
                default:  // RowNumber, RunningCount
                    out.set(r, local + 1);
                    break;
            }
        }
    }
    return out.series(TypeId::Int64);
}

Series ntile(const Layout& lay, std::int64_t buckets) {
    Column<std::int64_t> out(lay.n);
    for (const auto& [p, q] : lay.parts) {
        const std::int64_t sz = q - p;
        for (std::int64_t r = p; r < q; ++r) {
            if (buckets <= 0) {
                out.null(r);
                continue;
            }
            const std::int64_t local = r - p;
            const std::int64_t base = sz / buckets;
            const std::int64_t rem = sz % buckets;
            const std::int64_t big = rem * (base + 1);
            out.set(r, local < big ? local / (base + 1) + 1
                                   : rem + (local - big) / base + 1);
        }
    }
    return out.series(TypeId::Int64);
}

// Lag, Lead, FirstValue, LastValue, NthValue and FillForward: the value of
// another row of the partition, so the output is a gather of the column.
Series positional(const Layout& lay, const Series& column,
                  const WindowColumn& w) {
    dstd::Vector<std::int64_t> source(static_cast<std::size_t>(lay.n), -1);
    for (const auto& [p, q] : lay.parts) {
        steps::FillForward<std::int64_t> fill;
        steps::Lag<std::int64_t> lag;
        for (std::int64_t r = p; r < q; ++r) {
            const std::int64_t local = r - p;
            std::int64_t from = -1;
            switch (w.func) {
                case WindowFunc::Lag:
                case WindowFunc::Lead: {
                    const std::int64_t offset = w.params.offset;
                    if (w.func == WindowFunc::Lag && offset >= 0) {
                        // A lag past the partition has no row to read; the
                        // ring would only hold rows that never come back.
                        std::int64_t back = -1;
                        if (offset < q - p &&
                            lag.step(offset, lay.row(r), back))
                            from = back;
                        break;
                    }
                    const std::int64_t at = steps::offset_position(
                        local, q - p,
                        w.func == WindowFunc::Lag ? -offset : offset);
                    if (at >= 0) from = lay.row(p + at);
                    break;
                }
                case WindowFunc::FirstValue:
                    from = lay.row(p);
                    break;
                case WindowFunc::LastValue:
                    from = lay.row(q - 1);
                    break;
                case WindowFunc::NthValue: {
                    const std::int64_t k = w.params.offset;
                    if (k >= 1 && k <= q - p) from = lay.row(p + k - 1);
                    break;
                }
                default: {  // FillForward
                    fill.step(!column.is_null(lay.row(r)), lay.row(r), from);
                    break;
                }
            }
            source[static_cast<std::size_t>(r)] = from;
        }
    }
    return pass_through(column, source);
}

dstd::Result<Series> running(const Layout& lay, const Series& column,
                             const WindowColumn& w) {
    const dstd::Result<ColumnView> view = view_of(column, "running aggregate");
    if (!view) return view.error();
    const ColumnView& v = *view;
    switch (w.func) {
        case WindowFunc::RunningSum: {
            if (auto ok = numeric(v, "running aggregate"); !ok)
                return ok.error();
            if (v.kind() == Kind::Float) {
                Column<double> out(lay.n);
                for (const auto& [p, q] : lay.parts) {
                    steps::SumF64 sum;
                    for (std::int64_t r = p; r < q; ++r) {
                        const std::int64_t g = lay.row(r);
                        if (!v.is_null(g)) sum.add(v.get_double(g));
                        out.set(r, sum.acc);
                    }
                }
                return out.series(TypeId::Float64);
            }
            if (v.kind() == Kind::Unsigned) {
                Column<std::uint64_t> out(lay.n);
                bool overflowed = false;
                for (const auto& [p, q] : lay.parts) {
                    steps::SumU64 sum;
                    for (std::int64_t r = p; r < q; ++r) {
                        const std::int64_t g = lay.row(r);
                        if (!v.is_null(g)) sum.add(v.get_uint(g));
                        out.set(r, sum.acc);
                    }
                    overflowed |= sum.overflowed;
                }
                if (overflowed) return overflow_error("RUNNING_SUM", "uint64");
                return out.series(TypeId::Uint64);
            }
            Column<std::int64_t> out(lay.n);
            bool overflowed = false;
            for (const auto& [p, q] : lay.parts) {
                steps::SumI64 sum;
                for (std::int64_t r = p; r < q; ++r) {
                    const std::int64_t g = lay.row(r);
                    if (!v.is_null(g)) sum.add(v.get_int(g));
                    out.set(r, sum.acc);
                }
                overflowed |= sum.overflowed;
            }
            if (overflowed) return overflow_error("RUNNING_SUM", "int64");
            return out.series(TypeId::Int64);
        }
        case WindowFunc::RunningProd: {
            if (auto ok = numeric(v, "running aggregate"); !ok)
                return ok.error();
            Column<double> out(lay.n);
            for (const auto& [p, q] : lay.parts) {
                steps::Prod prod;
                for (std::int64_t r = p; r < q; ++r) {
                    const std::int64_t g = lay.row(r);
                    if (!v.is_null(g)) prod.mul(v.get_double(g));
                    out.set(r, prod.acc);
                }
            }
            return out.series(TypeId::Float64);
        }
        default: {  // RunningMin, RunningMax
            dstd::Vector<std::int64_t> source(static_cast<std::size_t>(lay.n),
                                              -1);
            const bool smallest = w.func == WindowFunc::RunningMin;
            // The kind of the column is chosen once; each row is then one call.
            const auto scan = [&](auto&& offer) {
                for (const auto& [p, q] : lay.parts) {
                    steps::Extreme ext;
                    std::int64_t best = -1;
                    for (std::int64_t r = p; r < q; ++r) {
                        const std::int64_t g = lay.row(r);
                        if (!v.is_null(g) && offer(ext, g, best)) best = g;
                        source[static_cast<std::size_t>(r)] = best;
                    }
                }
            };
            switch (v.kind()) {
                case Kind::Signed:
                    scan([&](steps::Extreme& e, std::int64_t g, std::int64_t) {
                        return e.offer_i(v.get_int(g), smallest);
                    });
                    break;
                case Kind::Unsigned:
                    scan([&](steps::Extreme& e, std::int64_t g, std::int64_t) {
                        return e.offer_u(v.get_uint(g), smallest);
                    });
                    break;
                case Kind::Float:
                    scan([&](steps::Extreme& e, std::int64_t g, std::int64_t) {
                        return e.offer_f(v.get_double(g), smallest);
                    });
                    break;
                default:
                    scan([&](steps::Extreme&, std::int64_t g,
                             std::int64_t best) {
                        return best < 0 || better(v, g, best, smallest);
                    });
                    break;
            }
            return pass_through(column, source);
        }
    }
}

dstd::Result<Series> delta(const Layout& lay, const Series& column) {
    const dstd::Result<ColumnView> view = view_of(column, "DELTA/RATE");
    if (!view) return view.error();
    const ColumnView& v = *view;
    if (auto ok = numeric(v, "DELTA/RATE"); !ok) return ok.error();
    const bool floating = v.kind() == Kind::Float;
    Column<double> fout(floating ? lay.n : 0);
    Column<std::int64_t> iout(floating ? 0 : lay.n);
    bool overflowed = false;
    for (const auto& [p, q] : lay.parts) {
        for (std::int64_t r = p; r < q; ++r) {
            const std::int64_t g = lay.row(r);
            const std::int64_t prev = r > p ? lay.row(r - 1) : -1;
            if (prev < 0 || v.is_null(g) || v.is_null(prev)) {
                if (floating)
                    fout.null(r);
                else
                    iout.null(r);
                continue;
            }
            if (floating) {
                fout.set(r,
                         steps::delta_f64(v.get_double(g), v.get_double(prev)));
                continue;
            }
            std::int64_t diff = 0;
            const bool fits =
                v.kind() == Kind::Unsigned
                    ? steps::delta_u64(v.get_uint(g), v.get_uint(prev), diff)
                    : steps::delta_i64(v.get_int(g), v.get_int(prev), diff);
            if (!fits) overflowed = true;
            iout.set(r, diff);
        }
    }
    if (overflowed) return overflow_error("DELTA", "int64");
    return floating ? fout.series(TypeId::Float64) : iout.series(TypeId::Int64);
}

dstd::Result<Series> rate(const Layout& lay, const Series& column,
                          const Series& time, bool counter) {
    const dstd::Result<ColumnView> value_view = view_of(column, "DELTA/RATE");
    if (!value_view) return value_view.error();
    const ColumnView& v = *value_view;
    if (auto ok = numeric(v, "DELTA/RATE"); !ok) return ok.error();
    const dstd::Result<ColumnView> time_view = view_of(time, "RATE/SESSIONIZE");
    if (!time_view) return time_view.error();
    const ColumnView& t = *time_view;
    if (t.is_bytes())
        return invalid_argument<
            "window: RATE/SESSIONIZE over a non-numeric time column">();
    Column<double> out(lay.n);
    for (const auto& [p, q] : lay.parts) {
        for (std::int64_t r = p; r < q; ++r) {
            const std::int64_t g = lay.row(r);
            const std::int64_t prev = r > p ? lay.row(r - 1) : -1;
            if (prev < 0 || v.is_null(g) || v.is_null(prev) || t.is_null(g) ||
                t.is_null(prev)) {
                out.null(r);
                continue;
            }
            const double dt = t.get_double(g) - t.get_double(prev);
            if (dt == 0.0) {
                out.null(r);
                continue;
            }
            const double cur = v.get_double(g);
            const double pv = v.get_double(prev);
            const double dv = (counter && cur < pv) ? cur : cur - pv;
            out.set(r, dv / dt);
        }
    }
    return out.series(TypeId::Float64);
}

dstd::Result<Series> sessionize(const Layout& lay, const Series& time,
                                const Series* end_column, double gap,
                                double span) {
    if (!(gap >= 0.0) || !(span >= 0.0))
        return invalid_argument<
            "window: SESSIONIZE gap and span must not be negative">();
    const dstd::Result<ColumnView> time_view = view_of(time, "RATE/SESSIONIZE");
    if (!time_view) return time_view.error();
    const ColumnView& t = *time_view;
    if (t.is_bytes())
        return invalid_argument<
            "window: RATE/SESSIONIZE over a non-numeric time column">();
    dstd::Optional<ColumnView> e;
    if (end_column != nullptr) {
        dstd::Result<ColumnView> end_view = view_of(*end_column, "SESSIONIZE");
        if (!end_view) return end_view.error();
        e.emplace(std::move(*end_view));
        if (e->is_bytes())
            return invalid_argument<
                "window: SESSIONIZE over a non-numeric end column">();
    }
    Column<std::int64_t> out(lay.n);
    for (const auto& [p, q] : lay.parts) {
        std::int64_t id = 0;
        double first = 0.0;
        double session_end = 0.0;
        for (std::int64_t r = p; r < q; ++r) {
            const std::int64_t g = lay.row(r);
            if (t.is_null(g)) {
                out.null(r);
                continue;
            }
            const double at = t.get_double(g);
            double end = at;
            if (e && !e->is_null(g)) end = std::max(at, e->get_double(g));
            if (id == 0 || at - session_end > gap ||
                (span > 0.0 && at - first > span)) {
                ++id;
                first = at;
                session_end = end;
            } else {
                session_end = std::max(session_end, end);
            }
            out.set(r, id);
        }
    }
    return out.series(TypeId::Int64);
}

struct FrameBounds {
    std::int64_t preceding;
    std::int64_t following;
    bool range;
};

dstd::Result<Series> frame(const Layout& lay, const Series& column,
                           const WindowColumn& w) {
    const WindowParams::Frame& f = w.params.frame;
    if ((f.preceding < 0 && f.preceding != WINDOW_UNBOUNDED) ||
        (f.following < 0 && f.following != WINDOW_UNBOUNDED) || f.min_count < 0)
        return invalid_argument<
            "window: frame bounds and min_count must not be negative">();
    const bool range = f.mode == WindowFrameMode::Range;
    const dstd::Vector<ColumnView>& order = *lay.order_keys;
    if (range && (order.size() != 1 || order[0].is_bytes()))
        return invalid_argument<
            "window: RANGE frame needs exactly one numeric order column">();

    const bool counting = w.func == WindowFunc::FrameCount;
    dstd::Optional<ColumnView> vv;
    if (!counting) {
        dstd::Result<ColumnView> value_view =
            view_of(column, "frame aggregate");
        if (!value_view) return value_view.error();
        vv.emplace(std::move(*value_view));
        if ((w.func == WindowFunc::FrameSum ||
             w.func == WindowFunc::FrameMean) &&
            vv->is_bytes())
            return invalid_argument<
                "window: FRAME_SUM/FRAME_MEAN over a non-numeric column">();
    }
    const bool min_max =
        w.func == WindowFunc::FrameMin || w.func == WindowFunc::FrameMax;
    const bool smallest = w.func == WindowFunc::FrameMin;
    const bool sum_double =
        w.func == WindowFunc::FrameSum && vv->kind() == Kind::Float;
    const auto present = [&](std::int64_t g) {
        return counting ? !column.is_null(g) : !vv->is_null(g);
    };

    Column<std::int64_t> ints(lay.n);
    Column<double> doubles(lay.n);
    dstd::Vector<std::int64_t> source(
        min_max ? static_cast<std::size_t>(lay.n) : 0, -1);
    const bool as_double = w.func == WindowFunc::FrameMean || sum_double;
    const bool unsigned_sum =
        w.func == WindowFunc::FrameSum && vv->kind() == Kind::Unsigned;
    Column<std::uint64_t> uints(unsigned_sum ? lay.n : 0);
    bool overflowed = false;

    for (const auto& [p, q] : lay.parts) {
        const std::int64_t sz = q - p;
        std::int64_t win_lo = 0, win_hi = -1, count = 0, first_missing = -1;
        steps::FrameSumInt sum_i;
        steps::FrameSumF64 sum_d;
        steps::SlidingExtreme dq;
        const auto position_row = [&](std::int64_t pos) {
            return lay.row(p + pos);
        };
        const auto enter = [&](std::int64_t pos) {
            const std::int64_t g = position_row(pos);
            if (!present(g)) return;
            ++count;
            if (counting) return;
            if (min_max) {
                dq.enter(pos, [&](std::int64_t back) {
                    const int c = vv->compare(g, position_row(back));
                    return smallest ? c <= 0 : c >= 0;
                });
            } else if (w.func == WindowFunc::FrameMean || sum_double) {
                sum_d.enter(vv->get_double(g));
            } else if (vv->kind() == Kind::Unsigned) {
                sum_i.enter_u(vv->get_uint(g));
            } else {
                sum_i.enter_i(vv->get_int(g));
            }
        };
        const auto leave = [&](std::int64_t pos) {
            const std::int64_t g = position_row(pos);
            if (!present(g)) return;
            --count;
            if (counting || min_max) return;
            if (w.func == WindowFunc::FrameMean || sum_double)
                sum_d.leave(vv->get_double(g));
            else if (vv->kind() == Kind::Unsigned)
                sum_i.leave_u(vv->get_uint(g));
            else
                sum_i.leave_i(vv->get_int(g));
        };
        for (std::int64_t r = p; r < q; ++r) {
            const std::int64_t local = r - p;
            std::int64_t lo, hi;
            if (!range) {
                lo = f.preceding == WINDOW_UNBOUNDED
                         ? 0
                         : std::max<std::int64_t>(0, local - f.preceding);
                hi = f.following == WINDOW_UNBOUNDED
                         ? sz - 1
                         : std::min<std::int64_t>(sz - 1, local + f.following);
            } else if (order[0].is_missing(lay.row(r))) {
                // Null and NaN order values sort last and are peers only of
                // each other.
                if (first_missing < 0) {
                    first_missing = r - p;
                    while (first_missing > 0 &&
                           order[0].is_missing(lay.row(p + first_missing - 1)))
                        --first_missing;
                }
                lo = std::max(win_lo, first_missing);
                hi = sz - 1;
            } else {
                const std::int64_t g = lay.row(r);
                const double ov = order[0].get_double(g);
                const double inf = std::numeric_limits<double>::infinity();
                const double lo_val =
                    f.preceding == WINDOW_UNBOUNDED
                        ? -inf
                        : ov - static_cast<double>(f.preceding);
                const double hi_val =
                    f.following == WINDOW_UNBOUNDED
                        ? inf
                        : ov + static_cast<double>(f.following);
                const auto od = [&](std::int64_t pos) {
                    return order[0].get_double(position_row(pos));
                };
                lo = win_lo;
                while (lo < sz && !order[0].is_missing(position_row(lo)) &&
                       od(lo) < lo_val)
                    ++lo;
                hi = std::max(win_hi, local);
                while (hi + 1 < sz &&
                       !order[0].is_missing(position_row(hi + 1)) &&
                       od(hi + 1) <= hi_val)
                    ++hi;
            }
            for (std::int64_t pos = win_hi + 1; pos <= hi; ++pos) enter(pos);
            for (std::int64_t pos = win_lo; pos < lo; ++pos) leave(pos);
            dq.drop_before(lo);
            win_lo = lo;
            win_hi = hi;

            if (counting) {
                ints.set(r, count);
            } else if (min_max) {
                if (dq.empty() || count < f.min_count)
                    source[static_cast<std::size_t>(r)] = -1;
                else
                    source[static_cast<std::size_t>(r)] =
                        position_row(dq.best());
            } else if (count == 0 || count < f.min_count) {
                if (as_double)
                    doubles.null(r);
                else if (unsigned_sum)
                    uints.null(r);
                else
                    ints.null(r);
            } else if (w.func == WindowFunc::FrameMean) {
                doubles.set(r, sum_d.mean(count));
            } else if (sum_double) {
                doubles.set(r, sum_d.sum);
            } else if (unsigned_sum) {
                if (!sum_i.fits_u64()) overflowed = true;
                uints.set(r, sum_i.u64());
            } else {
                if (!sum_i.fits_i64()) overflowed = true;
                ints.set(r, sum_i.i64());
            }
        }
    }
    if (overflowed)
        return overflow_error("FRAME_SUM", unsigned_sum ? "uint64" : "int64");
    if (min_max) return pass_through(column, source);
    if (unsigned_sum) return uints.series(TypeId::Uint64);
    return as_double ? doubles.series(TypeId::Float64)
                     : ints.series(TypeId::Int64);
}

constexpr std::int64_t WINDOW_COLLECT_MAX_ELEMENTS = std::int64_t{1} << 27;

struct FrameCtx {
    const Layout& lay;
    const WindowParams::Frame& f;
    bool range;
};

dstd::Result<FrameCtx> frame_context(const Layout& lay, const WindowColumn& w) {
    const WindowParams::Frame& f = w.params.frame;
    if ((f.preceding < 0 && f.preceding != WINDOW_UNBOUNDED) ||
        (f.following < 0 && f.following != WINDOW_UNBOUNDED) || f.min_count < 0)
        return invalid_argument<
            "window: frame bounds and min_count must not be negative">();
    const bool range = f.mode == WindowFrameMode::Range;
    const dstd::Vector<ColumnView>& order = *lay.order_keys;
    if (range && (order.size() != 1 || order[0].is_bytes()))
        return invalid_argument<
            "window: RANGE frame needs exactly one numeric order column">();
    return FrameCtx{lay, f, range};
}

// Visits each row of the partition [p, q) with its frame [lo, hi] (partition
// local positions), calling enter and leave for the positions that join and
// leave the frame first. The bounds are those of frame().
template <class Enter, class Leave, class Emit>
void walk_frame(const FrameCtx& c, std::int64_t p, std::int64_t q,
                Enter&& enter, Leave&& leave, Emit&& emit) {
    const Layout& lay = c.lay;
    const WindowParams::Frame& f = c.f;
    const dstd::Vector<ColumnView>& order = *lay.order_keys;
    const std::int64_t sz = q - p;
    std::int64_t win_lo = 0, win_hi = -1, first_missing = -1;
    for (std::int64_t r = p; r < q; ++r) {
        const std::int64_t local = r - p;
        std::int64_t lo, hi;
        if (!c.range) {
            lo = f.preceding == WINDOW_UNBOUNDED
                     ? 0
                     : std::max<std::int64_t>(0, local - f.preceding);
            hi = f.following == WINDOW_UNBOUNDED
                     ? sz - 1
                     : std::min<std::int64_t>(sz - 1, local + f.following);
        } else if (order[0].is_missing(lay.row(r))) {
            if (first_missing < 0) {
                first_missing = r - p;
                while (first_missing > 0 &&
                       order[0].is_missing(lay.row(p + first_missing - 1)))
                    --first_missing;
            }
            lo = std::max(win_lo, first_missing);
            hi = sz - 1;
        } else {
            const double ov = order[0].get_double(lay.row(r));
            const double inf = std::numeric_limits<double>::infinity();
            const double lo_val = f.preceding == WINDOW_UNBOUNDED
                                      ? -inf
                                      : ov - static_cast<double>(f.preceding);
            const double hi_val = f.following == WINDOW_UNBOUNDED
                                      ? inf
                                      : ov + static_cast<double>(f.following);
            const auto od = [&](std::int64_t pos) {
                return order[0].get_double(lay.row(p + pos));
            };
            lo = win_lo;
            while (lo < sz && !order[0].is_missing(lay.row(p + lo)) &&
                   od(lo) < lo_val)
                ++lo;
            hi = std::max(win_hi, local);
            while (hi + 1 < sz && !order[0].is_missing(lay.row(p + hi + 1)) &&
                   od(hi + 1) <= hi_val)
                ++hi;
        }
        for (std::int64_t pos = win_hi + 1; pos <= hi; ++pos) enter(pos);
        for (std::int64_t pos = win_lo; pos < lo; ++pos) leave(pos);
        win_lo = lo;
        win_hi = hi;
        emit(r, lo, hi);
    }
}

// Dense ranks of a partition's present values: equal values share a rank.
struct Ranks {
    dstd::Vector<std::int32_t> of;
    dstd::Vector<std::int32_t> scratch;
    dstd::Vector<double> value;
    std::int32_t count = 0;

    void build(const Layout& lay, const ColumnView& v, std::int64_t p,
               std::int64_t q, bool keep_values) {
        const std::int64_t sz = q - p;
        of.assign(static_cast<std::size_t>(sz), -1);
        scratch.clear();
        value.clear();
        count = 0;
        for (std::int64_t i = 0; i < sz; ++i)
            if (!v.is_null(lay.row(p + i)))
                scratch.push_back(static_cast<std::int32_t>(i));
        std::sort(scratch.begin(), scratch.end(),
                  [&](std::int32_t a, std::int32_t b) {
                      return v.compare(lay.row(p + a), lay.row(p + b)) < 0;
                  });
        for (std::size_t i = 0; i < scratch.size(); ++i) {
            const std::int64_t g = lay.row(p + scratch[i]);
            if (i == 0 || v.compare(lay.row(p + scratch[i - 1]), g) != 0) {
                ++count;
                if (keep_values) value.push_back(v.get_double(g));
            }
            of[static_cast<std::size_t>(scratch[i])] = count - 1;
        }
    }
};

struct Fenwick {
    dstd::Vector<std::int32_t> tree;
    std::int32_t n = 0;

    void reset(std::int32_t size) {
        n = size;
        tree.assign(static_cast<std::size_t>(size) + 1, 0);
    }
    void add(std::int32_t at, std::int32_t delta) {
        for (std::int32_t i = at + 1; i <= n; i += i & -i)
            tree[static_cast<std::size_t>(i)] += delta;
    }
    // The 0-based rank holding the k-th (1-based) counted element.
    std::int32_t kth(std::int32_t k) const {
        std::int32_t pos = 0;
        std::int32_t pw = 1;
        while ((pw << 1) <= n) pw <<= 1;
        for (; pw > 0; pw >>= 1) {
            const std::int32_t next = pos + pw;
            if (next <= n && tree[static_cast<std::size_t>(next)] < k) {
                pos = next;
                k -= tree[static_cast<std::size_t>(next)];
            }
        }
        return pos;
    }
};

dstd::Result<Series> frame_variance(const FrameCtx& c, const Series& column,
                                    bool root, std::int64_t min_count) {
    const dstd::Result<ColumnView> view =
        view_of(column, "FRAME_VAR/FRAME_STD");
    if (!view) return view.error();
    const ColumnView& v = *view;
    if (auto ok = numeric(v, "FRAME_VAR/FRAME_STD"); !ok) return ok.error();
    Column<double> out(c.lay.n);
    dstd::Vector<std::int64_t> next_present, prev_present, run_start;
    for (const auto& [p, q] : c.lay.parts) {
        std::int64_t n = 0;
        double mean = 0.0, m2 = 0.0;
        // Variance is shift invariant; centering on the partition mean keeps
        // the running mean small when the values sit far from zero.
        double shift = 0.0;
        std::int64_t present = 0;
        for (std::int64_t r = p; r < q; ++r) {
            const std::int64_t g = c.lay.row(r);
            if (v.is_null(g)) continue;
            shift += (v.get_double(g) - shift) / static_cast<double>(++present);
        }
        const std::int64_t sz = q - p;
        next_present.assign(static_cast<std::size_t>(sz) + 1, sz);
        prev_present.assign(static_cast<std::size_t>(sz), -1);
        run_start.assign(static_cast<std::size_t>(sz), -1);
        for (std::int64_t i = 0, last = -1; i < sz; ++i) {
            const std::int64_t g = c.lay.row(p + i);
            if (!v.is_null(g)) {
                const bool same =
                    last >= 0 &&
                    v.get_double(g) == v.get_double(c.lay.row(p + last));
                run_start[static_cast<std::size_t>(i)] =
                    same ? run_start[static_cast<std::size_t>(last)] : i;
                last = i;
            }
            prev_present[static_cast<std::size_t>(i)] = last;
        }
        for (std::int64_t i = sz - 1; i >= 0; --i)
            next_present[static_cast<std::size_t>(i)] =
                v.is_null(c.lay.row(p + i))
                    ? next_present[static_cast<std::size_t>(i) + 1]
                    : i;
        walk_frame(
            c, p, q,
            [&](std::int64_t pos) {
                const std::int64_t g = c.lay.row(p + pos);
                if (v.is_null(g)) return;
                const double x = v.get_double(g) - shift;
                ++n;
                const double d = x - mean;
                mean += d / static_cast<double>(n);
                m2 += d * (x - mean);
            },
            [&](std::int64_t pos) {
                const std::int64_t g = c.lay.row(p + pos);
                if (v.is_null(g)) return;
                const double x = v.get_double(g) - shift;
                if (--n == 0) {
                    mean = 0.0;
                    m2 = 0.0;
                    return;
                }
                const double d = x - mean;
                mean -= d / static_cast<double>(n);
                m2 -= d * (x - mean);
            },
            [&](std::int64_t r, std::int64_t lo, std::int64_t hi) {
                if (n < 2 || n < min_count) {
                    out.null(r);
                    return;
                }
                const std::int64_t last =
                    prev_present[static_cast<std::size_t>(hi)];
                const bool constant =
                    run_start[static_cast<std::size_t>(last)] <=
                    next_present[static_cast<std::size_t>(lo)];
                const double var =
                    constant ? 0.0
                             : std::max(m2, 0.0) / static_cast<double>(n - 1);
                out.set(r, root ? std::sqrt(var) : var);
            });
    }
    return out.series(TypeId::Float64);
}

dstd::Result<Series> frame_quantile(const FrameCtx& c, const Series& column,
                                    double level, std::int64_t min_count) {
    if (!(level >= 0.0 && level <= 1.0))
        return invalid_argument<
            "window: FRAME_QUANTILE level must be in [0, 1]">();
    const dstd::Result<ColumnView> view = view_of(column, "FRAME_QUANTILE");
    if (!view) return view.error();
    const ColumnView& v = *view;
    if (auto ok = numeric(v, "FRAME_QUANTILE"); !ok) return ok.error();
    Column<double> out(c.lay.n);
    Ranks ranks;
    Fenwick fen;
    for (const auto& [p, q] : c.lay.parts) {
        ranks.build(c.lay, v, p, q, true);
        fen.reset(ranks.count);
        std::int32_t n = 0;
        const auto at = [&](std::int32_t k) {
            return ranks.value[static_cast<std::size_t>(fen.kth(k))];
        };
        walk_frame(
            c, p, q,
            [&](std::int64_t pos) {
                const std::int32_t k = ranks.of[static_cast<std::size_t>(pos)];
                if (k < 0) return;
                ++n;
                fen.add(k, 1);
            },
            [&](std::int64_t pos) {
                const std::int32_t k = ranks.of[static_cast<std::size_t>(pos)];
                if (k < 0) return;
                --n;
                fen.add(k, -1);
            },
            [&](std::int64_t r, std::int64_t, std::int64_t) {
                if (n == 0 || n < min_count) {
                    out.null(r);
                    return;
                }
                if (level <= 0.0) {
                    out.set(r, at(1));
                } else if (level >= 1.0) {
                    out.set(r, at(n));
                } else {
                    const double pos = level * static_cast<double>(n - 1);
                    const std::int32_t lo = static_cast<std::int32_t>(pos);
                    const double frac = pos - static_cast<double>(lo);
                    out.set(r, lo + 1 >= n ? at(lo + 1)
                                           : at(lo + 1) * (1.0 - frac) +
                                                 at(lo + 2) * frac);
                }
            });
    }
    return out.series(TypeId::Float64);
}

dstd::Result<Series> frame_count_distinct(const FrameCtx& c,
                                          const Series& column) {
    const dstd::Result<ColumnView> view =
        view_of(column, "FRAME_COUNT_DISTINCT");
    if (!view) return view.error();
    const ColumnView& v = *view;
    Column<std::int64_t> out(c.lay.n);
    Ranks ranks;
    dstd::Vector<std::int32_t> freq;
    for (const auto& [p, q] : c.lay.parts) {
        ranks.build(c.lay, v, p, q, false);
        freq.assign(static_cast<std::size_t>(ranks.count), 0);
        std::int64_t distinct = 0;
        walk_frame(
            c, p, q,
            [&](std::int64_t pos) {
                const std::int32_t k = ranks.of[static_cast<std::size_t>(pos)];
                if (k >= 0 && freq[static_cast<std::size_t>(k)]++ == 0)
                    ++distinct;
            },
            [&](std::int64_t pos) {
                const std::int32_t k = ranks.of[static_cast<std::size_t>(pos)];
                if (k >= 0 && --freq[static_cast<std::size_t>(k)] == 0)
                    --distinct;
            },
            [&](std::int64_t r, std::int64_t, std::int64_t) {
                out.set(r, distinct);
            });
    }
    return out.series(TypeId::Int64);
}

dstd::Result<Series> frame_arg(const FrameCtx& c, const Series& column,
                               const Series& by, bool smallest,
                               std::int64_t min_count) {
    const dstd::Result<ColumnView> by_view =
        view_of(by, "FRAME_ARG_MAX/FRAME_ARG_MIN");
    if (!by_view) return by_view.error();
    const ColumnView& b = *by_view;
    dstd::Vector<std::int64_t> source(static_cast<std::size_t>(c.lay.n), -1);
    steps::SlidingExtreme dq;
    for (const auto& [p, q] : c.lay.parts) {
        dq.candidates.clear();
        std::int64_t n = 0;
        walk_frame(
            c, p, q,
            [&](std::int64_t pos) {
                const std::int64_t g = c.lay.row(p + pos);
                if (b.is_null(g)) return;
                ++n;
                dq.enter(pos, [&](std::int64_t back) {
                    const int cmp = b.compare(g, c.lay.row(p + back));
                    return smallest ? cmp < 0 : cmp > 0;
                });
            },
            [&](std::int64_t pos) {
                if (!b.is_null(c.lay.row(p + pos))) --n;
            },
            [&](std::int64_t r, std::int64_t lo, std::int64_t) {
                dq.drop_before(lo);
                if (!dq.empty() && n >= min_count)
                    source[static_cast<std::size_t>(r)] =
                        c.lay.row(p + dq.best());
            });
    }
    return pass_through(column, source);
}

dstd::Result<Series> frame_collect(const FrameCtx& c, const Series& column) {
    const std::size_t n = static_cast<std::size_t>(c.lay.n);
    dstd::Vector<std::int64_t> lows(n), highs(n);
    std::int64_t total = 0;
    bool too_many = false;
    for (const auto& [p, q] : c.lay.parts) {
        std::int64_t count = 0;
        walk_frame(
            c, p, q,
            [&](std::int64_t pos) {
                if (!column.is_null(c.lay.row(p + pos))) ++count;
            },
            [&](std::int64_t pos) {
                if (!column.is_null(c.lay.row(p + pos))) --count;
            },
            [&](std::int64_t r, std::int64_t lo, std::int64_t hi) {
                total += count;
                if (total > WINDOW_COLLECT_MAX_ELEMENTS) too_many = true;
                lows[static_cast<std::size_t>(r)] = p + lo;
                highs[static_cast<std::size_t>(r)] = p + hi;
            });
    }
    if (too_many)
        return common::make_error<
            "window: FRAME_COLLECT frames hold more than 2^27 values in all">(
            common::ErrorCode::InvalidArgument);
    dstd::Vector<std::int32_t> offsets(n + 1, 0);
    dstd::Vector<std::int64_t> source;
    source.reserve(static_cast<std::size_t>(total));
    for (std::size_t r = 0; r < n; ++r) {
        for (std::int64_t pos = lows[r]; pos <= highs[r]; ++pos) {
            const std::int64_t g = c.lay.row(pos);
            if (!column.is_null(g)) source.push_back(g);
        }
        offsets[r + 1] = static_cast<std::int32_t>(source.size());
    }
    return Series::list(offsets, pass_through(column, source));
}

dstd::Result<Series> frame_stats(const Layout& lay, const Series& column,
                                 const Series* by, const WindowColumn& w) {
    const dstd::Result<FrameCtx> ctx = frame_context(lay, w);
    if (!ctx) return ctx.error();
    const FrameCtx& c = *ctx;
    const std::int64_t min_count = w.params.frame.min_count;
    switch (w.func) {
        case WindowFunc::FrameVar:
        case WindowFunc::FrameStd:
            return frame_variance(c, column, w.func == WindowFunc::FrameStd,
                                  min_count);
        case WindowFunc::FrameQuantile:
            return frame_quantile(c, column, w.params.frame.q, min_count);
        case WindowFunc::FrameCountDistinct:
            return frame_count_distinct(c, column);
        case WindowFunc::FrameArgMax:
        case WindowFunc::FrameArgMin:
            return frame_arg(c, column, *by, w.func == WindowFunc::FrameArgMin,
                             min_count);
        default:
            return frame_collect(c, column);
    }
}

}  // namespace

WindowColumn window_column(const dftu_window_spec& s) {
    WindowColumn c;
    c.func = static_cast<WindowFunc>(s.func);
    if (s.value) c.set_value(s.value);
    c.out = s.out ? s.out : "";
    if (is_frame_func(c.func)) {
        c.params.frame = {s.param.frame.min_count, s.param.frame.preceding,
                          s.param.frame.following,
                          static_cast<WindowFrameMode>(s.param.frame.mode),
                          s.param.frame.q};
        if (s.param.frame.by) c.set_by(s.param.frame.by);
    } else if (is_offset_func(c.func)) {
        c.params.offset = s.param.offset;
    } else if (c.func == WindowFunc::Rate) {
        if (s.param.rate.time) c.set_time(s.param.rate.time);
        c.params.rate = {s.param.rate.counter != 0};
    } else if (c.func == WindowFunc::Sessionize) {
        if (s.param.session.time) c.set_time(s.param.session.time);
        if (s.param.session.end) c.set_end(s.param.session.end);
        c.params.session = {s.param.session.gap, s.param.session.span};
    }
    return c;
}

dstd::Result<DataFrame> window(const DataFrame& df,
                               const dstd::Vector<dstd::String>& partition_by,
                               const dstd::Vector<dstd::String>& order_by,
                               const dstd::Vector<WindowColumn>& specs) {
    dstd::Result<dstd::Vector<ColumnView>> part_views =
        ops::views_of(df, partition_by, "window");
    if (!part_views) return part_views.error();
    dstd::Result<dstd::Vector<ColumnView>> order_views =
        ops::views_of(df, order_by, "window");
    if (!order_views) return order_views.error();
    dstd::Vector<ColumnView> parts = std::move(*part_views);
    dstd::Vector<ColumnView> order = std::move(*order_views);
    dstd::Vector<ColumnView> keys = parts;
    keys.insert(keys.end(), order.begin(), order.end());

    Layout lay;
    lay.n = df.num_rows();
    lay.idx = order_rows(keys, lay.n);
    lay.order_keys = &order;
    for (std::int64_t p = 0; p < lay.n;) {
        const std::int64_t q =
            parts.empty() ? lay.n : run_end(parts, lay.idx, p);
        lay.parts.emplace_back(p, q);
        p = q;
    }

    DataFrame out;
    for (std::size_t c = 0; c < df.columns.size(); ++c) {
        out.names.push_back(df.names[c]);
        out.columns.push_back(df.columns[c].take(lay.idx));
    }
    for (const WindowColumn& w : specs) {
        dstd::Result<Series> column = Series{};
        switch (w.func) {
            case WindowFunc::RowNumber:
            case WindowFunc::Rank:
            case WindowFunc::DenseRank:
            case WindowFunc::RunningCount:
            case WindowFunc::PercentRank:
            case WindowFunc::CumeDist:
                column = ranking(lay, w.func);
                break;
            case WindowFunc::Ntile:
                column = ntile(lay, w.params.offset);
                break;
            case WindowFunc::Lag:
            case WindowFunc::Lead:
            case WindowFunc::FirstValue:
            case WindowFunc::LastValue:
            case WindowFunc::NthValue:
            case WindowFunc::FillForward: {
                const dstd::Result<const Series*> value = value_column(df, w);
                if (!value) return value.error();
                column = positional(lay, **value, w);
                break;
            }
            case WindowFunc::RunningSum:
            case WindowFunc::RunningMin:
            case WindowFunc::RunningMax:
            case WindowFunc::RunningProd: {
                const dstd::Result<const Series*> value = value_column(df, w);
                if (!value) return value.error();
                column = running(lay, **value, w);
                break;
            }
            case WindowFunc::Delta: {
                const dstd::Result<const Series*> value = value_column(df, w);
                if (!value) return value.error();
                column = delta(lay, **value);
                break;
            }
            case WindowFunc::Rate: {
                const dstd::Result<const Series*> value = value_column(df, w);
                if (!value) return value.error();
                const dstd::Result<const Series*> time =
                    named_column(df, w.time(), w, "time");
                if (!time) return time.error();
                column = rate(lay, **value, **time, w.params.rate.counter);
                break;
            }
            case WindowFunc::Sessionize: {
                const Series* end = nullptr;
                if (w.end()) {
                    const dstd::Result<const Series*> end_column =
                        named_column(df, w.end(), w, "end");
                    if (!end_column) return end_column.error();
                    end = *end_column;
                }
                const dstd::Result<const Series*> time =
                    named_column(df, w.time(), w, "time");
                if (!time) return time.error();
                column = sessionize(lay, **time, end, w.params.session.gap,
                                    w.params.session.span);
                break;
            }
            case WindowFunc::FrameSum:
            case WindowFunc::FrameMin:
            case WindowFunc::FrameMax:
            case WindowFunc::FrameCount:
            case WindowFunc::FrameMean: {
                const dstd::Result<const Series*> value = value_column(df, w);
                if (!value) return value.error();
                column = frame(lay, **value, w);
                break;
            }
            case WindowFunc::FrameVar:
            case WindowFunc::FrameStd:
            case WindowFunc::FrameQuantile:
            case WindowFunc::FrameCountDistinct:
            case WindowFunc::FrameCollect: {
                const dstd::Result<const Series*> value = value_column(df, w);
                if (!value) return value.error();
                column = frame_stats(lay, **value, nullptr, w);
                break;
            }
            case WindowFunc::FrameArgMax:
            case WindowFunc::FrameArgMin: {
                const dstd::Result<const Series*> by =
                    named_column(df, w.by(), w, "by");
                if (!by) return by.error();
                const dstd::Result<const Series*> value = value_column(df, w);
                if (!value) return value.error();
                column = frame_stats(lay, **value, *by, w);
                break;
            }
        }
        if (!column) return column.error();
        out.names.push_back(w.out);
        out.columns.push_back(std::move(*column));
    }
    return out;
}

}  // namespace dftracer::utils::dataframe
