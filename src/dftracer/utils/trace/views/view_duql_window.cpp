// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/query/overlap.h>
#include <dftracer/utils/dataframe/query/vectorize.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_duql_internal.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace dftracer::utils::trace::views::duql_detail {

namespace {

// Keeps a row index plus the seed inside int64.
constexpr std::uint64_t SEED_MASK = (std::uint64_t{1} << 62) - 1;

}  // namespace

Status Applier::stage(const duql::PipelineSample& s) {
    leave_scan("sample");
    const dstd::String seed = dstd::fmt<" seed {}">(s.seed);
    if (!s.percent) {
        const auto n = static_cast<std::int64_t>(s.amount);
        lines.push_back(dstd::fmt<"sample {}{}">(n, seed));
        v_ = v_.with_lazy(v_.lazy().sample(n, s.seed));
        return {};
    }
    const dstd::Vector<dstd::String> before = names();
    const dstd::String row = "__duql_row";
    v_ = v_.with_lazy(v_.lazy().with_row_index(std_str(row)));
    cols_.insert(cols_.begin(),
                 {row, dataframe::scalar(dataframe::TypeId::Int64), false});
    const dataframe::Expr hash = dataframe::expr_cast(
        dataframe::TypeId::Float64,
        dataframe::expr_prim(
            dataframe::PrimOp::Mix64,
            dataframe::expr_arith(dataframe::ArithOp::Add, col(row),
                                  dataframe::expr_lit(static_cast<std::int64_t>(
                                      s.seed & SEED_MASK)))));
    // mix64 is signed: uniform over [-2^63, 2^63).
    if (s.amount < 100)
        v_ = v_.filter(dataframe::expr_cmp_expr(
            dataframe::CmpOp::Lt, hash,
            dataframe::expr_lit(-9223372036854775808.0 +
                                s.amount / 100.0 * 18446744073709551616.0)));
    lines.push_back(dstd::fmt<"sample {}%{}">(number_text(s.amount), seed));
    return keep(before);
}

double Applier::us_per_time_unit() const {
    return static_cast<double>(roles_.time_ns_per_unit) / 1000.0;
}

Status Applier::stage(const duql::PipelineTimeRange& t) {
    if (scan_ && !t.overlap && std::isfinite(t.low) && std::isfinite(t.high)) {
        v_ = v_.time_range(t.low * us_per_time_unit(),
                           t.high * us_per_time_unit());
        lines.push_back(dstd::fmt<"scan time_range {} .. {} (pushed)">(
            number_text(t.low), number_text(t.high)));
        return {};
    }
    if (scan_) {
        v_ = v_.filter(
            duql::detail::QueryAccess::from_node(duql::detail::make_node(
                duql::detail::ExprLeaf{t.condition, t.text})));
        lines.push_back(dstd::fmt<"scan filter: {} (pushed)">(t.text));
        return {};
    }
    auto condition = compile(t.condition, dstd::String(t.text), true);
    if (!condition) return condition.error();
    v_ = v_.filter(std::move(*condition));
    lines.push_back(dstd::fmt<"filter after scan: {}">(t.text));
    return {};
}

Status Applier::stage(const duql::PipelineCallTree&) {
    scan_ = false;
    blocker_ = "call_tree";
    lines.push_back("call_tree pid, tid");
    v_ = v_.with_lazy(v_.call_tree({"pid", "tid"}, dstd::String(roles_.time),
                                   dstd::String(roles_.duration), "name"));
    columns_from_view();
    dstd::Vector<dstd::String> to = names();
    for (auto& n : to) {
        if (n == "level") n = "depth";
        if (n == "parent_id") n = "parent";
    }
    rename(std::move(to));
    return {};
}

void Applier::row_index(const dstd::String& name) {
    v_ = v_.with_lazy(v_.lazy().with_row_index(std_str(name)));
    cols_.insert(cols_.begin(),
                 {name, dataframe::scalar(dataframe::TypeId::Int64), false});
}

dstd::Optional<std::size_t> Applier::field_index(
    const dstd::String& field) const {
    if (auto i = index(field)) return i;
    return arg_index(field);
}

