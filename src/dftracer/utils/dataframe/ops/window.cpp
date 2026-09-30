#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/kernels/order.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

#include "ops_common.h"

namespace dftracer::utils::dataframe {

namespace {

using Kind = ColumnView::Kind;

// The rows in (partition, order) sorted order and the partitions in it.
struct Layout {
    std::int64_t n = 0;
    std::vector<std::int64_t> idx;
    std::vector<std::pair<std::int64_t, std::int64_t>> parts;
    const std::vector<ColumnView>* order_keys = nullptr;

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
    std::vector<T> value;
    std::vector<std::uint8_t> valid;

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
                    const std::vector<std::int64_t>& source) {
    return column.take(source);
}

__extension__ typedef __int128 Wide;

[[noreturn]] void overflow(const char* func, const char* type) {
    throw std::overflow_error(std::string("window: ") + func + " overflows " +
                              type);
}

std::int64_t to_int64(Wide v, const char* func) {
    if (v < std::numeric_limits<std::int64_t>::min() ||
        v > std::numeric_limits<std::int64_t>::max())
        overflow(func, "int64");
    return static_cast<std::int64_t>(v);
}

std::uint64_t to_uint64(Wide v, const char* func) {
    if (v < 0 ||
        v > static_cast<Wide>(std::numeric_limits<std::uint64_t>::max()))
        overflow(func, "uint64");
    return static_cast<std::uint64_t>(v);
}

const ColumnView& numeric(const ColumnView& v, const char* what) {
    if (v.is_bytes())
        throw std::invalid_argument(std::string("window: ") + what +
                                    " over a non-numeric column");
    return v;
}

ColumnView view_of(const Series& column, const char* what) {
    if (!ColumnView::supports(column.type()))
        throw std::invalid_argument(std::string("window: ") + what +
                                    " over an unsupported column type");
    return ColumnView(column);
}

const Series& value_column(const DataFrame& df, const WindowColumn& w) {
    if (!w.value())
        throw std::invalid_argument("window: '" + w.out +
                                    "' needs a value column");
    return df.columns[ops::column_named(df, *w.value(), "window")];
}

const Series& named_column(const DataFrame& df, const std::string* name,
                           const WindowColumn& w, const char* role) {
    if (!name)
        throw std::invalid_argument("window: '" + w.out + "' needs a " + role +
                                    " column");
    return df.columns[ops::column_named(df, *name, "window")];
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
    std::vector<std::int64_t> source(static_cast<std::size_t>(lay.n), -1);
    for (const auto& [p, q] : lay.parts) {
        std::int64_t last = -1;
        for (std::int64_t r = p; r < q; ++r) {
            const std::int64_t local = r - p;
            std::int64_t from = -1;
            switch (w.func) {
                case WindowFunc::Lag:
                case WindowFunc::Lead: {
                    const std::int64_t s = w.func == WindowFunc::Lag
                                               ? local - w.params.offset
                                               : local + w.params.offset;
                    if (s >= 0 && s < q - p) from = lay.row(p + s);
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
                    if (!column.is_null(lay.row(r))) last = lay.row(r);
                    from = last;
                    break;
                }
            }
            source[static_cast<std::size_t>(r)] = from;
        }
    }
    return pass_through(column, source);
}

Series running(const Layout& lay, const Series& column, const WindowColumn& w) {
    const ColumnView v = view_of(column, "running aggregate");
    switch (w.func) {
        case WindowFunc::RunningSum: {
            numeric(v, "running aggregate");
            if (v.kind() == Kind::Float) {
                Column<double> out(lay.n);
                for (const auto& [p, q] : lay.parts) {
                    double sum = 0.0;
                    for (std::int64_t r = p; r < q; ++r) {
                        const std::int64_t g = lay.row(r);
                        if (!v.is_null(g)) sum += v.get_double(g);
                        out.set(r, sum);
                    }
                }
                return out.series(TypeId::Float64);
            }
            if (v.kind() == Kind::Unsigned) {
                Column<std::uint64_t> out(lay.n);
                for (const auto& [p, q] : lay.parts) {
                    std::uint64_t sum = 0;
                    for (std::int64_t r = p; r < q; ++r) {
                        const std::int64_t g = lay.row(r);
                        if (!v.is_null(g) &&
                            __builtin_add_overflow(sum, v.get_uint(g), &sum))
                            overflow("RUNNING_SUM", "uint64");
                        out.set(r, sum);
                    }
                }
                return out.series(TypeId::Uint64);
            }
            Column<std::int64_t> out(lay.n);
            for (const auto& [p, q] : lay.parts) {
                std::int64_t sum = 0;
                for (std::int64_t r = p; r < q; ++r) {
                    const std::int64_t g = lay.row(r);
                    if (!v.is_null(g) &&
                        __builtin_add_overflow(sum, v.get_int(g), &sum))
                        overflow("RUNNING_SUM", "int64");
                    out.set(r, sum);
                }
            }
            return out.series(TypeId::Int64);
        }
        case WindowFunc::RunningProd: {
            numeric(v, "running aggregate");
            Column<double> out(lay.n);
            for (const auto& [p, q] : lay.parts) {
                double prod = 1.0;
                for (std::int64_t r = p; r < q; ++r) {
                    const std::int64_t g = lay.row(r);
                    if (!v.is_null(g)) prod *= v.get_double(g);
                    out.set(r, prod);
                }
            }
            return out.series(TypeId::Float64);
        }
        default: {  // RunningMin, RunningMax
            std::vector<std::int64_t> source(static_cast<std::size_t>(lay.n),
                                             -1);
            const bool smallest = w.func == WindowFunc::RunningMin;
            for (const auto& [p, q] : lay.parts) {
                std::int64_t ext = -1;
                for (std::int64_t r = p; r < q; ++r) {
                    const std::int64_t g = lay.row(r);
                    if (!v.is_null(g) &&
                        (ext < 0 || better(v, g, ext, smallest)))
                        ext = g;
                    source[static_cast<std::size_t>(r)] = ext;
                }
            }
            return pass_through(column, source);
        }
    }
}

Series delta(const Layout& lay, const Series& column) {
    const ColumnView v = view_of(column, "DELTA/RATE");
    numeric(v, "DELTA/RATE");
    const bool floating = v.kind() == Kind::Float;
    Column<double> fout(floating ? lay.n : 0);
    Column<std::int64_t> iout(floating ? 0 : lay.n);
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
            if (floating)
                fout.set(r, v.get_double(g) - v.get_double(prev));
            else if (v.kind() == Kind::Unsigned)
                iout.set(r, to_int64(static_cast<Wide>(v.get_uint(g)) -
                                         static_cast<Wide>(v.get_uint(prev)),
                                     "DELTA"));
            else
                iout.set(r, to_int64(static_cast<Wide>(v.get_int(g)) -
                                         static_cast<Wide>(v.get_int(prev)),
                                     "DELTA"));
        }
    }
    return floating ? fout.series(TypeId::Float64) : iout.series(TypeId::Int64);
}

