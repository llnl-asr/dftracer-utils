// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dataframe/query/vectorize.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view_duql_internal.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace dftracer::utils::trace::views::duql_detail {

dftu_scalar Applier::plugin_scalar(const duql::LiteralValue& v) {
    dftu_scalar s{};
    if (const auto* i = std::get_if<std::int64_t>(&v)) {
        s.kind = DFTU_SCALAR_TAG_I64;
        s.value.i = *i;
    } else if (const auto* u = std::get_if<std::uint64_t>(&v)) {
        s.kind = DFTU_SCALAR_TAG_U64;
        s.value.u = *u;
    } else if (const auto* d = std::get_if<double>(&v)) {
        s.kind = DFTU_SCALAR_TAG_F64;
        s.value.d = *d;
    } else if (const auto* b = std::get_if<bool>(&v)) {
        s.kind = DFTU_SCALAR_TAG_I64;
        s.value.i = *b ? 1 : 0;
    }
    return s;
}

dstd::Unexpected<dstd::Error> Applier::plugin_refusal(const dstd::String& what,
                                                      const dftu_op_desc* op,
                                                      const char* why) {
    return refuse<"{} for '{}'; it takes {}">(
        dstd::StringView(why), what,
        dstd::StringView(dftu_op_signature(op->sig)));
}

dstd::Result<dataframe::DataFrame> Applier::run_column_op(
    dataframe::DataFrame f, const dstd::String& name, const dftu_op_desc* op,
    const dstd::String& second, dftu_scalar a, dftu_scalar b,
    const dstd::Optional<dstd::String>& str) {
    dftu_dataframe* in = dataframe::dataframe_handle_wrap(std::move(f));
    dftu_dataframe* out = dftu_dataframe_column_op(
        in, name.c_str(), op->name, second.empty() ? nullptr : second.c_str(),
        a, b, str ? str->c_str() : nullptr);
    dftu_dataframe_free(in);
    if (!out) return plugin_refusal(op->name, op, "the arguments do not fit");
    return dataframe::dataframe_handle_take(out);
}

Status Applier::stage(const duql::PipelinePlugin& p) {
    leave_scan("plugin function");
    const dftu_op_desc* op = dftu_op_find(p.op.c_str());
    if (!op) return refuse<"plugin function '{}' is not registered">(p.op);
    auto compiled = compile(p.arg.term, dstd::String(p.arg.text));
    if (!compiled) return compiled.error();
    if (auto done = with_column(dstd::String(p.arg.name), *compiled,
                                dstd::String(p.arg.text));
        !done)
        return done;
    dstd::String second;
    if (p.arg2) {
        auto compiled2 = compile(p.arg2->term, dstd::String(p.arg2->text));
        if (!compiled2) return compiled2.error();
        if (auto done = with_column(dstd::String(p.arg2->name), *compiled2,
                                    dstd::String(p.arg2->text));
            !done)
            return done;
        second = p.arg2->name;
    }
    std::size_t takes_scalars = 0, takes_series = 0, takes_str = 0;
    for (std::uint32_t i = 1; i < DFTU_OP_MAX_ARGS; ++i) {
        const dftu_op_tok tok = DFTU_OP_SIG_ARG(op->sig, i);
        if (tok == DFTU_TOK_NONE) break;
        if (tok == DFTU_TOK_SERIES)
            ++takes_series;
        else if (tok == DFTU_TOK_STR || tok == DFTU_TOK_CHAR)
            ++takes_str;
        else
            ++takes_scalars;
    }
    if (p.scalars.size() > takes_scalars || p.arg2.has_value() > takes_series ||
        p.str.has_value() > takes_str)
        return plugin_refusal(dstd::String(p.op), op,
                              "the arguments do not fit");
    const dftu_scalar a =
        p.scalars.size() > 0 ? plugin_scalar(p.scalars[0]) : dftu_scalar{};
    const dftu_scalar b =
        p.scalars.size() > 1 ? plugin_scalar(p.scalars[1]) : dftu_scalar{};
    const std::size_t at = *index(p.arg.name);
    dataframe::DataType type = dataframe::scalar(dataframe::TypeId::Unknown);
    bool known = cols_[at].type.id != dataframe::TypeId::Unknown;
    if (p.arg2)
        known = known &&
                cols_[*index(second)].type.id != dataframe::TypeId::Unknown;
    if (known) {
        dataframe::DataFrame probe;
        probe.names.emplace_back(dstd::String(p.arg.name));
        probe.columns.push_back(dataframe::Series::nulls(cols_[at].type.id, 0));
        if (p.arg2) {
            probe.names.emplace_back(second);
            probe.columns.push_back(
                dataframe::Series::nulls(cols_[*index(second)].type.id, 0));
        }
        const auto out =
            run_column_op(std::move(probe), dstd::String(p.arg.name), op,
                          second, a, b, p.str);
        if (!out) return out.error();
        type = dataframe::scalar(out->columns[0].type());
    }
    dstd::Vector<dataframe::query::VectorColumn> next = cols_;
    next[at].type = type;
    map_rows(
        [name = p.arg.name, op, second, a, b, str = p.str](
            dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            return run_column_op(std::move(f), dstd::String(name), op, second,
                                 a, b, str);
        },
        std::move(next), dstd::fmt<"plugin {} over {}">(p.op, p.arg.name));
    return {};
}

