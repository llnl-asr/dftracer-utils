// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/pattern.h>
#include <dftracer/utils/duql/vectorize.h>
#include <dftracer/utils/index/cache/lookup_store.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/view_duql_internal.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace dftracer::utils::trace::views::duql_detail {

duql::PluginCatalog registry_plugins() {
    duql::PluginCatalog out;
    out.kind = [](dstd::StringView name) {
        const dftu_op_desc* op = dftu_op_find(dstd::String(name).c_str());
        if (!op) return duql::PluginKind::None;
        const dftu_op_kind k = dftu_op_kind_of(op->sig);
        if (k == DFTU_OP_KIND_SERIES &&
            DFTU_OP_SIG_ARG(op->sig, 0) == DFTU_TOK_SERIES)
            return duql::PluginKind::Column;
        if (k == DFTU_OP_KIND_FRAME &&
            DFTU_OP_SIG_ARG(op->sig, 0) == DFTU_TOK_FRAME)
            return duql::PluginKind::Table;
        if (k == DFTU_OP_KIND_AGGREGATE &&
            DFTU_OP_SIG_ARG(op->sig, 0) == DFTU_TOK_SERIES)
            return duql::PluginKind::Aggregate;
        return duql::PluginKind::Other;
    };
    for (std::uint32_t k = 0; k < dftu_op_count(); ++k) {
        const dstd::StringView op = dftu_op_at(k)->name;
        const auto dot = op.find('.');
        if (dot == dstd::StringView::npos || op.starts_with("dftu.")) continue;
        dstd::String ns(op.substr(0, dot));
        if (std::find(out.namespaces.begin(), out.namespaces.end(), ns) ==
            out.namespaces.end())
            out.namespaces.push_back(std::move(ns));
    }
    std::sort(out.namespaces.begin(), out.namespaces.end());
    return out;
}

dstd::Unexpected<dstd::Error> refuse(dstd::StringView why) {
    return common::make_error<"View::duql {}">(
        common::ErrorCode::InvalidArgument, why);
}

dstd::Vector<dstd::String> std_strs(const dstd::Vector<dstd::String>& v) {
    dstd::Vector<dstd::String> out;
    out.reserve(v.size());
    for (const auto& n : v) out.emplace_back(n);
    return out;
}

std::uint64_t pivot_max_columns() {
    return common::Env::get<std::uint64_t>("DUQL_PIVOT_MAX_COLUMNS")
        .value_or(1024);
}

std::uint64_t fill_max_rows() {
    return common::Env::get<std::uint64_t>("DUQL_FILL_MAX_ROWS")
        .value_or(10'000'000);
}

dstd::String agg_name(duql::AggFn fn) {
    switch (fn) {
        case duql::AggFn::Count:
            return "count";
        case duql::AggFn::CountIf:
            return "count_if";
        case duql::AggFn::Sum:
            return "sum";
        case duql::AggFn::Min:
            return "min";
        case duql::AggFn::Max:
            return "max";
        case duql::AggFn::Mean:
            return "mean";
        case duql::AggFn::Var:
            return "var";
        case duql::AggFn::Std:
            return "std";
        case duql::AggFn::First:
            return "first";
        case duql::AggFn::Last:
            return "last";
        case duql::AggFn::Quantile:
            return "quantile";
        case duql::AggFn::Histogram:
            return "histogram";
        case duql::AggFn::Busy:
            return "busy";
        case duql::AggFn::Concurrency:
            return "concurrency";
        case duql::AggFn::Utilization:
            return "utilization";
        case duql::AggFn::Active:
            return "active";
        case duql::AggFn::CountDistinct:
            return "count_distinct";
        case duql::AggFn::Collect:
            return "collect";
        case duql::AggFn::Argmax:
            return "arg_max";
        case duql::AggFn::Argmin:
            return "arg_min";
        case duql::AggFn::Sketch:
            return "sketch";
        case duql::AggFn::Merge:
            return "merge";
        case duql::AggFn::Plugin:
            return "plugin function";
    }
    return {};
}