void Applier::window_op(const dstd::Vector<dstd::String>& part,
                        const dstd::String& order,
                        const dstd::Vector<WinSpec>& specs) {
    if (specs.empty()) return;
    dstd::Vector<const char*> keys;
    for (const auto& k : part) keys.push_back(k.c_str());
    const char* by = order.c_str();
    dstd::Vector<dftu_window_spec> raw;
    dstd::Vector<dstd::String> out = names();
    for (const auto& s : specs) {
        dftu_window_spec w{};
        w.func = s.func;
        w.value = s.value.empty() ? nullptr : s.value.c_str();
        w.out = s.out.c_str();
        if ((s.func >= DFTU_WINDOW_FRAME_SUM &&
             s.func <= DFTU_WINDOW_FRAME_MEAN) ||
            s.func >= DFTU_WINDOW_FRAME_VAR)
            w.param.frame = {
                s.offset, s.preceding, s.following,
                s.mode,   s.q,         s.by.empty() ? nullptr : s.by.c_str()};
        else
            w.param.offset = s.offset;
        raw.push_back(w);
        out.push_back(s.out);
        cols_.push_back({s.out, s.type, false});
    }
    dataframe::OpArgs a;
    a.strlist(1, keys.data(), static_cast<std::int32_t>(keys.size()))
        .strlist(2, &by, 1)
        .winlist(3, raw);
    v_ = v_.with_lazy(
        v_.lazy().frame_op("dftu.frame.window", a, {}, std_strs(out)));
}

void Applier::partition_aggs(
    const dstd::Vector<dstd::Pair<const duql::PipelineWinCall*, dstd::String>>&
        calls,
    const dstd::String& pid) {
    dstd::Vector<dataframe::GroupAgg> specs;
    dstd::Vector<dstd::String> quantiles;
    dstd::Vector<dataframe::query::VectorColumn> next = cols_;
    dstd::String text;
    for (const auto& [c, in] : calls) {
        dataframe::GroupAgg g{dataframe::Agg::Var, std_str(in),
                              std_str(c->column)};
        dataframe::DataType type =
            dataframe::scalar(dataframe::TypeId::Float64);
        switch (c->fn) {
            case duql::WinFn::Std:
                g.op = dataframe::Agg::Std;
                break;
            case duql::WinFn::Quantile:
                g.op = dataframe::Agg::Pct;
                g.param = c->q;
                quantiles.push_back(dstd::String(c->column));
                break;
            case duql::WinFn::Histogram:
                g.op = dataframe::Agg::Hist;
                type = dataframe::list_of(dataframe::struct_of(
                    {dataframe::Field{
                         "lo", dataframe::scalar(dataframe::TypeId::Float64),
                         true},
                     dataframe::Field{
                         "hi", dataframe::scalar(dataframe::TypeId::Float64),
                         true},
                     dataframe::Field{
                         "count", dataframe::scalar(dataframe::TypeId::Uint64),
                         true}}));
                break;
            default:
                break;
        }
        specs.push_back(std::move(g));
        next.push_back({dstd::String(c->column), std::move(type)});
        if (!text.empty()) text += ", ";
        text += c->text;
    }
    dataframe::Schema schema;
    for (const auto& c : next)
        schema.fields.push_back(
            dataframe::Field{dstd::String(c.name), c.type, true});
    const dstd::String spid(pid);
    finish(
        [spid,
         specs](dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            auto g = f.group_by(dstd::Vector<dstd::String>{spid}, specs);
            if (!g) return g.error();
            return f.join(*g, {spid}, {spid}, dataframe::JoinHow::Lookup);
        },
        dstd::fmt<"partition aggregates {}">(text), std::move(schema));
    cols_ = std::move(next);
    for (const auto& q : quantiles)
        v_ = v_.with_column(
            std_str(q),
            dataframe::expr_select(
                dataframe::expr_unary(dataframe::UnaryOp::IsNan, col(q)),
                dataframe::expr_lit_null(dataframe::TypeId::Float64), col(q)));
}

dataframe::DataType Applier::type_of(const dstd::String& column) const {
    return cols_[*index(column)].type;
}