Status Applier::stage(const duql::PipelineCall& c) {
    leave_scan("call");
    const dftu_op_desc* op = dftu_op_find(c.op.c_str());
    if (!op) return refuse<"'call {}' is not registered">(c.op);
    const dstd::String what = dstd::fmt<"call {}">(c.op);
    dataframe::OpArgs args;
    std::size_t used = 0;
    for (std::uint32_t i = 1; i < DFTU_OP_MAX_ARGS; ++i) {
        const dftu_op_tok tok = DFTU_OP_SIG_ARG(op->sig, i);
        if (tok == DFTU_TOK_NONE) break;
        if (used >= c.args.size())
            return plugin_refusal(what, op, "too few arguments");
        const duql::LiteralValue& v = c.args[used++];
        const auto* s = std::get_if<dstd::String>(&v);
        const auto* d = std::get_if<double>(&v);
        std::int64_t n = 0;
        if (const auto* x = std::get_if<std::int64_t>(&v))
            n = *x;
        else if (const auto* u = std::get_if<std::uint64_t>(&v))
            n = static_cast<std::int64_t>(*u);
        else if (const auto* b = std::get_if<bool>(&v))
            n = *b ? 1 : 0;
        const bool integer = !s && !d;
        switch (tok) {
            case DFTU_TOK_I64:
                if (!integer)
                    return plugin_refusal(what, op, "an integer is needed");
                args.i64(i, n);
                break;
            case DFTU_TOK_I32:
            case DFTU_TOK_BOOL:
                if (!integer)
                    return plugin_refusal(what, op, "an integer is needed");
                args.i32(i, static_cast<std::int32_t>(n));
                break;
            case DFTU_TOK_F64:
                if (s) return plugin_refusal(what, op, "a number is needed");
                args.f64(i, d ? *d : static_cast<double>(n));
                break;
            case DFTU_TOK_STR:
                if (!s) return plugin_refusal(what, op, "a string is needed");
                args.str(i, *s);
                break;
            case DFTU_TOK_SCALAR:
                if (s) return plugin_refusal(what, op, "a number is needed");
                args.scalar(i, plugin_scalar(v));
                break;
            default:
                return plugin_refusal(what, op, "an argument of this kind");
        }
    }
    if (used != c.args.size())
        return plugin_refusal(what, op, "too many arguments");
    lines.push_back(dstd::fmt<"frame_op {}">(c.op));
    v_ = v_.with_lazy(v_.lazy().frame_op(std_str(c.op), args));
    cols_.clear();
    return {};
}