dstd::Optional<AggOp> trace_op(const duql::PipelineAgg& a) {
    switch (a.fn) {
        case duql::AggFn::Count:
            if (a.arg) return dstd::nullopt;
            return AggOp::Count;
        case duql::AggFn::Sum:
            return AggOp::Sum;
        case duql::AggFn::Min:
            return AggOp::Min;
        case duql::AggFn::Max:
            return AggOp::Max;
        case duql::AggFn::Mean:
            return AggOp::Mean;
        case duql::AggFn::Var:
            return AggOp::Var;
        case duql::AggFn::Std:
            return AggOp::Std;
        case duql::AggFn::Quantile:
            if (a.merged || !(a.q > 0 && a.q < 1)) return dstd::nullopt;
            return AggOp::Pct;
        case duql::AggFn::Histogram:
            return AggOp::Hist;
        case duql::AggFn::Busy:
            return AggOp::Busy;
        case duql::AggFn::Concurrency:
            return AggOp::Concurrency;
        case duql::AggFn::Utilization:
            return AggOp::Utilization;
        case duql::AggFn::Active:
            return AggOp::Active;
        case duql::AggFn::CountIf:
        case duql::AggFn::First:
        case duql::AggFn::Last:
        case duql::AggFn::CountDistinct:
        case duql::AggFn::Collect:
        case duql::AggFn::Argmax:
        case duql::AggFn::Argmin:
        case duql::AggFn::Sketch:
        case duql::AggFn::Merge:
        case duql::AggFn::Plugin:
            return dstd::nullopt;
    }
    return dstd::nullopt;
}

const duql::TField* array_field(const duql::Term& t) {
    const auto* f = std::get_if<duql::TField>(&t.node);
    if (!f || f->root != duql::FieldRoot::Record ||
        f->neg_at != f->steps.size())
        return nullptr;
    return f;
}

dstd::String pivot_name(const duql::TConst& c) {
    return std::visit(
        [](const auto& v) -> dstd::String {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, duql::TNull>)
                return "null";
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, dstd::String>)
                return dstd::String(v);
            else if constexpr (std::is_same_v<T, double>)
                return number_text(v);
            else
                return dstd::to_string(v);
        },
        c.value);
}

