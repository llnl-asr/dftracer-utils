// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dataframe/query/group_fold.h>
#include <dftracer/utils/dataframe/query/vectorize.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_duql_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace dftracer::utils::trace::views::duql_detail {

dstd::Optional<dataframe::TypeId> Applier::agg_type(
    const duql::PipelineAgg& a, dstd::Optional<dataframe::TypeId> in) const {
    switch (a.fn) {
        case duql::AggFn::Count:
        case duql::AggFn::CountIf:
        case duql::AggFn::CountDistinct:
            return dataframe::TypeId::Int64;
        case duql::AggFn::Sketch:
        case duql::AggFn::Merge:
            return dataframe::TypeId::String;
        case duql::AggFn::Collect:
        case duql::AggFn::Plugin:
            return dstd::nullopt;
        case duql::AggFn::Sum:
            if (!in) return dstd::nullopt;
            return is_integer(*in) ? dataframe::TypeId::Int64
                                   : dataframe::TypeId::Float64;
        case duql::AggFn::Min:
        case duql::AggFn::Max:
        case duql::AggFn::First:
        case duql::AggFn::Last:
        case duql::AggFn::Argmax:
        case duql::AggFn::Argmin:
            if (in && (is_number(*in) || *in == dataframe::TypeId::String ||
                       *in == dataframe::TypeId::Bool))
                return in;
            return dstd::nullopt;
        case duql::AggFn::Mean:
        case duql::AggFn::Var:
        case duql::AggFn::Std:
        case duql::AggFn::Quantile:
            return dataframe::TypeId::Float64;
        case duql::AggFn::Histogram:
        case duql::AggFn::Busy:
        case duql::AggFn::Concurrency:
        case duql::AggFn::Utilization:
        case duql::AggFn::Active:
            return dstd::nullopt;
    }
    return dstd::nullopt;
}

dstd::Result<dstd::Optional<dataframe::TypeId>> Applier::term_type(
    const duql::TermRef& t, const dstd::String& text) {
    if (!t) return dstd::Optional<dataframe::TypeId>();
    if (const auto* f = whole_field(t))
        if (const auto c = scan_column(dstd::String(f->base)))
            return c->type.id == dataframe::TypeId::Unknown
                       ? dstd::Optional<dataframe::TypeId>()
                       : dstd::Optional<dataframe::TypeId>(c->type.id);
    auto compiled = compile(t, text);
    if (!compiled) return compiled.error();
    auto inferred = dataframe::infer_type(*compiled, types());
    if (!inferred) return inferred.error();
    if (inferred->id == dataframe::TypeId::Unknown)
        return dstd::Optional<dataframe::TypeId>();
    return dstd::Optional<dataframe::TypeId>(inferred->id);
}

dstd::Optional<dataframe::query::VectorColumn> Applier::trace_column(
    const duql::TermRef& t) {
    const auto* f = whole_field(t);
    if (!f) return dstd::nullopt;
    auto c = scan_column(dstd::String(f->base));
    if (!c || c->json) return dstd::nullopt;
    return c;
}

dstd::Result<dstd::String> Applier::trace_refusal(
    const duql::PipelineGroup& g,
    const dstd::Optional<duql::PipelineBucket>& pending) {
    if (!scan_)
        return dstd::fmt<
            "'{}' comes before it; only where, time_range, bucket and a "
            "field select may">(blocker_);
    if (pending) {
        if (pending->every)
            return dstd::String(
                "a hopping bucket assigns a record to several windows");
        const double us = pending->width * us_per_time_unit();
        if (std::trunc(us) != us)
            return dstd::String(
                "its bucket width is not a whole number of microseconds");
        const double origin_us = pending->origin * us_per_time_unit();
        if (std::trunc(origin_us) != origin_us || origin_us < 0)
            return dstd::String(
                "its bucket origin is not a whole, non-negative number of "
                "microseconds");
    }
    // A select at the scan fixes the columns: a field it left out is no
    // column, whatever the files hold.
    if (scan_selected_) {
        dstd::String missing;
        for (const auto& k : g.keys)
            duql::for_each_term_field(*k.term, [&](const duql::TField& f) {
                if (!has_column_for(dstd::String(f.base))) missing = f.base;
            });
        for (const auto& a : g.aggs)
            if (a.arg)
                duql::for_each_term_field(*a.arg, [&](const duql::TField& f) {
                    if (!has_column_for(dstd::String(f.base))) missing = f.base;
                });
        if (!missing.empty())
            return refuse<"has no column '{}'; the columns here are {}">(
                missing, joined(names()));
    }
    dstd::Vector<dstd::String> seen;
    for (const auto& k : g.keys) {
        const auto c = trace_column(k.term);
        if (!c || (c->type.id != dataframe::TypeId::String &&
                   c->type.id != dataframe::TypeId::Int64 &&
                   c->type.id != dataframe::TypeId::Uint64))
            return dstd::fmt<"key '{}' is not a string or integer field">(
                k.text);
        for (const auto& s : seen)
            if (s == c->name) return dstd::String("a field is a key twice");
        seen.push_back(c->name);
    }
    for (const auto& a : g.aggs) {
        if (!trace_op(a))
            return dstd::fmt<"'{}' runs only on the scanned columns">(a.text);
        if (!a.arg) continue;
        const auto c = trace_column(a.arg);
        if (!c || !is_number(c->type.id))
            return dstd::fmt<"'{}' does not read a numeric field">(a.text);
    }
    return dstd::String();
}