Status Applier::stage(const duql::PipelineSession& ss) {
    leave_scan("session");
    if (index(ss.name))
        return refuse<
            "'session' gives column '{}', which is already a column; name "
            "it with 'as'">(ss.name);
    dstd::Vector<dstd::String> keep_names = names();
    keep_names.push_back(dstd::String(ss.name));
    const dstd::String pos = "__duql_pos";
    row_index(pos);
    dstd::Vector<dstd::String> part;
    dstd::String keys_text;
    for (std::size_t k = 0; k < ss.keys.size(); ++k) {
        auto column = source(ss.keys[k], dstd::fmt<"__duql_part_{}">(k));
        if (!column) return column.error();
        part.push_back(std::move(*column));
        if (!keys_text.empty()) keys_text += ", ";
        keys_text += ss.keys[k].text;
    }
    const dstd::String time = "__duql_time";
    const dstd::String end = "__duql_end";
    auto time_expr = compile(ss.time, dstd::String(duql::term_text(*ss.time)));
    if (!time_expr) return time_expr.error();
    if (auto done = with_column(time, *time_expr,
                                dstd::String(duql::term_text(*ss.time)));
        !done)
        return done;
    if (ss.end) {
        auto end_expr = compile(ss.end, dstd::String(duql::term_text(*ss.end)));
        if (!end_expr) return end_expr.error();
        if (auto done = with_column(end, *end_expr,
                                    dstd::String(duql::term_text(*ss.end)));
            !done)
            return done;
    }
    // Time order, ties in input order.
    lines.push_back(dstd::fmt<"sort_by_multi {}, {}">(time, pos));
    v_ = v_.sort_by_multi({std_str(time), std_str(pos)},
                          dstd::Vector<bool>{false, false});
    const dstd::String ord = "__duql_ord";
    row_index(ord);

    dstd::Vector<const char*> keys;
    for (const auto& k : part) keys.push_back(k.c_str());
    const char* by = ord.c_str();
    dftu_window_spec w{};
    w.func = DFTU_WINDOW_SESSIONIZE;
    w.out = ss.name.c_str();
    w.param.session = {time.c_str(), ss.end ? end.c_str() : nullptr, ss.gap,
                       ss.span};
    dstd::Vector<dstd::String> out = names();
    out.push_back(dstd::String(ss.name));
    cols_.push_back({dstd::String(ss.name),
                     dataframe::scalar(dataframe::TypeId::Int64), false});
    const dstd::Vector<dftu_window_spec> specs{w};
    dataframe::OpArgs a;
    a.strlist(1, keys.data(), static_cast<std::int32_t>(keys.size()))
        .strlist(2, &by, 1)
        .winlist(3, specs);
    v_ = v_.with_lazy(
        v_.lazy().frame_op("dftu.frame.window", a, {}, std_strs(out)));

    lines.push_back(dstd::fmt<"sort_by_multi {}">(pos));
    v_ = v_.sort_by_multi({std_str(pos)}, dstd::Vector<bool>{false});
    lines.push_back(
        dstd::fmt<"session keys {}; time {}; gap {}; max {}; as {}">(
            keys_text, ss.text, number_text(ss.gap),
            ss.span > 0 ? number_text(ss.span) : dstd::String("none"),
            ss.name));
    return keep(std::move(keep_names));
}