dstd::Result<dataframe::DataType> Applier::sum_type(const dstd::String& in,
                                                    const dstd::String& text) {
    const dataframe::TypeId t = type_of(in).id;
    if (t != dataframe::TypeId::Unknown && !is_number(t))
        return refuse<"'{}' in a window needs numbers">(text);
    return dataframe::scalar(is_integer(t) ? dataframe::TypeId::Int64
                                           : dataframe::TypeId::Float64);
}

dataframe::Expr Applier::differs(const dstd::String& a, const dstd::String& b) {
    const dataframe::Expr na = dataframe::expr_is_null(col(a));
    const dataframe::Expr nb = dataframe::expr_is_null(col(b));
    return dataframe::expr_select(
        na, dataframe::expr_not(nb),
        dataframe::expr_select(
            nb, dataframe::expr_lit_bool(true),
            dataframe::expr_cmp_expr(dataframe::CmpOp::Ne, col(a), col(b))));
}

Status Applier::window_framed(WindowPasses& p, WinSpec s,
                              const duql::PipelineWinCall& c) {
    if (!c.frame) {
        p.forward.push_back(std::move(s));
        return {};
    }
    s.offset = 1;
    s.following = 0;
    if (c.frame->range) {
        if (p.sort_keys.size() != 1)
            return refuse<"'over' needs one sort key">();
        const dataframe::TypeId t = type_of(p.sort_keys[0]).id;
        if (t != dataframe::TypeId::Unknown && !is_number(t))
            return refuse<"'{}' needs a numeric sort key">(c.text);
        s.preceding = static_cast<std::int64_t>(c.frame->width);
        s.mode = DFTU_WINDOW_FRAME_RANGE;
        p.ranged.push_back(std::move(s));
    } else {
        s.preceding = c.frame->rows - 1;
        p.forward.push_back(std::move(s));
    }
    return {};
}

dstd::Result<dstd::String> Applier::window_input(const duql::PipelineWinCall& c,
                                                 std::size_t i) {
    dstd::String in;
    if (c.fn == duql::WinFn::CountIf) {
        in = dstd::fmt<"__duql_in_{}">(i);
        auto condition = compile(c.arg, dstd::String(c.text), true);
        if (!condition) return condition.error();
        if (auto done = with_column(
                in,
                dataframe::expr_select(
                    *condition, dataframe::expr_lit(std::int64_t{1}),
                    dataframe::expr_lit_null(dataframe::TypeId::Int64)),
                dstd::String(c.text));
            !done)
            return done.error();
    } else if (c.arg) {
        auto column = source({"", c.arg, c.text}, dstd::fmt<"__duql_in_{}">(i));
        if (!column) return column.error();
        in = std::move(*column);
    }
    return in;
}

Status Applier::window_arg(WindowPasses& p, const duql::PipelineWinCall& c,
                           std::size_t i, const dstd::String& in) {
    WinSpec s{c.fn == duql::WinFn::Argmax ? DFTU_WINDOW_FRAME_ARG_MAX
                                          : DFTU_WINDOW_FRAME_ARG_MIN,
              in, dstd::String(c.column), type_of(in)};
    auto by = source({"", c.by, c.text}, dstd::fmt<"__duql_by_{}">(i));
    if (!by) return by.error();
    s.by = std::move(*by);
    return window_framed(p, std::move(s), c);
}