Reads referenced_fields(const dstd::Vector<duql::PipelineStage>& stages,
                        const dstd::Function<dstd::Vector<dstd::String>(
                            const duql::PipelineLookup&)>& adds,
                        bool carried) {
    Reads out;
    dstd::Vector<dstd::String> defined;
    auto known = [](const dstd::Vector<dstd::String>& v, dstd::StringView n) {
        return std::find(v.begin(), v.end(), n) != v.end();
    };
    auto field = [&](dstd::StringView base, bool array) {
        if (known(defined, base) ||
            known(defined, base.substr(0, base.find_first_of(".["))))
            return;
        if (!known(out.fields, base)) out.fields.emplace_back(base);
        if (array && !known(out.arrays, base)) out.arrays.emplace_back(base);
    };
    auto add = [&](const duql::TermRef& t) {
        if (!t) return;
        duql::for_each_term(*t, [&](const duql::Term& x) {
            if (const auto* f = std::get_if<duql::TField>(&x.node);
                f && f->root != duql::FieldRoot::Element)
                field(f->base, false);
            if (const auto* q = std::get_if<duql::TQuant>(&x.node))
                if (const auto* a = array_field(*q->subject))
                    field(a->base, true);
            if (const auto* i = std::get_if<duql::TIndex>(&x.node))
                if (const auto* a = array_field(*i->array))
                    field(a->base, true);
        });
    };
    auto items = [&](const dstd::Vector<duql::PipelineItem>& v) {
        for (const auto& it : v) add(it.term);
    };
    auto define = [&](const dstd::Vector<duql::PipelineItem>& v) {
        for (const auto& it : v) defined.push_back(dstd::String(it.name));
    };
    auto sort_keys = [&](const dstd::Vector<duql::PipelineSortKey>& v) {
        for (const auto& k : v) add(k.key.term);
    };
    bool whole_rows = false;
    dstd::String bucket_name = "bucket";
    for (const auto& stage : stages)
        std::visit(
            [&](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, duql::PipelineGroup> ||
                              std::is_same_v<T, duql::PipelineSelect> ||
                              std::is_same_v<T, duql::PipelinePivot>) {
                    if (!whole_rows) out.closed = true;
                } else if constexpr (std::is_same_v<T,
                                                    duql::PipelineDistinct>) {
                    if (s.items.empty())
                        whole_rows = true;
                    else if (!whole_rows)
                        out.closed = true;
                }
                if constexpr (std::is_same_v<T, duql::PipelineWhere>) {
                    add(s.condition);
                } else if constexpr (std::is_same_v<T, duql::PipelineSelect> ||
                                     std::is_same_v<T, duql::PipelineDerive> ||
                                     std::is_same_v<T,
                                                    duql::PipelineDistinct>) {
                    items(s.items);
                    define(s.items);
                } else if constexpr (std::is_same_v<T, duql::PipelineRename>) {
                    for (const auto& pair : s.pairs) {
                        if (!carried) field(pair.second, false);
                        defined.push_back(dstd::String(pair.first));
                    }
                } else if constexpr (std::is_same_v<T, duql::PipelinePlugin>) {
                    add(s.arg.term);
                    defined.push_back(dstd::String(s.arg.name));
                    if (s.arg2) {
                        add(s.arg2->term);
                        defined.push_back(dstd::String(s.arg2->name));
                    }
                } else if constexpr (std::is_same_v<T, duql::PipelineSort>) {
                    sort_keys(s.keys);
                } else if constexpr (std::is_same_v<T, duql::PipelineTakeBy>) {
                    items(s.keys);
                    sort_keys(s.order);
                } else if constexpr (std::is_same_v<T, duql::PipelineGroup>) {
                    items(s.keys);
                    for (const auto& a : s.aggs) {
                        add(a.arg);
                        add(a.by);
                    }
                    define(s.keys);
                    for (const auto& a : s.aggs)
                        defined.push_back(dstd::String(a.name));
                    defined.push_back(bucket_name);
                } else if constexpr (std::is_same_v<T,
                                                    duql::PipelineTimeRange>) {
                    add(s.condition);
                } else if constexpr (std::is_same_v<T, duql::PipelineBucket>) {
                    add(s.key);
                    bucket_name = s.name;
                } else if constexpr (std::is_same_v<T, duql::PipelineSession>) {
                    items(s.keys);
                    add(s.time);
                    add(s.end);
                    defined.push_back(dstd::String(s.name));
                } else if constexpr (std::is_same_v<T, duql::PipelineWindow>) {
                    items(s.keys);
                    sort_keys(s.order);
                    for (const auto& c : s.calls) {
                        add(c.arg);
                        add(c.by);
                        defined.push_back(dstd::String(c.column));
                    }
                    items(s.items);
                    define(s.items);
                } else if constexpr (std::is_same_v<T, duql::PipelineExpand>) {
                    field(s.path, true);
                    defined.push_back(dstd::String(s.name));
                    if (!s.index.empty())
                        defined.push_back(dstd::String(s.index));
                } else if constexpr (std::is_same_v<T, duql::PipelinePivot>) {
                    add(s.key.term);
                    for (const auto& a : s.aggs) {
                        add(a.arg);
                        add(a.by);
                    }
                    for (const auto& a : s.aggs)
                        for (const auto& v : s.values)
                            defined.push_back(dstd::String(a.name + ".") +
                                              pivot_name(v));
                } else if constexpr (std::is_same_v<T, duql::PipelineUnpivot>) {
                    for (const auto& f : s.fields) field(f, false);
                    defined.push_back(dstd::String(s.key));
                    defined.push_back(dstd::String(s.value));
                } else if constexpr (std::is_same_v<T, duql::PipelineLookup>) {
                    for (const auto& k : s.keys) add(k.first.term);
                    if (const auto* a =
                            std::get_if<duql::PipelineAsof>(&s.mode))
                        add(a->time.term);
                    if (const auto* o =
                            std::get_if<duql::PipelineOverlap>(&s.mode)) {
                        add(o->time.term);
                        add(o->duration.term);
                    }
                    for (auto& n : adds(s)) defined.push_back(std::move(n));
                }
            },
            stage);
    return out;
}