Status Applier::stage(const duql::PipelineExpand& e) {
    leave_scan("expand");
    dstd::String line = dstd::fmt<"expand {} as {}">(e.path, e.name);
    if (!e.index.empty()) line += dstd::fmt<" with_index {}">(e.index);
    if (e.keep_empty) line += " keep_empty";
    const auto li = field_index(dstd::String(e.path));
    const bool list = li && cols_[*li].type.id == dataframe::TypeId::List;
    const dataframe::DataType elem =
        list && !cols_[*li].type.fields.empty()
            ? cols_[*li].type.fields.front().type
            : dataframe::scalar(dataframe::TypeId::String);
    const bool is_struct = elem.id == dataframe::TypeId::Struct;
    const bool json = list && cols_[*li].json;
    auto replaced = [&](const dstd::String& n) {
        return n == dstd::String(e.name) || n == dstd::String(e.index) ||
               (is_struct && n.starts_with(e.name + "."));
    };
    dstd::Vector<dataframe::query::VectorColumn> next;
    dstd::Vector<bool> drop;
    auto element_columns = [&] {
        if (is_struct)
            for (const auto& f : elem.fields)
                next.push_back(
                    {dstd::String(e.name + ".").append(f.name), f.type});
        else
            next.push_back({dstd::String(e.name), elem, json});
        if (!e.index.empty())
            next.push_back({dstd::String(e.index),
                            dataframe::scalar(dataframe::TypeId::Int64)});
    };
    for (std::size_t j = 0; j < cols_.size(); ++j) {
        const bool here = li && j == *li;
        drop.push_back(!here && replaced(cols_[j].name));
        if (here)
            element_columns();
        else if (!drop.back())
            next.push_back(cols_[j]);
    }
    if (!li) element_columns();
    const std::size_t at = li ? *li : cols_.size();
    const dstd::String name = dstd::String(e.name);
    const dstd::String idx = dstd::String(e.index);
    const bool keep_empty = e.keep_empty;
    map_rows(
        [at, drop, name, idx, keep_empty](
            dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            const std::int64_t n = f.num_rows();
            dataframe::Series lists;
            const std::int32_t* off = nullptr;
            if (at < f.columns.size() &&
                f.columns[at].type() == dataframe::TypeId::List) {
                lists = f.columns[at].materialize();
                off = lists.offsets();
            }
            dstd::Vector<std::int64_t> parent;
            dstd::Vector<std::int64_t> element;
            dstd::Vector<std::int64_t> position;
            auto row = [&](std::int64_t r, std::int64_t el, std::int64_t p) {
                parent.push_back(r);
                element.push_back(el);
                position.push_back(p);
            };
            for (std::int64_t r = 0; r < n; ++r) {
                if (!off || lists.is_null(r)) {
                    row(r, -1, -1);
                } else if (off[r] == off[r + 1]) {
                    if (keep_empty) row(r, -1, -1);
                } else {
                    for (std::int32_t k = off[r]; k < off[r + 1]; ++k)
                        row(r, k, k - off[r]);
                }
            }
            const auto m = static_cast<std::int64_t>(parent.size());
            dataframe::DataFrame out;
            auto elements = [&] {
                if (off && lists.child(0).type() == dataframe::TypeId::Struct) {
                    const dataframe::Series child = lists.child(0);
                    for (std::int64_t i = 0; i < child.num_children(); ++i) {
                        dstd::String joined_name(name);
                        joined_name.append(".").append(child.field_name(i));
                        out.names.push_back(std::move(joined_name));
                        out.columns.push_back(child.child(i).take(element));
                    }
                } else if (off) {
                    out.names.emplace_back(name);
                    out.columns.push_back(lists.child(0).take(element));
                } else {
                    out.names.emplace_back(name);
                    out.columns.push_back(
                        dataframe::Series::nulls(dataframe::TypeId::String, m));
                }
                if (idx.empty()) return;
                dstd::Vector<std::uint8_t> valid(
                    static_cast<std::size_t>((m + 7) / 8), 0);
                for (std::int64_t r = 0; r < m; ++r)
                    if (position[static_cast<std::size_t>(r)] >= 0)
                        valid[static_cast<std::size_t>(r >> 3)] |=
                            static_cast<std::uint8_t>(1u << (r & 7));
                out.names.emplace_back(idx);
                out.columns.push_back(dataframe::Series::flat_i64(
                    position.data(), m, valid.data()));
            };
            for (std::size_t j = 0; j < f.columns.size(); ++j) {
                if (j == at) {
                    elements();
                } else if (!drop[j]) {
                    out.names.emplace_back(f.names[j]);
                    out.columns.push_back(f.columns[j].take(parent));
                }
            }
            if (at >= f.columns.size()) elements();
            return out;
        },
        std::move(next), line);
    return {};
}