Status Applier::window_call(WindowPasses& p, const duql::PipelineWinCall& c,
                            std::size_t i) {
    const dataframe::DataType i64 = dataframe::scalar(dataframe::TypeId::Int64);
    auto input = window_input(c, i);
    if (!input) return input.error();
    const dstd::String in = std::move(*input);
    const dstd::String tmp = dstd::fmt<"__duql_fill_{}">(i);
    const dstd::String text(c.text);
    switch (c.fn) {
        case duql::WinFn::RowNumber:
            p.forward.push_back(
                {DFTU_WINDOW_ROW_NUMBER, "", dstd::String(c.column), i64});
            break;
        case duql::WinFn::RunningCount:
            p.forward.push_back(
                {DFTU_WINDOW_RUNNING_COUNT, "", dstd::String(c.column), i64});
            break;
        case duql::WinFn::Lag:
        case duql::WinFn::Lead:
            p.forward.push_back(
                {c.fn == duql::WinFn::Lag ? DFTU_WINDOW_LAG : DFTU_WINDOW_LEAD,
                 in, dstd::String(c.column), type_of(in), c.offset});
            if (c.fallback) {
                const dstd::String edge = dstd::fmt<"__duql_edge_{}">(i);
                p.forward.push_back(
                    {p.forward.back().func, p.pos, edge, i64, c.offset});
                p.edges.emplace_back(edge, &c);
            }
            break;
        case duql::WinFn::RunningMin:
        case duql::WinFn::RunningMax:
            p.forward.push_back({c.fn == duql::WinFn::RunningMin
                                     ? DFTU_WINDOW_RUNNING_MIN
                                     : DFTU_WINDOW_RUNNING_MAX,
                                 in, dstd::String(c.column), type_of(in)});
            break;
        case duql::WinFn::RunningMean:
            if (auto numbers = sum_type(in, text); !numbers)
                return numbers.error();
            p.forward.push_back(
                {DFTU_WINDOW_FRAME_MEAN, in, dstd::String(c.column),
                 dataframe::scalar(dataframe::TypeId::Float64), 1, 0});
            break;
        case duql::WinFn::Ntile:
            p.forward.push_back(
                {DFTU_WINDOW_NTILE, "", dstd::String(c.column), i64, c.offset});
            break;
        case duql::WinFn::Nth:
            p.forward.push_back({DFTU_WINDOW_NTH_VALUE, in,
                                 dstd::String(c.column), type_of(in),
                                 c.offset});
            break;
        case duql::WinFn::FillForward:
            p.forward.push_back({DFTU_WINDOW_FILL_FORWARD, in,
                                 dstd::String(c.column), type_of(in)});
            break;
        case duql::WinFn::PercentRank:
        case duql::WinFn::CumeDist:
            p.dist.push_back(&c);
            break;
        case duql::WinFn::RunningSum: {
            auto type = sum_type(in, text);
            if (!type) return type.error();
            p.forward.push_back({DFTU_WINDOW_FRAME_SUM, in,
                                 dstd::String(c.column), *type, 0, 0});
            break;
        }
        case duql::WinFn::Count:
            return window_framed(
                p,
                {DFTU_WINDOW_FRAME_COUNT, in.empty() ? p.pos : in,
                 dstd::String(c.column), i64},
                c);
        case duql::WinFn::Sum: {
            auto type = sum_type(in, text);
            if (!type) return type.error();
            return window_framed(
                p, {DFTU_WINDOW_FRAME_SUM, in, dstd::String(c.column), *type},
                c);
        }
        case duql::WinFn::Mean:
            if (auto numbers = sum_type(in, text); !numbers)
                return numbers.error();
            return window_framed(
                p,
                {DFTU_WINDOW_FRAME_MEAN, in, dstd::String(c.column),
                 dataframe::scalar(dataframe::TypeId::Float64)},
                c);
        case duql::WinFn::Min:
        case duql::WinFn::Max:
            return window_framed(
                p,
                {c.fn == duql::WinFn::Min ? DFTU_WINDOW_FRAME_MIN
                                          : DFTU_WINDOW_FRAME_MAX,
                 in, dstd::String(c.column), type_of(in)},
                c);
        case duql::WinFn::Last:
            p.forward.push_back(
                {DFTU_WINDOW_FILL_FORWARD, in, tmp, type_of(in)});
            p.reverse.push_back({DFTU_WINDOW_FIRST_VALUE, tmp,
                                 dstd::String(c.column), type_of(in)});
            break;
        case duql::WinFn::First:
            p.reverse.push_back(
                {DFTU_WINDOW_FILL_FORWARD, in, tmp, type_of(in)});
            p.last.push_back({DFTU_WINDOW_FIRST_VALUE, tmp,
                              dstd::String(c.column), type_of(in)});
            break;
        case duql::WinFn::Rank:
        case duql::WinFn::DenseRank:
            p.ranks.push_back(&c);
            break;
        case duql::WinFn::CountIf:
            return window_framed(
                p, {DFTU_WINDOW_FRAME_COUNT, in, dstd::String(c.column), i64},
                c);
        case duql::WinFn::CountDistinct:
            return window_framed(p,
                                 {DFTU_WINDOW_FRAME_COUNT_DISTINCT, in,
                                  dstd::String(c.column), i64},
                                 c);
        case duql::WinFn::Collect:
            return window_framed(
                p,
                {DFTU_WINDOW_FRAME_COLLECT, in, dstd::String(c.column),
                 dataframe::list_of(type_of(in))},
                c);
        case duql::WinFn::Argmax:
        case duql::WinFn::Argmin:
            return window_arg(p, c, i, in);
        case duql::WinFn::Var:
        case duql::WinFn::Std:
        case duql::WinFn::Quantile:
            if (c.frame) {
                if (auto numbers = sum_type(in, text); !numbers)
                    return numbers.error();
                WinSpec s{c.fn == duql::WinFn::Var ? DFTU_WINDOW_FRAME_VAR
                          : c.fn == duql::WinFn::Std
                              ? DFTU_WINDOW_FRAME_STD
                              : DFTU_WINDOW_FRAME_QUANTILE,
                          in, dstd::String(c.column),
                          dataframe::scalar(dataframe::TypeId::Float64)};
                s.q = c.q;
                return window_framed(p, std::move(s), c);
            }
            [[fallthrough]];
        case duql::WinFn::Histogram:
            if (auto numbers = sum_type(in, text); !numbers)
                return numbers.error();
            p.partition.emplace_back(&c, in);
            break;
    }
    return {};
}