namespace {

char type_letter(const dstd::String& type) {
    if (type == "int" || type == "uint") return 'i';
    if (type == "float") return 'f';
    if (type == "string") return 's';
    if (type == "bool") return 'b';
    if (type == "null") return 0;
    return 'j';
}

char join_letters(char a, char b) {
    if (!a) return b;
    if (!b || a == b) return a;
    if ((a == 'i' && b == 'f') || (a == 'f' && b == 'i')) return 'f';
    return 'j';
}

dataframe::DataType letter_type(char c) {
    switch (c) {
        case 'i':
            return dataframe::scalar(dataframe::TypeId::Int64);
        case 'f':
            return dataframe::scalar(dataframe::TypeId::Float64);
        case 'b':
            return dataframe::scalar(dataframe::TypeId::Bool);
        case 'a':
            return dataframe::scalar(dataframe::TypeId::Unknown);
        default:
            return dataframe::scalar(dataframe::TypeId::String);
    }
}

bool digits(dstd::StringView s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

}  // namespace

const dstd::String JSON_LIST = detail::list_token("j", "");

dstd::Pair<dstd::String, dataframe::DataType> list_type(
    const dstd::Vector<SchemaLeaf>& tree, const dstd::String& path,
    bool args_fallback) {
    char scalar = 0;
    bool scalars = false;
    bool objects = false;
    bool nested = false;
    dstd::Map<dstd::String, char> fields;
    dstd::Vector<dstd::String> prefixes = {dotted(path) + "."};
    if (args_fallback) prefixes.push_back("args." + dotted(path) + ".");
    for (const auto& leaf : tree) {
        dstd::StringView rest;
        for (const auto& p : prefixes)
            if (leaf.path.size() > p.size() && leaf.path.starts_with(p)) {
                rest = dstd::StringView(leaf.path).substr(p.size());
                break;
            }
        const std::size_t dot = rest.find('.');
        if (!digits(rest.substr(0, dot))) continue;
        const char c = type_letter(leaf.type);
        if (dot == dstd::StringView::npos) {
            scalars = true;
            scalar = join_letters(scalar, c);
            continue;
        }
        const dstd::StringView f = rest.substr(dot + 1);
        const std::size_t end = f.find('.');
        const dstd::String name(f.substr(0, end));
        if (digits(name)) {
            nested = true;
            continue;
        }
        objects = true;
        char& slot = fields[name];
        slot = end == dstd::StringView::npos ? join_letters(slot, c) : 'j';
    }
    if (objects && !scalars && !nested) {
        dstd::String spec = "{";
        dstd::Vector<dataframe::Field> members;
        for (const auto& [name, c] : fields) {
            const char k = c ? c : 's';
            if (spec.size() > 1) spec += ',';
            spec += name + "=" + k;
            members.push_back(
                dataframe::Field{dstd::String(name), letter_type(k), true});
        }
        spec += '}';
        return {detail::list_token(spec, path),
                dataframe::list_of(dataframe::struct_of(std::move(members)))};
    }
    const char k = nested || objects ? 'j'
                   : scalars         ? (scalar ? scalar : 's')
                                     : 'a';
    return {detail::list_token(dstd::String(1, k), path),
            dataframe::list_of(letter_type(k))};
}

dstd::Vector<dstd::String> scan_reads(const dstd::Vector<SchemaLeaf>& tree,
                                      const dstd::String& f, bool array,
                                      dstd::Vector<dstd::String>& arrays,
                                      bool args_fallback) {
    dstd::Vector<dstd::String> prefixes = {dotted(f) + "."};
    if (args_fallback) prefixes.push_back("args." + dotted(f) + ".");
    bool elements = false;
    dstd::Vector<dstd::String> leaves;
    for (const auto& leaf : tree)
        for (const auto& p : prefixes)
            if (leaf.path.size() > p.size() && leaf.path.starts_with(p)) {
                const dstd::StringView rest =
                    dstd::StringView(leaf.path).substr(p.size());
                if (digits(rest.substr(0, rest.find('.'))))
                    elements = true;
                else
                    leaves.push_back(leaf.path);
                break;
            }
    if (array || (elements && leaves.empty())) {
        if (std::find(arrays.begin(), arrays.end(), f) == arrays.end())
            arrays.push_back(f);
        return {list_type(tree, f, args_fallback).first};
    }
    if (!leaves.empty() && !elements) return leaves;
    return {f};
}

dataframe::TypeId observed_type(const dstd::String& type) {
    if (type == "int" || type == "uint") return dataframe::TypeId::Int64;
    if (type == "float") return dataframe::TypeId::Float64;
    if (type == "string") return dataframe::TypeId::String;
    if (type == "bool") return dataframe::TypeId::Bool;
    return dataframe::TypeId::Unknown;
}

dataframe::Series null_cell(const dataframe::DataType& type) {
    if (type.id == dataframe::TypeId::List && !type.fields.empty())
        return dataframe::Series::list({0, 0},
                                       null_cell(type.fields.front().type))
            .take(dstd::Vector<std::int64_t>{-1});
    const bool scalar = type.id != dataframe::TypeId::Unknown &&
                        type.id != dataframe::TypeId::List &&
                        type.id != dataframe::TypeId::Struct;
    return dataframe::Series::nulls(
        scalar ? type.id : dataframe::TypeId::String, 1);
}

dstd::String number_text(double v) {
    if (std::trunc(v) == v && std::abs(v) < 9.0e15)
        return dstd::to_string(static_cast<std::int64_t>(v));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

bool is_count(duql::AggFn fn) {
    return fn == duql::AggFn::Count || fn == duql::AggFn::CountIf ||
           fn == duql::AggFn::CountDistinct;
}

Status reject_resolved(const duql::Program& p) {
    Status result;
    auto check = [&result](dstd::StringView path) {
        if (!result || !path.starts_with("resolved.")) return;
        result = refuse<
            "'{}': 'resolved.' columns are removed; write an arrow into a "
            "row set of the source, such as 'fhash -> files.path', "
            "'hhash -> hosts.name' or 'run -> runs.app'">(path);
    };
    dstd::Function<void(const duql::detail::QueryNode&)> node =
        [&](const duql::detail::QueryNode& n) {
            std::visit(
                [&](const auto& x) {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, duql::detail::AndNode> ||
                                  std::is_same_v<T, duql::detail::OrNode>) {
                        node(*x.left);
                        node(*x.right);
                    } else if constexpr (std::is_same_v<
                                             T, duql::detail::NotNode>) {
                        node(*x.operand);
                    } else if constexpr (
                        std::is_same_v<T, duql::detail::CompareNode> ||
                        std::is_same_v<T, duql::detail::InNode> ||
                        std::is_same_v<T, duql::detail::NotInNode> ||
                        std::is_same_v<T, duql::detail::MatchNode>) {
                        check(x.field.path);
                    }
                },
                n.data);
        };
    auto pipeline = [&](const duql::Pipeline& pl) {
        if (pl.filter) node(*pl.filter);
        dstd::Vector<dstd::StringView> bases;
        duql::for_each_pipeline_term(pl, [&](const duql::Term& t) {
            if (const auto* f = std::get_if<duql::TField>(&t.node))
                bases.push_back(f->base);
        });
        for (const dstd::StringView base : bases) check(base);
    };
    pipeline(p.main);
    for (const auto& s : p.sides) pipeline(s.pipeline);
    return result;
}

dstd::String input_text(const duql::Input& in, const duql::Program& program) {
    switch (in.kind) {
        case duql::InputKind::Side:
            return dstd::String(program.sides[in.side].name);
        case duql::InputKind::All:
            return "all";
        case duql::InputKind::Data:
            return "data";
        case duql::InputKind::File:
            return dstd::fmt<"\"{}\"">(in.path);
    }
    return {};
}

void side_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out) {
    if (p.input.kind == duql::InputKind::Side) out.push_back(p.input.side);
    duql::for_each_pipeline_term(p, [&](const duql::Term& t) {
        if (const auto* l = std::get_if<duql::TLookup>(&t.node)) {
            out.push_back(l->side);
            if (l->empty) out.push_back(*l->empty);
        }
    });
    for (const auto& s : p.stages) {
        if (const auto* l = std::get_if<duql::PipelineLookup>(&s))
            out.push_back(l->side);
        if (const auto* u = std::get_if<duql::PipelineUnion>(&s)) {
            dstd::Vector<std::size_t> other;
            side_reads(*u->other, other);
            for (const auto i : other)
                if (std::find(out.begin(), out.end(), i) == out.end())
                    out.push_back(i);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void asof_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out) {
    for (const auto& s : p.stages) {
        if (const auto* l = std::get_if<duql::PipelineLookup>(&s);
            l && (std::holds_alternative<duql::PipelineAsof>(l->mode) ||
                  std::holds_alternative<duql::PipelineOverlap>(l->mode)))
            out.push_back(l->side);
        if (const auto* u = std::get_if<duql::PipelineUnion>(&s))
            asof_reads(*u->other, out);
    }
}

void term_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out) {
    duql::for_each_pipeline_term(p, [&](const duql::Term& t) {
        if (const auto* l = std::get_if<duql::TLookup>(&t.node)) {
            if (!duql::joined(*l) && !l->key_set) out.push_back(l->side);
            if (l->empty) out.push_back(*l->empty);
        }
    });
    asof_reads(p, out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void set_reads(const duql::Pipeline& p, dstd::Vector<std::size_t>& out) {
    duql::for_each_pipeline_term(p, [&](const duql::Term& t) {
        if (const auto* l = std::get_if<duql::TLookup>(&t.node);
            l && l->key_set)
            out.push_back(l->side);
    });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void each_pushed(
    const duql::Pipeline& p,
    const dstd::Function<void(const duql::Term&, const duql::TField&,
                              const duql::TLookup&, const duql::Term*)>& fn) {
    dstd::Function<void(const duql::detail::QueryNode&)> walk =
        [&](const duql::detail::QueryNode& n) {
            if (const auto* a = std::get_if<duql::detail::AndNode>(&n.data)) {
                walk(*a->left);
                walk(*a->right);
                return;
            }
            const auto* leaf = std::get_if<duql::detail::ExprLeaf>(&n.data);
            if (!leaf) return;
            const duql::Term* cond = nullptr;
            const duql::TLookup* l =
                std::get_if<duql::TLookup>(&leaf->term->node);
            if (l && (l->kind != duql::LookupKind::In || l->negated))
                l = nullptr;
            if (const auto* q = std::get_if<duql::TQuant>(&leaf->term->node);
                q && !q->all) {
                l = std::get_if<duql::TLookup>(&q->subject->node);
                cond = q->cond.get();
            }
            if (!l || l->keys.size() != 1) return;
            if (const auto* f = whole_field(l->keys.front().get()))
                fn(*leaf->term, *f, *l, cond);
        };
    if (p.filter) walk(*p.filter);
}

namespace {

dstd::Optional<duql::LiteralNode> literal_of(const dataframe::Series& s,
                                             std::int64_t r) {
    if (s.is_null(r)) return dstd::nullopt;
    const dataframe::TypeId t = s.type();
    if (t == dataframe::TypeId::Uint64)
        return duql::LiteralNode{s.data<std::uint64_t>()[r]};
    if (is_integer(t))
        return duql::LiteralNode{s.slice(r, 1)
                                     .cast(dataframe::TypeId::Int64)
                                     .data<std::int64_t>()[0]};
    if (t == dataframe::TypeId::Float32 || t == dataframe::TypeId::Float64)
        return duql::LiteralNode{
            s.slice(r, 1).cast(dataframe::TypeId::Float64).data<double>()[0]};
    if (t == dataframe::TypeId::Bool)
        return duql::LiteralNode{
            ((s.data<std::uint8_t>()[r >> 3] >> (r & 7)) & 1) != 0};
    if (t == dataframe::TypeId::String || t == dataframe::TypeId::LargeString)
        return duql::LiteralNode{dstd::String(s.string_at(r))};
    return dstd::nullopt;
}

}  // namespace

dstd::Optional<duql::Query> pushed(const Pushdown& p,
                                   dstd::Optional<duql::Query> query) {
    if (!query) return p.extra;
    duql::detail::QueryNodePtr root =
        duql::clone(duql::detail::QueryAccess::root(*query));
    dstd::Function<void(duql::detail::QueryNodePtr&)> walk =
        [&](duql::detail::QueryNodePtr& n) {
            if (auto* a = std::get_if<duql::detail::AndNode>(&n->data)) {
                walk(a->left);
                walk(a->right);
                return;
            }
            if (const auto* leaf =
                    std::get_if<duql::detail::ExprLeaf>(&n->data))
                for (const auto& [term, keys] : p.replace)
                    if (leaf->term.get() == term) {
                        n = duql::clone(duql::detail::QueryAccess::root(keys));
                        return;
                    }
        };
    walk(root);
    if (p.extra)
        root = duql::detail::make_node(duql::detail::AndNode{
            duql::clone(duql::detail::QueryAccess::root(*p.extra)),
            std::move(root)});
    return duql::detail::QueryAccess::from_node(std::move(root));
}

dstd::Result<Pushdown> key_sets(const duql::Pipeline& p) {
    Pushdown out;
    duql::detail::QueryNodePtr extra;
    dstd::Optional<dstd::Error> failure;
    each_pushed(p, [&](const duql::Term& leaf, const duql::TField& f,
                       const duql::TLookup& l, const duql::Term* cond) {
        if (failure) return;
        const duql::LookupTable& t = *l.slot->table;
        const dataframe::Series& keys = t.frame->columns[t.key_columns.front()];
        dstd::Optional<dataframe::Series> pass;
        bool record = false;
        if (cond)
            duql::for_each_term(*cond, [&](const duql::Term& x) {
                if (const auto* fx = std::get_if<duql::TField>(&x.node);
                    fx && fx->root != duql::FieldRoot::Element)
                    record = true;
                if (std::holds_alternative<duql::TLookup>(x.node) ||
                    std::holds_alternative<duql::TQuant>(x.node))
                    record = true;
            });
        if (cond && !record && t.value) {
            const dataframe::Series& values = t.frame->columns[*t.value];
            if (auto e = duql::vectorize_condition(
                    *cond, {{".", values.data_type()}}, false)) {
                auto evaluated = dataframe::eval(*e, {&values});
                if (!evaluated) {
                    failure = evaluated.error();
                    return;
                }
                pass = evaluated->materialize();
            }
        }
        auto passes = [&](std::int64_t r) {
            if (!pass) return !cond || !t.value || record;
            return !pass->is_null(r) &&
                   ((pass->data<std::uint8_t>()[r >> 3] >> (r & 7)) & 1);
        };
        duql::detail::ArrayNode arr;
        for (const auto& [key, rows] : t.rows)
            for (const std::int64_t r : rows)
                if (passes(r)) {
                    if (auto lit = literal_of(keys, r))
                        arr.elements.push_back(std::move(*lit));
                    break;
                }
        // Past the cap the list prunes nothing, and the lookup term filters
        // the same rows without a per-record scan of the list.
        if (arr.elements.size() > index::plan::SEMI_JOIN_CAP) return;
        arr.set = duql::make_in_set(arr);
        auto node = duql::detail::make_node(duql::detail::InNode{
            duql::detail::FieldNode{f.base}, std::move(arr)});
        // A condition on the row set's value alone passes the same records
        // as the key set, so the set replaces the term.
        if (!cond || pass) {
            out.replace.emplace_back(
                &leaf, duql::detail::QueryAccess::from_node(std::move(node)));
            return;
        }
        extra = extra ? duql::detail::make_node(duql::detail::AndNode{
                            std::move(extra), std::move(node)})
                      : std::move(node);
    });
    if (failure) return *failure;
    if (extra)
        out.extra = duql::detail::QueryAccess::from_node(std::move(extra));
    return out;
}

dstd::Optional<CacheKey> cache_key(const detail::ViewPlan& given,
                                   duql::InputKind input,
                                   dstd::StringView text) {
    // A file opened without an index path reads the index beside it.
    detail::ViewPlan plan = given;
    for (auto& f : plan.files)
        if (f.index_path.empty())
            f.index_path = internal::determine_index_path(f.file_path, "");
    const dstd::String path = index::cache::lookup_cache_path(plan);
    if (path.empty()) return dstd::nullopt;
    detail::plan_record_schema(plan);
    // A `data` side reads the View as given: its filters are part of it.
    dstd::String key(text);
    if (input == duql::InputKind::Data) {
        const dstd::String source =
            plan.query ? dstd::String(plan.query->source()) : dstd::String();
        key += dstd::fmt<"\n{}\n{}{}">(source, static_cast<int>(plan.phase),
                                       static_cast<int>(plan.all_records));
        if (plan.time_range)
            key += dstd::fmt<"\n{}..{}">(number_text(plan.time_range->first),
                                         number_text(plan.time_range->second));
    }
    const auto sig = index::cache::lookup_signature(plan, key);
    if (!sig) return dstd::nullopt;
    return CacheKey{dstd::String(path.data(), path.size()), *sig};
}

}  // namespace dftracer::utils::trace::views::duql_detail