duql::PipelineItem Applier::column_item(const dstd::String& name) {
    duql::TField f;
    f.base = dstd::String(name);
    f.steps.push_back({dstd::String(name), false, dstd::nullopt});
    f.neg_at = 1;
    return {dstd::String(name),
            dstd::make_shared<const duql::Term>(duql::Term{std::move(f)}),
            dstd::String(name)};
}

Status Applier::stage(const duql::PipelinePivot& p) {
    leave_scan("pivot");
    dstd::Vector<dstd::String> read;
    auto reads = [&](const duql::TermRef& t) {
        if (!t) return;
        duql::for_each_term_field(*t, [&](const duql::TField& f) {
            if (auto i = field_index(dstd::String(f.base)))
                read.push_back(cols_[*i].name);
        });
    };
    reads(p.key.term);
    for (const auto& a : p.aggs) {
        reads(a.arg);
        reads(a.by);
    }
    duql::PipelineGroup g;
    for (const auto& c : cols_) {
        const bool skip =
            std::find(read.begin(), read.end(), c.name) != read.end() ||
            std::find(extras_.begin(), extras_.end(), c.name) !=
                extras_.end() ||
            c.name == dstd::String(p.key.name);
        if (skip) continue;
        duql::PipelineItem it = column_item(c.name);
        if (c.record && c.name.starts_with("args.")) it.name = c.name.substr(5);
        g.keys.push_back(std::move(it));
    }
    const std::size_t n_keys = g.keys.size();
    duql::PipelineItem key = p.key;
    key.name = "__duql_pivot";
    g.keys.push_back(std::move(key));
    g.aggs = p.aggs;
    dstd::Vector<dstd::String> fixed;
    for (const auto& v : p.values) fixed.push_back(pivot_name(v));
    lines.push_back(dstd::fmt<"pivot {}{}">(
        p.key.text, p.fixed ? dstd::fmt<" in [{}]">(joined(fixed))
                            : dstd::String(" (columns after the scan)")));
    if (auto done = stage(g); !done) return done;
    dstd::Vector<dstd::String> aggs;
    dstd::Vector<dataframe::query::VectorColumn> next(cols_.begin(),
                                                      cols_.begin() + n_keys);
    for (std::size_t a = 0; a < p.aggs.size(); ++a) {
        aggs.push_back(dstd::String(p.aggs[a].name));
        for (const auto& v : fixed)
            next.push_back({dstd::fmt<"{}.{}">(p.aggs[a].name, v),
                            cols_[n_keys + 1 + a].type});
    }
    dstd::Optional<dataframe::Schema> schema{dataframe::Schema{}};
    if (p.fixed)
        for (const auto& c : next)
            schema->fields.push_back(
                dataframe::Field{std_str(c.name), c.type, true});
    const bool open = !p.fixed;
    const std::uint64_t limit = pivot_max_columns();
    finish(
        [n_keys, aggs, fixed, open,
         limit](dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            return spread(std::move(f), n_keys, aggs, fixed, open, limit);
        },
        dstd::fmt<"spread __duql_pivot into {}">(joined(aggs)),
        std::move(schema));
    cols_ = p.fixed ? std::move(next)
                    : dstd::Vector<dataframe::query::VectorColumn>{};
    duql::PipelineRename labels;
    for (std::size_t i = 0; i < p.labels.size() && i < fixed.size(); ++i) {
        if (p.labels[i].empty()) continue;
        for (const auto& a : aggs)
            labels.pairs.emplace_back(aggs.size() == 1
                                          ? dstd::String(p.labels[i])
                                          : dstd::fmt<"{}_{}">(a, p.labels[i]),
                                      dstd::fmt<"{}.{}">(a, fixed[i]));
    }
    if (!labels.pairs.empty()) return stage(labels);
    return {};
}