Status Applier::window_peers(WindowPasses& p, bool peers) {
    const dataframe::DataType i64 = dataframe::scalar(dataframe::TypeId::Int64);
    if (peers) {
        dataframe::Expr peer =
            dataframe::expr_cmp(dataframe::CmpOp::Eq, col("__duql_rn"),
                                dataframe::detail::expr_scalar_i(1));
        for (std::size_t k = 0; k < p.sort_keys.size(); ++k)
            peer = dataframe::expr_logical(
                dataframe::LogicalOp::Or, peer,
                differs(p.sort_keys[k], dstd::fmt<"__duql_prev_{}">(k)));
        if (auto done =
                with_column("__duql_peer",
                            dataframe::expr_select(
                                peer, dataframe::expr_lit(std::int64_t{1}),
                                dataframe::expr_lit(std::int64_t{0})),
                            "peer start");
            !done)
            return done;
        if (auto done = with_column(
                "__duql_start",
                dataframe::expr_select(
                    peer, col("__duql_rn"),
                    dataframe::expr_lit_null(dataframe::TypeId::Int64)),
                "peer start row");
            !done)
            return done;
        for (const auto* c : p.ranks)
            p.last.push_back(c->fn == duql::WinFn::Rank
                                 ? WinSpec{DFTU_WINDOW_FILL_FORWARD,
                                           "__duql_start",
                                           dstd::String(c->column), i64}
                                 : WinSpec{DFTU_WINDOW_FRAME_SUM, "__duql_peer",
                                           dstd::String(c->column), i64, 0, 0});
        if (!p.dist.empty())
            p.last.push_back(
                {DFTU_WINDOW_FRAME_SUM, "__duql_peer", "__duql_dr", i64, 0, 0});
    } else {
        for (const auto* c : p.ranks)
            if (auto done = with_column(dstd::String(c->column),
                                        dataframe::expr_lit(std::int64_t{1}),
                                        dstd::String(c->text));
                !done)
                return done;
        if (!p.dist.empty())
            return with_column("__duql_dr",
                               dataframe::expr_lit(std::int64_t{1}),
                               "peer group");
    }
    return {};
}