Status Applier::stage(const duql::PipelineGroup& group) {
    dstd::Optional<duql::PipelineBucket> bucket = std::move(bucket_);
    bucket_.reset();
    // A sum over no values is null: a hidden mean tells which groups
    // had one.
    duql::PipelineGroup g = group;
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> sums;
    for (const auto& a : group.aggs)
        if (a.fn == duql::AggFn::Sum) {
            dstd::String valid = "__duql_valid_" + dstd::to_string(sums.size());
            duql::PipelineAgg hidden;
            hidden.name = valid;
            hidden.fn = duql::AggFn::Mean;
            hidden.arg = a.arg;
            hidden.text = dstd::fmt<"mean({})">(duql::term_text(*a.arg));
            g.aggs.push_back(std::move(hidden));
            sums.emplace_back(a.name, std::move(valid));
        }
    auto reason = trace_refusal(g, bucket);
    if (!reason) return reason.error();
    const dstd::String why = std::move(*reason);
    if (!why.empty()) {
        for (const auto& a : g.aggs)
            if (duql::is_occupancy(a.fn)) {
                if (bucket && bucket->every)
                    return refuse<"'{}' does not run with a hopping bucket">(
                        agg_name(a.fn));
                return refuse<
                    "'{}' needs the trace aggregation, which this group "
                    "cannot take: {}">(agg_name(a.fn), why);
            }
        leave_scan("group");
    }
    dstd::Vector<dstd::Optional<dataframe::TypeId>> key_types;
    if (bucket) {
        auto type = term_type(bucket->key, dstd::String(bucket->text));
        if (!type) return type.error();
        key_types.push_back(type->value_or(dataframe::TypeId::Int64));
    }
    for (const auto& k : g.keys) {
        auto type = term_type(k.term, dstd::String(k.text));
        if (!type) return type.error();
        key_types.push_back(*type);
    }
    dstd::Vector<dstd::Optional<dataframe::TypeId>> agg_types;
    for (const auto& a : g.aggs) {
        auto type = term_type(a.arg, dstd::String(a.text));
        if (!type) return type.error();
        agg_types.push_back(agg_type(a, *type));
    }

    dstd::Vector<dstd::String> keys;
    if (bucket) keys.push_back(dstd::String(bucket->name));
    for (const auto& k : g.keys) keys.push_back(dstd::String(k.name));
    dstd::Vector<dstd::String> out = keys;
    for (const auto& a : group.aggs) out.push_back(dstd::String(a.name));
    const dstd::Vector<dstd::String> visible = out;
    for (std::size_t i = 0; i < out.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (out[i] == out[j])
                return refuse<"group gives two columns named '{}'">(out[i]);

    dstd::String text;
    for (const auto& k : g.keys) {
        if (!text.empty()) text += ", ";
        text += k.text;
    }
    for (const auto& [sum, valid] : sums) out.push_back(valid);
    dstd::String aggs;
    for (const auto& a : group.aggs) {
        if (!aggs.empty()) aggs += ", ";
        aggs += dstd::fmt<"{} = {}">(a.name, a.text);
    }
    Status grouped;
    if (why.empty()) {
        lines.push_back(
            dstd::fmt<"group (trace plan): keys {}; aggs {}">(text, aggs));
        grouped = trace_group(g, bucket);
    } else {
        lines.push_back(
            dstd::fmt<"group (frame plan): keys {}; aggs {}">(text, aggs));
        grouped = frame_group(g, bucket);
    }
    if (!grouped) return grouped;
    rename(out);
    cols_.clear();
    for (const auto& f : v_.output_schema().fields)
        cols_.push_back({dstd::String(f.name), f.type, false});
    for (std::size_t i = 0; i < out.size(); ++i) {
        const dstd::Optional<dataframe::TypeId> want =
            i < key_types.size() ? key_types[i]
                                 : agg_types[i - key_types.size()];
        if (!want || cols_[i].type.id == *want ||
            (*want == dataframe::TypeId::String &&
             cols_[i].type.id != dataframe::TypeId::Unknown))
            continue;
        dataframe::Expr e = col(out[i]);
        const bool is_text = cols_[i].type.id == dataframe::TypeId::String;
        if ((is_text && is_number(*want)) ||
            (cols_[i].type.id == dataframe::TypeId::Uint64 &&
             *want == dataframe::TypeId::Int64))
            e = dataframe::expr_convert(is_integer(*want)
                                            ? dataframe::ConvertOp::Int
                                            : dataframe::ConvertOp::Float,
                                        e);
        v_ = v_.with_column(std_str(out[i]), dataframe::expr_cast(*want, e));
        cols_[i].type = dataframe::scalar(*want);
    }
    for (const auto& [sum, valid] : sums)
        v_ = v_.with_column(
            std_str(sum),
            dataframe::expr_arith(
                dataframe::ArithOp::Mul, col(sum),
                dataframe::expr_select(
                    dataframe::expr_is_null(col(valid), false),
                    dataframe::expr_lit(std::int64_t{1}),
                    dataframe::expr_lit_null(dataframe::TypeId::Int64))));
    for (const auto& a : group.aggs)
        if (a.fn == duql::AggFn::Quantile)
            v_ = v_.with_column(
                std_str(dstd::String(a.name)),
                dataframe::expr_select(
                    dataframe::expr_unary(dataframe::UnaryOp::IsNan,
                                          col(a.name)),
                    dataframe::expr_lit_null(dataframe::TypeId::Float64),
                    col(a.name)));
    if (out != visible)
        if (auto done = keep(visible, false); !done) return done;
    if (!keys.empty()) {
        lines.push_back(dstd::fmt<"sort_by_multi {}">(joined(keys)));
        v_ = v_.sort_by_multi(std_strs(keys),
                              dstd::Vector<bool>(keys.size(), false));
    }
    if (bucket && bucket->fill)
        finish_fill(g, *bucket);
    else if (keys.empty())
        finish_empty(g);
    extras_.clear();
    return {};
}

Status Applier::trace_group(
    const duql::PipelineGroup& g,
    const dstd::Optional<duql::PipelineBucket>& bucket) {
    // A select before it would project the aggregation's output; the
    // keys and arguments are among the selected columns already.
    View v = v_.select(dstd::Vector<dstd::String>{});
    dstd::Vector<dstd::String> keep_cols;
    if (bucket) {
        const auto width = static_cast<std::uint64_t>(
            std::llround(bucket->width * us_per_time_unit()));
        const auto origin = static_cast<std::uint64_t>(
            std::llround(bucket->origin * us_per_time_unit()));
        if (origin == 0) {
            lines.push_back(dstd::fmt<"time_bucket {} us">(width));
            v = v.time_bucket(width);
        } else {
            lines.push_back(
                dstd::fmt<"time_bucket {} us at {} us">(width, origin));
            v = v.time_bucket(width, origin);
        }
        keep_cols.push_back("time_bucket");
    }
    dstd::Vector<GroupKey> keys;
    for (const auto& k : g.keys) {
        keys.push_back(
            GroupKey::field(dstd::String(whole_field(k.term)->base)));
        keep_cols.push_back(dstd::String(whole_field(k.term)->base));
    }
    dstd::Vector<AggSpec> specs;
    for (std::size_t i = 0; i < g.aggs.size(); ++i) {
        const auto& a = g.aggs[i];
        dstd::String out = "__duql_agg_" + dstd::to_string(i);
        specs.emplace_back(
            *trace_op(a),
            a.arg ? dstd::String(whole_field(a.arg)->base) : dstd::String(),
            out, "", a.q);
        keep_cols.push_back(std::move(out));
    }
    if (!keys.empty()) v = v.group_by(std::move(keys));
    v_ = v.agg(std::move(specs));
    scan_ = false;
    blocker_ = "group";
    columns_from_view();
    if (auto done = keep(std::move(keep_cols), false); !done) return done;
    // busy() and active() come back in microseconds; the duration role
    // may count in another unit.
    const auto dur_ns = roles_.fields.find(roles_.duration);
    const double us_per_dur =
        dur_ns == roles_.fields.end()
            ? 1.0
            : static_cast<double>(dur_ns->second) / 1000.0;
    for (std::size_t i = 0; i < g.aggs.size() && us_per_dur != 1.0; ++i) {
        if (g.aggs[i].fn != duql::AggFn::Busy &&
            g.aggs[i].fn != duql::AggFn::Active)
            continue;
        const dstd::String out = "__duql_agg_" + dstd::to_string(i);
        if (auto done = with_column(
                out,
                dataframe::expr_binary(
                    dataframe::BinaryOp::Div,
                    dataframe::expr_col(static_cast<std::int32_t>(*index(out))),
                    dataframe::expr_lit(us_per_dur)),
                dstd::fmt<"{} in duration units">(out));
            !done)
            return done;
    }
    // The trace aggregation keys buckets in microseconds; the time role
    // may count in another unit.
    if (bucket && us_per_time_unit() != 1.0) {
        const auto i = index("time_bucket");
        dataframe::Expr in_unit = dataframe::expr_binary(
            dataframe::BinaryOp::Div,
            dataframe::expr_col(static_cast<std::int32_t>(*i)),
            dataframe::expr_lit(us_per_time_unit()));
        const auto* field = schema_.field_at(roles_.time);
        if (field && field->type == trace::index::FieldType::Int)
            in_unit = dataframe::expr_cast(dataframe::TypeId::Int64, in_unit);
        return with_column("time_bucket", in_unit, "time_bucket in time units");
    }
    return {};
}

dstd::Result<dstd::Vector<double>> Applier::plugin_params(
    const duql::PipelineAgg& a) {
    const dftu_op_desc* op = dftu_op_find(a.plugin.c_str());
    if (!op) return refuse<"plugin function '{}' is not registered">(a.plugin);
    if (!a.arg)
        return plugin_refusal(dstd::String(a.plugin), op,
                              "the column is missing");
    std::size_t takes = 0;
    for (std::uint32_t i = 1; i < DFTU_OP_MAX_ARGS; ++i)
        if (DFTU_OP_SIG_ARG(op->sig, i) != DFTU_TOK_NONE) ++takes;
    if (a.params.size() != takes)
        return plugin_refusal(dstd::String(a.plugin), op,
                              "the arguments do not fit");
    dstd::Vector<double> out;
    for (std::size_t i = 0; i < takes; ++i) {
        const dftu_op_tok tok =
            DFTU_OP_SIG_ARG(op->sig, static_cast<std::uint32_t>(i + 1));
        const dftu_scalar v = plugin_scalar(a.params[i]);
        const bool whole = v.kind != DFTU_SCALAR_TAG_F64;
        const bool real = tok == DFTU_TOK_F64 || tok == DFTU_TOK_SCALAR;
        const bool count = tok == DFTU_TOK_I32 || tok == DFTU_TOK_REDUCE ||
                           tok == DFTU_TOK_BOOL;
        if (v.kind == DFTU_SCALAR_TAG_STR || !(real || (count && whole)))
            return plugin_refusal(dstd::String(a.plugin), op,
                                  "the arguments do not fit");
        out.push_back(v.kind == DFTU_SCALAR_TAG_F64 ? v.value.d
                      : v.kind == DFTU_SCALAR_TAG_U64
                          ? static_cast<double>(v.value.u)
                          : static_cast<double>(v.value.i));
    }
    return out;
}

dataframe::DataType Applier::plugin_type(const dstd::String& name) {
    switch (DFTU_OP_SIG_RET(dftu_op_find(name.c_str())->sig)) {
        case DFTU_TOK_I64:
            return dataframe::scalar(dataframe::TypeId::Int64);
        case DFTU_TOK_BOOL:
            return dataframe::scalar(dataframe::TypeId::Bool);
        default:
            return dataframe::scalar(dataframe::TypeId::Float64);
    }
}

Status Applier::hop_rows(const duql::PipelineBucket& b) {
    const dstd::String time = "__duql_time";
    auto compiled = compile(column_item(dstd::String(roles_.time)).term,
                            dstd::String(roles_.time));
    if (!compiled) return compiled.error();
    if (auto done = with_column(time, *compiled, dstd::String(roles_.time));
        !done)
        return done;
    const std::size_t at = *index(time);
    const dataframe::TypeId tid = cols_[at].type.id;
    const auto whole = [](double x) { return std::trunc(x) == x; };
    const double step = *b.every;
    const bool exact =
        (tid == dataframe::TypeId::Int64 || tid == dataframe::TypeId::Uint64) &&
        whole(b.width) && whole(step) && whole(b.origin);
    const dataframe::TypeId out_type =
        exact ? dataframe::TypeId::Int64 : dataframe::TypeId::Float64;
    const double width = b.width;
    const double origin = b.origin;
    dstd::Vector<dataframe::query::VectorColumn> next = cols_;
    next.push_back({"__duql_bucket", dataframe::scalar(out_type)});
    const auto windows =
        static_cast<std::size_t>(std::max(1.0, std::ceil(width / step)));
    map_rows(
        [at, exact, width, step, origin, windows](
            dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            const std::int64_t n = f.num_rows();
            dstd::Vector<std::int64_t> parent;
            parent.reserve(static_cast<std::size_t>(n) * windows);
            dstd::Vector<std::int64_t> starts_i;
            dstd::Vector<double> starts_f;
            if (exact) {
                const dataframe::Series t =
                    f.columns[at].cast(dataframe::TypeId::Int64).materialize();
                const std::int64_t* v = t.data<std::int64_t>();
                const auto w = static_cast<std::int64_t>(width);
                const auto e = static_cast<std::int64_t>(step);
                const auto o = static_cast<std::int64_t>(origin);
                starts_i.reserve(parent.capacity());
                for (std::int64_t r = 0; r < n; ++r) {
                    if (t.is_null(r)) continue;
                    const std::int64_t u = v[r] - o;
                    const std::int64_t hi = floor_div(u, e);
                    for (std::int64_t k = floor_div(u - w, e) + 1; k <= hi;
                         ++k) {
                        parent.push_back(r);
                        starts_i.push_back(o + k * e);
                    }
                }
            } else {
                const dataframe::Series t =
                    f.columns[at]
                        .cast(dataframe::TypeId::Float64)
                        .materialize();
                const double* v = t.data<double>();
                starts_f.reserve(parent.capacity());
                for (std::int64_t r = 0; r < n; ++r) {
                    if (t.is_null(r)) continue;
                    const double u = v[r] - origin;
                    const auto hi =
                        static_cast<std::int64_t>(std::floor(u / step));
                    for (auto k = static_cast<std::int64_t>(
                                      std::floor((u - width) / step)) +
                                  1;
                         k <= hi; ++k) {
                        parent.push_back(r);
                        starts_f.push_back(origin +
                                           static_cast<double>(k) * step);
                    }
                }
            }
            const auto m = static_cast<std::int64_t>(parent.size());
            dataframe::DataFrame out;
            for (std::size_t j = 0; j < f.columns.size(); ++j) {
                out.names.emplace_back(f.names[j]);
                out.columns.push_back(f.columns[j].take(parent));
            }
            out.names.emplace_back("__duql_bucket");
            out.columns.push_back(
                exact ? dataframe::Series::flat_i64(starts_i.data(), m)
                      : dataframe::Series::flat_f64(starts_f.data(), m));
            return out;
        },
        std::move(next),
        dstd::fmt<"hop buckets {} every {}{}">(
            number_text(width), number_text(step),
            origin != 0 ? dstd::fmt<" at {}">(number_text(origin))
                        : dstd::String()));
    return {};
}

Status Applier::frame_group(
    const duql::PipelineGroup& g,
    const dstd::Optional<duql::PipelineBucket>& bucket) {
    dstd::Vector<dstd::String> keys;
    if (bucket) {
        if (bucket->every) {
            if (auto done = hop_rows(*bucket); !done) return done;
        } else {
            auto compiled = compile(bucket->key, dstd::String(bucket->text));
            if (!compiled) return compiled.error();
            if (auto done = with_column("__duql_bucket", *compiled,
                                        dstd::String(bucket->text));
                !done)
                return done;
        }
        keys.push_back("__duql_bucket");
    }
    for (std::size_t k = 0; k < g.keys.size(); ++k) {
        auto column = source(g.keys[k], dstd::fmt<"__duql_key_{}">(k));
        if (!column) return column.error();
        keys.push_back(std::move(*column));
    }
    dstd::Vector<dataframe::GroupAgg> specs;
    dstd::Vector<dataframe::query::FoldAgg> fold(g.aggs.size());
    dstd::Vector<dstd::String> by(g.aggs.size());
    bool folded = false;
    for (std::size_t i = 0; i < g.aggs.size(); ++i) {
        const auto& a = g.aggs[i];
        const dstd::String out = "__duql_agg_" + dstd::to_string(i);
        dstd::String in;
        if (a.by) {
            auto column = source({"", a.by, duql::term_text(*a.by)},
                                 dstd::fmt<"__duql_by_{}">(i));
            if (!column) return column.error();
            by[i] = std::move(*column);
        }
        if (a.fn == duql::AggFn::CountIf) {
            in = dstd::fmt<"__duql_in_{}">(i);
            auto condition = compile(a.arg, dstd::String(a.text), true);
            if (!condition) return condition.error();
            if (auto done = with_column(
                    in,
                    dataframe::expr_select(
                        *condition, dataframe::expr_lit(std::int64_t{1}),
                        dataframe::expr_lit_null(dataframe::TypeId::Int64)),
                    dstd::String(a.text));
                !done)
                return done;
        } else if (a.arg) {
            auto column =
                source({"", a.arg, a.text}, dstd::fmt<"__duql_in_{}">(i));
            if (!column) return column.error();
            in = std::move(*column);
        }
        dataframe::GroupAgg spec{dataframe::Agg::Count, std_str(in),
                                 std_str(out)};
        switch (a.fn) {
            case duql::AggFn::Count:
                spec.op = dataframe::Agg::CountValid;
                if (!a.arg) {
                    // A key-less Count with no input gives null.
                    spec.column = "__duql_one";
                    if (!index(spec.column))
                        if (auto done = with_column(
                                dstd::String(spec.column),
                                dataframe::expr_lit(std::int64_t{1}), "1");
                            !done)
                            return done;
                }
                break;
            case duql::AggFn::CountIf:
                spec.op = dataframe::Agg::CountValid;
                break;
            case duql::AggFn::Sum:
                spec.op = dataframe::Agg::Sum;
                break;
            case duql::AggFn::Min:
                spec.op = dataframe::Agg::Min;
                break;
            case duql::AggFn::Max:
                spec.op = dataframe::Agg::Max;
                break;
            case duql::AggFn::Mean:
                spec.op = dataframe::Agg::Mean;
                break;
            case duql::AggFn::Var:
                spec.op = dataframe::Agg::Var;
                break;
            case duql::AggFn::Std:
                spec.op = dataframe::Agg::Std;
                break;
            case duql::AggFn::First:
                spec.op = dataframe::Agg::First;
                break;
            case duql::AggFn::Last:
                spec.op = dataframe::Agg::Last;
                break;
            case duql::AggFn::Quantile:
                spec.op = dataframe::Agg::Pct;
                spec.param = a.q;
                if (a.merged)
                    fold[i].op = dataframe::query::FoldOp::MergeQuantile;
                break;
            case duql::AggFn::Histogram:
                spec.op = dataframe::Agg::Hist;
                break;
            case duql::AggFn::Busy:
            case duql::AggFn::Concurrency:
            case duql::AggFn::Utilization:
            case duql::AggFn::Active:
                return refuse<"'{}' needs the trace aggregation">(
                    agg_name(a.fn));
            case duql::AggFn::CountDistinct:
                fold[i].op = dataframe::query::FoldOp::CountDistinct;
                break;
            case duql::AggFn::Collect:
                fold[i].op = dataframe::query::FoldOp::Collect;
                break;
            case duql::AggFn::Argmax:
                fold[i].op = dataframe::query::FoldOp::Argmax;
                break;
            case duql::AggFn::Argmin:
                fold[i].op = dataframe::query::FoldOp::Argmin;
                break;
            case duql::AggFn::Sketch:
                fold[i].op = dataframe::query::FoldOp::Sketch;
                break;
            case duql::AggFn::Merge:
                fold[i].op = dataframe::query::FoldOp::Merge;
                break;
            case duql::AggFn::Plugin:
                fold[i].op = dataframe::query::FoldOp::Plugin;
                fold[i].plugin = a.plugin;
                {
                    auto params = plugin_params(a);
                    if (!params) return params.error();
                    fold[i].params = std::move(*params);
                }
                break;
        }
        folded = folded || fold[i].op != dataframe::query::FoldOp::Engine;
        fold[i].engine = dataframe::to_agg_op(spec.op);
        fold[i].param = spec.param;
        specs.push_back(std::move(spec));
    }
    if (!folded) {
        v_ = v_.group_by(std_strs(keys), specs);
        columns_from_view();
        return {};
    }
    fold_group(keys, specs, by, std::move(fold));
    return {};
}

void Applier::fold_group(const dstd::Vector<dstd::String>& keys,
                         const dstd::Vector<dataframe::GroupAgg>& specs,
                         const dstd::Vector<dstd::String>& by,
                         dstd::Vector<dataframe::query::FoldAgg> fold) {
    dstd::Vector<dstd::String> read = keys;
    auto at = [&](dstd::StringView name) {
        const auto it = std::find(read.begin(), read.end(), name);
        if (it != read.end())
            return static_cast<std::int32_t>(it - read.begin());
        read.emplace_back(name);
        return static_cast<std::int32_t>(read.size() - 1);
    };
    for (std::size_t i = 0; i < fold.size(); ++i) {
        if (!specs[i].column.empty()) fold[i].in = at(specs[i].column);
        if (!by[i].empty()) fold[i].by = at(by[i]);
    }
    dataframe::Schema schema;
    dstd::Vector<dataframe::DataType> key_types;
    for (const auto& k : keys) {
        key_types.push_back(type_of(k));
        schema.fields.push_back(dataframe::Field{std_str(k), type_of(k), true});
    }
    dstd::Vector<dstd::Pair<dstd::String, dataframe::TypeId>> in;
    for (const auto& n : read) in.emplace_back(n, type_of(n).id);
    for (std::size_t i = 0; i < fold.size(); ++i) {
        auto& f = fold[i];
        const dataframe::DataType t =
            f.in < 0 ? dataframe::scalar(dataframe::TypeId::Int64)
                     : type_of(read[static_cast<std::size_t>(f.in)]);
        switch (f.op) {
            case dataframe::query::FoldOp::Engine:
                f.type = dataframe::agg_output_type(f.engine, t.id);
                break;
            case dataframe::query::FoldOp::CountDistinct:
                f.type = dataframe::scalar(dataframe::TypeId::Int64);
                break;
            case dataframe::query::FoldOp::Collect:
                f.type = dataframe::list_of(t);
                break;
            case dataframe::query::FoldOp::Argmax:
            case dataframe::query::FoldOp::Argmin:
                f.type = t;
                break;
            case dataframe::query::FoldOp::Plugin:
                f.type = plugin_type(dstd::String(f.plugin));
                break;
            case dataframe::query::FoldOp::Sketch:
            case dataframe::query::FoldOp::Merge:
                f.type = dataframe::scalar(dataframe::TypeId::String);
                break;
            case dataframe::query::FoldOp::MergeQuantile:
                f.type = dataframe::scalar(dataframe::TypeId::Float64);
                break;
        }
        schema.fields.push_back(
            dataframe::Field{dstd::fmt<"__duql_agg_{}">(i), f.type, true});
    }
    lines.push_back(dstd::fmt<"group fold over {} (one pass, in input order)">(
        joined(read)));
    v_ = v_.with_lazy(fold_source(v_.lazy().select(std_strs(read)),
                                  std::move(schema), std::move(in),
                                  std::move(key_types), std::move(fold)));
    columns_from_view();
}

void Applier::finish(FrameFn fn, const dstd::String& line,
                     dstd::Optional<dataframe::Schema> schema) {
    lines.push_back(line);
    if (!schema) schema = v_.output_schema();
    v_ = v_.with_lazy(
        finish_source(v_.lazy(), std::move(*schema), std::move(fn)));
}

void Applier::finish_empty(const duql::PipelineGroup& g) {
    dstd::Vector<dstd::String> counts;
    for (const auto& a : g.aggs)
        if (is_count(a.fn)) counts.push_back(dstd::String(a.name));
    dstd::Vector<dstd::Pair<dstd::String, dataframe::DataType>> cols;
    for (const auto& c : cols_) cols.emplace_back(c.name, c.type);
    finish(
        [counts,
         cols](dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            if (f.num_rows() > 0) return f;
            dataframe::DataFrame row;
            const std::int64_t zero = 0;
            for (const auto& [name, type] : cols) {
                row.names.emplace_back(name);
                if (std::find(counts.begin(), counts.end(), name) !=
                    counts.end())
                    row.columns.push_back(
                        dataframe::Series::flat_i64(&zero, 1));
                else
                    row.columns.push_back(null_cell(type));
            }
            return row;
        },
        "one row when empty");
}

namespace {

struct FillSpec {
    dstd::Vector<dstd::String> counts;
    std::size_t n_keys;
    double width;
    double origin;
    std::uint64_t limit;
    duql::syntax::FillMode mode;
    dstd::Optional<double> range_lo;
    dstd::Optional<double> range_hi;
};

dstd::Vector<dataframe::Series> fill_key_texts(const dataframe::DataFrame& fr,
                                               std::size_t n_keys) {
    dstd::Vector<dataframe::Series> text;
    for (std::size_t c = 1; c <= n_keys; ++c)
        text.push_back(fr.columns[c].type() == dataframe::TypeId::String
                           ? fr.columns[c].share()
                           : fr.columns[c].cast(dataframe::TypeId::String));
    return text;
}

dstd::String fill_key_in(const dstd::Vector<dataframe::Series>& text,
                         std::int64_t r) {
    dstd::String k;
    for (const dataframe::Series& s : text) {
        if (s.is_null(r)) {
            k += '\x01';
        } else {
            k += '\x02';
            k += s.string_at(r);
        }
        k += '\0';
    }
    return k;
}

// False when `f` needs no added row.
dstd::Result<bool> fill_missing_rows(const FillSpec& spec,
                                     const dataframe::DataFrame& f,
                                     dataframe::DataFrame& out) {
    const auto& n_keys = spec.n_keys;
    const auto& width = spec.width;
    const auto& origin = spec.origin;
    const auto& limit = spec.limit;
    const auto& mode = spec.mode;
    const auto& counts = spec.counts;
    const auto& range_lo = spec.range_lo;
    const auto& range_hi = spec.range_hi;
    const std::int64_t n = f.num_rows();
    if (n == 0) return false;
    const dataframe::Series bucket =
        f.columns[0].cast(dataframe::TypeId::Float64);
    const double* at = bucket.data<double>();
    double lo = std::numeric_limits<double>::infinity();
    double hi = -lo;
    for (std::int64_t r = 0; r < n; ++r) {
        if (bucket.is_null(r)) continue;
        lo = std::min(lo, at[r]);
        hi = std::max(hi, at[r]);
    }
    if (range_lo) {
        lo = origin + std::floor((*range_lo - origin) / width) * width;
        hi = origin + std::floor((*range_hi - origin) / width) * width;
    }
    if (lo > hi) return false;
    const auto steps =
        static_cast<std::uint64_t>(std::llround((hi - lo) / width)) + 1;
    dstd::Map<dstd::String, std::int64_t> groups;
    dstd::Vector<std::int64_t> reps;
    dstd::Map<dstd::Pair<dstd::String, std::int64_t>, bool> present;
    const dstd::Vector<dataframe::Series> text = fill_key_texts(f, n_keys);
    auto key_of = [&](std::int64_t r) { return fill_key_in(text, r); };
    for (std::int64_t r = 0; r < n; ++r) {
        dstd::String k = key_of(r);
        if (groups.emplace(k, static_cast<std::int64_t>(reps.size())).second)
            reps.push_back(r);
        if (!bucket.is_null(r))
            present[{k, std::llround((at[r] - lo) / width)}] = true;
    }
    if (steps * reps.size() > limit)
        return refuse<
            "'bucket ... fill' would make {} buckets x {} groups, over "
            "DUQL_FILL_MAX_ROWS ({})">(steps, reps.size(), limit);
    dstd::Vector<std::int64_t> take;
    dstd::Vector<double> starts;
    for (std::uint64_t s = 0; s < steps; ++s)
        for (const auto& [k, at_rep] : groups)
            if (!present.count({k, static_cast<std::int64_t>(s)})) {
                take.push_back(reps[static_cast<std::size_t>(at_rep)]);
                starts.push_back(lo + static_cast<double>(s) * width);
            }
    if (take.empty()) return false;
    dstd::Vector<dstd::String> key_names(f.names.begin(),
                                         f.names.begin() + 1 + n_keys);
    auto key_columns = f.select(std_strs(key_names));
    if (!key_columns) return key_columns.error();
    dataframe::DataFrame add = key_columns->take(take);
    const auto m = static_cast<std::int64_t>(take.size());
    add.columns[0] =
        dataframe::Series::flat_f64(starts.data(), m).cast(f.columns[0].type());
    const dstd::Vector<std::int64_t> zeros(static_cast<std::size_t>(m), 0);
    for (const auto& c : counts) {
        add.names.emplace_back(c);
        add.columns.push_back(dataframe::Series::flat_i64(zeros.data(), m));
    }
    const bool carry = mode != duql::syntax::FillMode::Zero;
    dataframe::DataFrame flagged;
    const dataframe::DataFrame* base = &f;
    if (carry) {
        const dstd::Vector<std::int64_t> none(static_cast<std::size_t>(n), 0);
        const dstd::Vector<std::int64_t> ones(static_cast<std::size_t>(m), 1);
        flagged = f.with_column("__duql_added",
                                dataframe::Series::flat_i64(none.data(), n));
        add.names.emplace_back("__duql_added");
        add.columns.push_back(dataframe::Series::flat_i64(ones.data(), m));
    }
    auto joined_rows =
        dataframe::concat({base, &add}, dataframe::ConcatHow::Diagonal);
    if (!joined_rows) return joined_rows.error();
    out = std::move(*joined_rows);
    dstd::Vector<dstd::String> keep = strings_of(f.names);
    if (carry) keep.push_back("__duql_added");
    auto kept = out.select(std_strs(keep));
    if (!kept) return kept.error();
    auto ordered = kept->sort_by_multi(
        std_strs(key_names), dstd::Vector<bool>(key_names.size(), false));
    if (!ordered) return ordered.error();
    out = std::move(*ordered);
    return true;
}

void fill_carry(const FillSpec& spec, const dataframe::DataFrame& f,
                dataframe::DataFrame& out) {
    const auto& n_keys = spec.n_keys;
    const auto& mode = spec.mode;
    const auto& counts = spec.counts;
    const std::int64_t total = out.num_rows();
    const dataframe::Series added_col = out.columns.back().materialize();
    const std::int64_t* added = added_col.data<std::int64_t>();
    const dataframe::Series pos =
        out.columns[0].cast(dataframe::TypeId::Float64);
    const double* position = pos.data<double>();
    const dstd::Vector<dataframe::Series> sorted_text =
        fill_key_texts(out, n_keys);
    dstd::Map<dstd::String, dstd::Vector<std::int64_t>> members;
    for (std::int64_t r = 0; r < total; ++r)
        if (!pos.is_null(r)) members[fill_key_in(sorted_text, r)].push_back(r);
    const bool linear = mode == duql::syntax::FillMode::Linear;
    for (std::size_t c = 1 + n_keys; c < f.names.size(); ++c) {
        if (std::find(counts.begin(), counts.end(),
                      dstd::StringView(f.names[c])) != counts.end())
            continue;
        const dataframe::Series& col = out.columns[c];
        const bool numeric = dataframe::is_numeric_dispatchable(col.type());
        if (linear && !numeric) continue;
        if (!linear) {
            dstd::Vector<std::int64_t> pick(static_cast<std::size_t>(total));
            for (std::int64_t r = 0; r < total; ++r) pick[r] = r;
            for (const auto& [k, rows] : members) {
                std::int64_t last = -1;
                for (const std::int64_t r : rows) {
                    if (added[r] == 0) {
                        if (!col.is_null(r)) last = r;
                    } else if (last >= 0) {
                        pick[r] = last;
                    }
                }
            }
            out.columns[c] = col.take(pick);
            continue;
        }
        const dataframe::Series num = col.cast(dataframe::TypeId::Float64);
        const double* v = num.data<double>();
        dstd::Vector<double> value(v, v + total);
        dstd::Vector<std::uint8_t> valid(
            static_cast<std::size_t>((total + 7) / 8), 0);
        for (std::int64_t r = 0; r < total; ++r)
            if (!col.is_null(r))
                valid[static_cast<std::size_t>(r / 8)] |=
                    static_cast<std::uint8_t>(1U << (r % 8));
        for (const auto& [k, rows] : members) {
            dstd::Vector<std::int64_t> before(rows.size(), -1);
            std::int64_t last = -1;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                before[i] = last;
                const std::int64_t r = rows[i];
                if (added[r] == 0 && !col.is_null(r)) last = r;
            }
            last = -1;
            for (std::size_t i = rows.size(); i-- > 0;) {
                const std::int64_t r = rows[i];
                if (added[r] == 0) {
                    if (!col.is_null(r)) last = r;
                } else if (last >= 0 && before[i] >= 0) {
                    const std::int64_t p = before[i];
                    const double t = (position[r] - position[p]) /
                                     (position[last] - position[p]);
                    value[static_cast<std::size_t>(r)] =
                        v[p] + (v[last] - v[p]) * t;
                    valid[static_cast<std::size_t>(r / 8)] |=
                        static_cast<std::uint8_t>(1U << (r % 8));
                }
            }
        }
        out.columns[c] =
            dataframe::Series::flat(dataframe::TypeId::Float64, value.data(),
                                    total, valid.data())
                .cast(col.type());
    }
}

}  // namespace