dstd::Result<dataframe::DataFrame> Applier::spread(
    dataframe::DataFrame f, std::size_t n_keys,
    const dstd::Vector<dstd::String>& aggs, dstd::Vector<dstd::String> values,
    bool open, std::uint64_t limit) {
    const std::int64_t n = f.num_rows();
    auto text = [](const dataframe::Series& s, std::int64_t r) -> dstd::String {
        if (s.is_null(r)) return "null";
        if (s.type() == dataframe::TypeId::String)
            return dstd::String(s.string_at(r));
        if (s.type() == dataframe::TypeId::Bool)
            return s.cast(dataframe::TypeId::Int64).data<std::int64_t>()[r]
                       ? "true"
                       : "false";
        if (s.type() == dataframe::TypeId::Uint64)
            return dstd::to_string(s.materialize().data<std::uint64_t>()[r]);
        if (is_integer(s.type()))
            return dstd::to_string(s.slice(r, 1)
                                       .cast(dataframe::TypeId::Int64)
                                       .data<std::int64_t>()[0]);
        return number_text(
            s.slice(r, 1).cast(dataframe::TypeId::Float64).data<double>()[0]);
    };
    const dataframe::Series& pk = f.columns[n_keys];
    dstd::Vector<dstd::String> row_value;
    for (std::int64_t r = 0; r < n; ++r) row_value.push_back(text(pk, r));
    if (open) {
        dstd::Vector<std::int64_t> firsts;
        dstd::HashSet<dstd::String> seen;
        for (std::int64_t r = 0; r < n; ++r)
            if (seen.insert(row_value[static_cast<std::size_t>(r)]).second)
                firsts.push_back(r);
        if (firsts.size() > limit)
            return refuse<
                "'pivot' gives {} columns per aggregate, over "
                "DUQL_PIVOT_MAX_COLUMNS ({})">(firsts.size(), limit);
        const dataframe::Series order = pk.take(firsts).argsort();
        for (std::int64_t i = 0; i < order.length(); ++i)
            values.push_back(row_value[static_cast<std::size_t>(
                firsts[static_cast<std::size_t>(
                    order.data<std::int64_t>()[i])])]);
    }
    dstd::Vector<dataframe::Series> key_text;
    for (std::size_t c = 0; c < n_keys; ++c)
        key_text.push_back(f.columns[c].share());
    auto same_tuple = [&](std::int64_t a, std::int64_t b) {
        for (const auto& s : key_text)
            if (text(s, a) != text(s, b)) return false;
        return true;
    };
    dstd::Vector<std::int64_t> starts;
    dstd::Vector<dstd::Vector<std::int64_t>> cells(values.size());
    for (std::int64_t r = 0; r < n; ++r) {
        if (r == 0 || !same_tuple(r, r - 1)) {
            starts.push_back(r);
            for (auto& c : cells) c.push_back(-1);
        }
        for (std::size_t v = 0; v < values.size(); ++v)
            if (values[v] == row_value[static_cast<std::size_t>(r)])
                cells[v].back() = r;
    }
    dataframe::DataFrame out;
    for (std::size_t c = 0; c < n_keys; ++c) {
        out.names.emplace_back(f.names[c]);
        out.columns.push_back(f.columns[c].take(starts));
    }
    for (std::size_t a = 0; a < aggs.size(); ++a)
        for (std::size_t v = 0; v < values.size(); ++v) {
            out.names.emplace_back(dstd::fmt<"{}.{}">(aggs[a], values[v]));
            out.columns.push_back(f.columns[n_keys + 1 + a].take(cells[v]));
        }
    return out;
}