Series rate(const Layout& lay, const Series& column, const Series& time,
            bool counter) {
    const ColumnView v = view_of(column, "DELTA/RATE");
    numeric(v, "DELTA/RATE");
    const ColumnView t = view_of(time, "RATE/SESSIONIZE");
    if (t.is_bytes())
        throw std::invalid_argument(
            "window: RATE/SESSIONIZE over a non-numeric time column");
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

Series sessionize(const Layout& lay, const Series& time,
                  const Series* end_column, double gap, double span) {
    if (!(gap >= 0.0) || !(span >= 0.0))
        throw std::invalid_argument(
            "window: SESSIONIZE gap and span must not be negative");
    const ColumnView t = view_of(time, "RATE/SESSIONIZE");
    if (t.is_bytes())
        throw std::invalid_argument(
            "window: RATE/SESSIONIZE over a non-numeric time column");
    std::optional<ColumnView> e;
    if (end_column != nullptr) {
        e.emplace(view_of(*end_column, "SESSIONIZE"));
        if (e->is_bytes())
            throw std::invalid_argument(
                "window: SESSIONIZE over a non-numeric end column");
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

Series frame(const Layout& lay, const Series& column, const WindowColumn& w) {
    const WindowParams::Frame& f = w.params.frame;
    if ((f.preceding < 0 && f.preceding != WINDOW_UNBOUNDED) ||
        (f.following < 0 && f.following != WINDOW_UNBOUNDED) || f.min_count < 0)
        throw std::invalid_argument(
            "window: frame bounds and min_count must not be negative");
    const bool range = f.mode == WindowFrameMode::Range;
    const std::vector<ColumnView>& order = *lay.order_keys;
    if (range && (order.size() != 1 || order[0].is_bytes()))
        throw std::invalid_argument(
            "window: RANGE frame needs exactly one numeric order column");

    const bool counting = w.func == WindowFunc::FrameCount;
    std::optional<ColumnView> vv;
    if (!counting) {
        vv.emplace(view_of(column, "frame aggregate"));
        if ((w.func == WindowFunc::FrameSum ||
             w.func == WindowFunc::FrameMean) &&
            vv->is_bytes())
            throw std::invalid_argument(
                "window: FRAME_SUM/FRAME_MEAN over a non-numeric column");
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
    std::vector<std::int64_t> source(
        min_max ? static_cast<std::size_t>(lay.n) : 0, -1);
    const bool as_double = w.func == WindowFunc::FrameMean || sum_double;
    const bool unsigned_sum =
        w.func == WindowFunc::FrameSum && vv->kind() == Kind::Unsigned;
    Column<std::uint64_t> uints(unsigned_sum ? lay.n : 0);

    for (const auto& [p, q] : lay.parts) {
        const std::int64_t sz = q - p;
        std::int64_t win_lo = 0, win_hi = -1, count = 0, first_missing = -1;
        Wide sum_i = 0;
        double sum_d = 0.0;
        std::deque<std::int64_t> dq;
        const auto position_row = [&](std::int64_t pos) {
            return lay.row(p + pos);
        };
        const auto enter = [&](std::int64_t pos) {
            const std::int64_t g = position_row(pos);
            if (!present(g)) return;
            ++count;
            if (counting) return;
            if (min_max) {
                while (!dq.empty()) {
                    const std::int64_t back = position_row(dq.back());
                    const int c = vv->compare(g, back);
                    if (smallest ? c <= 0 : c >= 0)
                        dq.pop_back();
                    else
                        break;
                }
                dq.push_back(pos);
            } else if (w.func == WindowFunc::FrameMean || sum_double) {
                sum_d += vv->get_double(g);
            } else if (vv->kind() == Kind::Unsigned) {
                sum_i += static_cast<Wide>(vv->get_uint(g));
            } else {
                sum_i += static_cast<Wide>(vv->get_int(g));
            }
        };
        const auto leave = [&](std::int64_t pos) {
            const std::int64_t g = position_row(pos);
            if (!present(g)) return;
            --count;
            if (counting || min_max) return;
            if (w.func == WindowFunc::FrameMean || sum_double)
                sum_d -= vv->get_double(g);
            else if (vv->kind() == Kind::Unsigned)
                sum_i -= static_cast<Wide>(vv->get_uint(g));
            else
                sum_i -= static_cast<Wide>(vv->get_int(g));
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
            while (!dq.empty() && dq.front() < lo) dq.pop_front();
            win_lo = lo;
            win_hi = hi;

            if (counting) {
                ints.set(r, count);
            } else if (min_max) {
                if (dq.empty() || count < f.min_count)
                    source[static_cast<std::size_t>(r)] = -1;
                else
                    source[static_cast<std::size_t>(r)] =
                        position_row(dq.front());
            } else if (count == 0 || count < f.min_count) {
                if (as_double)
                    doubles.null(r);
                else if (unsigned_sum)
                    uints.null(r);
                else
                    ints.null(r);
            } else if (w.func == WindowFunc::FrameMean) {
                doubles.set(r, sum_d / static_cast<double>(count));
            } else if (sum_double) {
                doubles.set(r, sum_d);
            } else if (unsigned_sum) {
                uints.set(r, to_uint64(sum_i, "FRAME_SUM"));
            } else {
                ints.set(r, to_int64(sum_i, "FRAME_SUM"));
            }
        }
    }
    if (min_max) return pass_through(column, source);
    if (unsigned_sum) return uints.series(TypeId::Uint64);
    return as_double ? doubles.series(TypeId::Float64)
                     : ints.series(TypeId::Int64);
}

}  // namespace

WindowColumn window_column(const dftu_window_spec& s) {
    WindowColumn c;
    c.func = static_cast<WindowFunc>(s.func);
    if (s.value) c.set_value(s.value);
    c.out = s.out ? s.out : "";
    if (is_frame_func(c.func)) {
        c.params.frame = {s.param.frame.min_count, s.param.frame.preceding,
                          s.param.frame.following, WindowFrameMode::Rows};
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

DataFrame window(const DataFrame& df,
                 const std::vector<std::string>& partition_by,
                 const std::vector<std::string>& order_by,
                 const std::vector<WindowColumn>& specs) {
    std::vector<ColumnView> parts = ops::views_of(df, partition_by, "window");
    std::vector<ColumnView> order = ops::views_of(df, order_by, "window");
    std::vector<ColumnView> keys = parts;
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
        Series column;
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
            case WindowFunc::FillForward:
                column = positional(lay, value_column(df, w), w);
                break;
            case WindowFunc::RunningSum:
            case WindowFunc::RunningMin:
            case WindowFunc::RunningMax:
            case WindowFunc::RunningProd:
                column = running(lay, value_column(df, w), w);
                break;
            case WindowFunc::Delta:
                column = delta(lay, value_column(df, w));
                break;
            case WindowFunc::Rate:
                column = rate(lay, value_column(df, w),
                              named_column(df, w.time(), w, "time"),
                              w.params.rate.counter);
                break;
            case WindowFunc::Sessionize: {
                const Series* end =
                    w.end() ? &named_column(df, w.end(), w, "end") : nullptr;
                column =
                    sessionize(lay, named_column(df, w.time(), w, "time"), end,
                               w.params.session.gap, w.params.session.span);
                break;
            }
            case WindowFunc::FrameSum:
            case WindowFunc::FrameMin:
            case WindowFunc::FrameMax:
            case WindowFunc::FrameCount:
            case WindowFunc::FrameMean:
                column = frame(lay, value_column(df, w), w);
                break;
        }
        out.names.push_back(w.out);
        out.columns.push_back(std::move(column));
    }
    return out;
}

}  // namespace dftracer::utils::dataframe