void Applier::finish_fill(const duql::PipelineGroup& g,
                          const duql::PipelineBucket& b) {
    FillSpec spec;
    for (const auto& a : g.aggs)
        if (is_count(a.fn)) spec.counts.push_back(dstd::String(a.name));
    spec.n_keys = g.keys.size();
    spec.width = b.every.value_or(b.width);
    spec.origin = b.origin;
    spec.limit = fill_max_rows();
    spec.mode = b.fill_mode;
    spec.range_lo = b.low;
    spec.range_hi = b.high;
    const double width = spec.width;
    const double origin = spec.origin;
    const duql::syntax::FillMode mode = spec.mode;
    finish(
        [spec = std::move(spec)](
            dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            dataframe::DataFrame out;
            auto added = fill_missing_rows(spec, f, out);
            if (!added.has_value()) return added.error();
            if (!*added) return f;
            if (spec.mode == duql::syntax::FillMode::Zero) return out;
            fill_carry(spec, f, out);
            return out.select(f.names);
        },
        dstd::fmt<"fill buckets {}{}{}{}">(
            number_text(width),
            origin != 0 ? dstd::fmt<" at {}">(number_text(origin))
                        : dstd::String(),
            mode == duql::syntax::FillMode::Forward  ? " forward"
            : mode == duql::syntax::FillMode::Linear ? " linear"
                                                     : "",
            b.low ? dstd::fmt<" from {} to {}">(number_text(*b.low),
                                                number_text(*b.high))
                  : dstd::String()));
}

}  // namespace dftracer::utils::trace::views::duql_detail