Status Applier::stage(const duql::PipelineUnpivot& u) {
    leave_scan("unpivot");
    dstd::Vector<std::size_t> at;
    dstd::Vector<dataframe::TypeId> types;
    for (const auto& f : u.fields) {
        const auto i = field_index(dstd::String(f));
        if (!i) return refuse<"'unpivot' has no field '{}'">(f);
        at.push_back(*i);
        types.push_back(cols_[*i].type.id);
    }
    const bool unknown = std::find(types.begin(), types.end(),
                                   dataframe::TypeId::Unknown) != types.end();
    const bool numbers = std::all_of(types.begin(), types.end(), is_number);
    const bool same =
        std::all_of(types.begin(), types.end(),
                    [&](dataframe::TypeId t) { return t == types[0]; });
    dataframe::TypeId target =
        types.empty() ? dataframe::TypeId::String : types[0];
    if (unknown) {
        target = dataframe::TypeId::String;
    } else if (!same && numbers) {
        target = std::all_of(types.begin(), types.end(), is_integer)
                     ? dataframe::TypeId::Int64
                     : dataframe::TypeId::Float64;
    } else if (!same) {
        dstd::String what;
        for (std::size_t k = 0; k < u.fields.size(); ++k) {
            if (!what.empty()) what += ", ";
            what += dstd::fmt<"{} ({})">(u.fields[k],
                                         dataframe::type_name(types[k]));
        }
        return refuse<
            "'unpivot' mixes the types of {}; convert them with int(), "
            "float() or string()">(what);
    }
    dstd::Vector<dstd::String> values;
    for (std::size_t k = 0; k < at.size(); ++k) {
        const dstd::String name = dstd::fmt<"__duql_value_{}">(k);
        dataframe::Expr e = col(cols_[at[k]].name);
        if (types[k] != target)
            e = target == dataframe::TypeId::String
                    ? dataframe::expr_convert(dataframe::ConvertOp::String, e)
                    : dataframe::expr_cast(target, e);
        if (auto done = with_column(name, e, dstd::String(u.fields[k])); !done)
            return done;
        values.push_back(name);
    }
    dstd::Vector<std::size_t> ids;
    dstd::Vector<dataframe::query::VectorColumn> next;
    for (std::size_t j = 0; j < cols_.size(); ++j) {
        const bool listed = std::find(at.begin(), at.end(), j) != at.end() ||
                            cols_[j].name.starts_with("__duql_value_");
        if (listed || cols_[j].name == dstd::String(u.key) ||
            cols_[j].name == dstd::String(u.value))
            continue;
        ids.push_back(j);
        next.push_back(cols_[j]);
    }
    next.push_back(
        {dstd::String(u.key), dataframe::scalar(dataframe::TypeId::String)});
    next.push_back({dstd::String(u.value), dataframe::scalar(target)});
    dstd::Vector<std::size_t> value_at;
    for (const auto& v : values) value_at.push_back(*index(v));
    const dstd::Vector<dstd::String> keys = strings_of(u.fields);
    const dstd::String key = dstd::String(u.key);
    const dstd::String value = dstd::String(u.value);
    map_rows(
        [ids, value_at, keys, key,
         value](dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            const std::int64_t n = f.num_rows();
            const auto k = static_cast<std::int64_t>(keys.size());
            dstd::Vector<std::int64_t> parent;
            dstd::Vector<std::int64_t> pick;
            dstd::Vector<dstd::String> names;
            for (std::int64_t r = 0; r < n; ++r)
                for (std::int64_t j = 0; j < k; ++j) {
                    parent.push_back(r);
                    pick.push_back(j * n + r);
                    names.emplace_back(keys[static_cast<std::size_t>(j)]);
                }
            dstd::Vector<dataframe::DataFrame> parts;
            for (const auto v : value_at) {
                dataframe::DataFrame one;
                one.names.emplace_back("v");
                one.columns.push_back(f.columns[v].share());
                parts.push_back(std::move(one));
            }
            dstd::Vector<const dataframe::DataFrame*> ptrs;
            for (const auto& p : parts) ptrs.push_back(&p);
            dataframe::DataFrame stacked;
            if (!ptrs.empty()) {
                auto all =
                    dataframe::concat(ptrs, dataframe::ConcatHow::Vertical);
                if (!all) return all.error();
                stacked = std::move(*all);
            }
            dataframe::DataFrame out;
            for (const auto j : ids) {
                out.names.emplace_back(f.names[j]);
                out.columns.push_back(f.columns[j].take(parent));
            }
            out.names.emplace_back(key);
            out.columns.push_back(dataframe::Series::strings(names));
            out.names.emplace_back(value);
            out.columns.push_back(stacked.columns.front().take(pick));
            return out;
        },
        std::move(next),
        dstd::fmt<"unpivot {} as {}, {}">(joined(u.fields), u.key, u.value));
    return {};
}

}  // namespace dftracer::utils::trace::views::duql_detail