Status Applier::stage(const duql::PipelineWindow& w) {
    leave_scan("window");
    const dstd::Vector<dstd::String> before = names();
    const dstd::String pos = "__duql_pos";
    row_index(pos);
    WindowPasses p;
    p.pos = pos;
    dstd::Vector<dstd::String> part;
    dstd::String keys_text;
    for (std::size_t k = 0; k < w.keys.size(); ++k) {
        auto column = source(w.keys[k], dstd::fmt<"__duql_part_{}">(k));
        if (!column) return column.error();
        part.push_back(std::move(*column));
        if (!keys_text.empty()) keys_text += ", ";
        keys_text += w.keys[k].text;
    }
    dstd::String ord = pos;
    dstd::String sort_text;
    if (!w.order.empty()) {
        auto sorted = sort_columns(w.order);
        if (!sorted) return sorted.error();
        SortColumns c = std::move(*sorted);
        p.sort_keys = c.keys;
        lines.push_back(dstd::fmt<"sort_by_multi {}">(joined(c.by)));
        v_ = v_.sort_by_multi(std_strs(c.by), std::move(c.desc));
        ord = "__duql_ord";
        row_index(ord);
        for (const auto& k : w.order) {
            if (!sort_text.empty()) sort_text += ", ";
            if (k.descending) sort_text += '-';
            sort_text += k.key.text;
        }
    }
    for (std::size_t i = 0; i < w.calls.size(); ++i)
        if (auto done = window_call(p, w.calls[i], i); !done) return done;
    const dataframe::DataType i64 = dataframe::scalar(dataframe::TypeId::Int64);
    const dstd::String pid = "__duql_pid";
    if (!p.partition.empty())
        p.forward.push_back({DFTU_WINDOW_FIRST_VALUE, pos, pid, i64});
    const bool peers =
        (!p.ranks.empty() || !p.dist.empty()) && !p.sort_keys.empty();
    if (peers) {
        p.forward.push_back({DFTU_WINDOW_ROW_NUMBER, "", "__duql_rn", i64});
        for (std::size_t k = 0; k < p.sort_keys.size(); ++k)
            p.forward.push_back({DFTU_WINDOW_LAG, p.sort_keys[k],
                                 dstd::fmt<"__duql_prev_{}">(k),
                                 type_of(p.sort_keys[k]), 1});
    }
    window_op(part, ord, p.forward);
    window_op(part, p.sort_keys.empty() ? ord : p.sort_keys[0], p.ranged);
    for (const auto& [edge, c] : p.edges) {
        auto fallback = compile(c->fallback, dstd::String(c->text));
        if (!fallback) return fallback.error();
        if (auto done = with_column(
                dstd::String(c->column),
                dataframe::expr_select(dataframe::expr_is_null(col(edge)),
                                       *fallback, col(c->column)),
                dstd::String(c->text));
            !done)
            return done;
    }
    if (auto done = window_peers(p, peers); !done) return done;
    if (!p.reverse.empty()) {
        if (auto done = with_column("__duql_rev", dataframe::expr_neg(col(ord)),
                                    dstd::fmt<"-{}">(ord));
            !done)
            return done;
        window_op(part, "__duql_rev", p.reverse);
    }
    window_op(part, ord, p.last);
    for (const auto* c : p.dist)
        p.distribution.push_back(
            {c->fn == duql::WinFn::PercentRank ? DFTU_WINDOW_PERCENT_RANK
                                               : DFTU_WINDOW_CUME_DIST,
             "", dstd::String(c->column),
             dataframe::scalar(dataframe::TypeId::Float64)});
    window_op(part, "__duql_dr", p.distribution);
    if (!p.partition.empty()) partition_aggs(p.partition, pid);
    lines.push_back(dstd::fmt<"sort_by_multi {}">(pos));
    v_ = v_.sort_by_multi({std_str(pos)}, dstd::Vector<bool>{false});
    dstd::Vector<dstd::String> keep_names = before;
    dstd::String items;
    for (const auto& it : w.items) {
        auto compiled = compile(it.term, dstd::String(it.text));
        if (!compiled) return compiled.error();
        if (auto done = with_column(dstd::String(it.name), *compiled,
                                    dstd::String(it.text));
            !done)
            return done;
        if (std::find(keep_names.begin(), keep_names.end(),
                      dstd::String(it.name)) == keep_names.end())
            keep_names.push_back(dstd::String(it.name));
        if (!items.empty()) items += ", ";
        items += dstd::fmt<"{} = {}">(it.name, it.text);
    }
    lines.push_back(
        dstd::fmt<"window keys {}; sort {}; {}">(keys_text, sort_text, items));
    return keep(std::move(keep_names));
}

}  // namespace dftracer::utils::trace::views::duql_detail
