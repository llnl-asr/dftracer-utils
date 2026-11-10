// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/rest_column.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/vectorize.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_duql_internal.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

namespace dftracer::utils::trace::views::duql_detail {

void Applier::columns_from_view(bool records) {
    cols_.clear();
    for (const auto& f : v_.output_schema().fields) {
        dataframe::DataType type = f.type;
        if (type.id == dataframe::TypeId::Unknown)
            type = dataframe::scalar(observed(f.name));
        cols_.push_back(
            {dstd::String(f.name), std::move(type), is_json(f.name), records});
    }
    if (!records) return;
    for (const auto& a : arrays_) {
        auto i = index(a);
        if (!i) i = arg_index(a);
        if (i && cols_[*i].type.fields.empty() &&
            (cols_[*i].type.id == dataframe::TypeId::List ||
             cols_[*i].type.id == dataframe::TypeId::Unknown)) {
            auto [token, type] = list_type(tree(), a, schema_.args_fallback);
            cols_[*i].type = std::move(type);
            cols_[*i].json = token.starts_with(JSON_LIST);
        }
    }
}

dstd::Optional<duql::VectorColumn> Applier::scan_column(
    const dstd::String& base) {
    if (auto i = index(base)) return cols_[*i];
    if (auto i = arg_index(base)) return cols_[*i];
    if (!scan_) return dstd::nullopt;
    dataframe::TypeId t = observed(base);
    if (t == dataframe::TypeId::Unknown && schema_.args_fallback)
        t = observed("args." + base);
    if (t == dataframe::TypeId::Unknown) return dstd::nullopt;
    return duql::VectorColumn{base, dataframe::scalar(t), is_json(base)};
}

bool Applier::indexed(const dstd::String& f) {
    const dstd::String d = dotted(f);
    for (const auto& leaf : tree()) {
        for (const dstd::String& p :
             {d, schema_.args_fallback ? "args." + d : dstd::String()})
            if (!p.empty() && (leaf.path == p || (leaf.path.size() > p.size() &&
                                                  leaf.path.starts_with(p) &&
                                                  leaf.path[p.size()] == '.')))
                return true;
    }
    return false;
}

void Applier::leave_scan(const char* stage) {
    if (!scan_) return;
    // A closed pipeline reads only its fields; otherwise every column
    // stays and the fields add what the scan does not give yet.
    dstd::Vector<dstd::String> read = names();
    dstd::Vector<dstd::String> only;
    bool narrow = closed_;
    const auto add = [](dstd::Vector<dstd::String>& v, dstd::String f) {
        if (std::find(v.begin(), v.end(), f) == v.end())
            v.push_back(std::move(f));
    };
    for (const auto& f : fields_) {
        if (index(f) || arg_index(f)) {
            add(only, f);
            continue;
        }
        // A field the index does not list may still be in the events.
        if (narrow && !indexed(f)) narrow = false;
        for (auto& r : scan_reads(tree(), f, is_array(f), arrays_,
                                  schema_.args_fallback)) {
            add(only, r);
            add(read, std::move(r));
        }
    }
    if (narrow && !only.empty()) read = std::move(only);
    if (!narrow && read.size() > cols_.size() && name_paths(read)) {
        lines.push_back("scan names: " + joined(read));
    } else if (read.size() > cols_.size() ||
               (narrow && read.size() < cols_.size())) {
        const dstd::Vector<dstd::String> before = names();
        lines.push_back("scan select: " + joined(read));
        v_ = v_.select(std::move(read));
        columns_from_view(true);
        for (const auto& n : names())
            if (std::find(before.begin(), before.end(), n) == before.end())
                extras_.push_back(n);
    }
    scan_ = false;
    blocker_ = stage;
    lines.push_back("scan order: file, then line");
    v_ = ctx_.order(v_);
}

bool Applier::name_paths(const dstd::Vector<dstd::String>& read) {
    if (!v_.filters_events()) return false;
    dstd::Vector<dataframe::Field> named;
    for (std::size_t i = cols_.size(); i < read.size(); ++i) {
        if (read[i] != detail::select_source_field(read[i]) ||
            read[i].starts_with(detail::list_token("", "").substr(0, 2)))
            return false;
        named.push_back(dataframe::Field{
            dstd::String(read[i]), dataframe::scalar(observed(read[i])), true});
    }
    v_ = detail::with_named_columns(v_, std::move(named));
    columns_from_view(true);
    return true;
}

dstd::Vector<dstd::String> Applier::names() const {
    dstd::Vector<dstd::String> out;
    out.reserve(cols_.size());
    for (const auto& c : cols_) out.push_back(c.name);
    return out;
}

dstd::Vector<dataframe::DataType> Applier::types() const {
    dstd::Vector<dataframe::DataType> out;
    out.reserve(cols_.size());
    for (const auto& c : cols_) out.push_back(c.type);
    return out;
}

dstd::Optional<std::size_t> Applier::index(dstd::StringView name) const {
    for (std::size_t i = 0; i < cols_.size(); ++i)
        if (cols_[i].name == name) return i;
    return dstd::nullopt;
}

dstd::Optional<std::size_t> Applier::arg_index(const dstd::String& base) const {
    if (!schema_.args_fallback) return dstd::nullopt;
    return index("args." + base);
}

dataframe::Expr Applier::col(dstd::StringView name) const {
    return dataframe::expr_col(static_cast<std::int32_t>(*index(name)));
}

bool Applier::has_column_for(const dstd::String& base) const {
    auto under = [](const dstd::String& a, const dstd::String& b) {
        return a.size() > b.size() && a.starts_with(b) &&
               (a[b.size()] == '.' || a[b.size()] == '[');
    };
    auto reads = [&](const dstd::String& path) {
        for (const auto& c : cols_)
            if (c.name == path || under(path, c.name) || under(c.name, path))
                return true;
        return false;
    };
    return reads(base) || (schema_.args_fallback && reads("args." + base));
}

dstd::Result<dataframe::Expr> Applier::compile(const duql::TermRef& t,
                                               const dstd::String& text,
                                               bool condition) {
    if (auto h = hidden(t); !h) return h.error();
    // After the scan the columns are fixed, so a name none of them holds
    // is a mistake, not a missing field.
    if (!scan_) {
        dstd::Optional<dstd::String> missing;
        duql::for_each_term_field(*t, [&](const duql::TField& f) {
            if (!missing && f.root == duql::FieldRoot::Record &&
                !has_column_for(dstd::String(f.base)))
                missing = dstd::String(f.base);
        });
        if (missing)
            return refuse<"has no column '{}'; the columns here are {}">(
                *missing, joined(names()));
    }
    const bool fb = schema_.args_fallback;
    if (untyped(*t)) return per_batch(t, text, condition);
    auto e = condition ? duql::vectorize_condition(*t, cols_, fb)
                       : duql::vectorize(*t, cols_, fb);
    if (!e) return refuse<"{} in '{}'">(e.error().message(), text);
    return std::move(*e);
}

bool Applier::untyped(const duql::Term& t) const {
    bool out = false;
    duql::for_each_term_field(t, [&](const duql::TField& f) {
        if (f.root != duql::FieldRoot::Record) return;
        for (const auto& c : cols_)
            if (c.type.id == dataframe::TypeId::Unknown && !c.json &&
                (c.name == dstd::String(f.base) ||
                 (schema_.args_fallback &&
                  c.name == dstd::String("args." + f.base)) ||
                 (f.base.size() > c.name.size() && f.base.starts_with(c.name) &&
                  f.base[c.name.size()] == '.')))
                out = true;
    });
    return out;
}

dstd::Result<dataframe::Expr> Applier::per_batch(const duql::TermRef& t,
                                                 const dstd::String& text,
                                                 bool condition) {
    const dstd::String name = "__duql_t_" + dstd::to_string(typed_++);
    dstd::Vector<duql::VectorColumn> next = cols_;
    next.push_back(
        {name, dataframe::scalar(condition ? dataframe::TypeId::Bool
                                           : dataframe::TypeId::Unknown)});
    next.back().term = t.get();
    struct Cache {
        std::mutex mu;
        dstd::Map<dstd::Vector<dataframe::TypeId>, dataframe::Expr> exprs;
    };
    map_rows(
        [t, text, condition, name, cols = cols_, fb = schema_.args_fallback,
         cache = dstd::make_shared<Cache>()](
            dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            dstd::Vector<duql::VectorColumn> typed = cols;
            dstd::Vector<dataframe::TypeId> types;
            for (std::size_t i = 0; i < typed.size(); ++i) {
                if (typed[i].type.id == dataframe::TypeId::Unknown)
                    typed[i].type = f.columns[i].data_type();
                types.push_back(typed[i].type.id);
            }
            dataframe::Expr e;
            {
                std::lock_guard<std::mutex> lock(cache->mu);
                auto it = cache->exprs.find(types);
                if (it == cache->exprs.end()) {
                    auto built = condition
                                     ? duql::vectorize_condition(*t, typed, fb)
                                     : duql::vectorize(*t, typed, fb);
                    if (!built)
                        return common::make_error<"{} in '{}'">(
                            common::ErrorCode::InvalidArgument,
                            built.error().message(), text);
                    it = cache->exprs.emplace(types, std::move(*built)).first;
                }
                e = it->second;
            }
            // Series*>
            dstd::Vector<const dataframe::Series*> inputs;
            for (const auto& c : f.columns) inputs.push_back(&c);
            auto out = dataframe::eval(e, inputs);
            if (!out) return out.error();
            f.names.emplace_back(name);
            f.columns.push_back(std::move(*out));
            return f;
        },
        std::move(next), dstd::fmt<"per batch {} = {}">(name, text));
    return dataframe::expr_col(static_cast<std::int32_t>(cols_.size() - 1));
}

void Applier::map_rows(FrameFn fn, dstd::Vector<duql::VectorColumn> next,
                       dstd::String line) {
    lines.push_back(std::move(line));
    dataframe::Schema schema;
    for (const auto& c : next)
        schema.fields.push_back(
            dataframe::Field{dstd::String(c.name), c.type, true});
    dstd::Vector<dstd::Pair<dstd::String, dataframe::TypeId>> in;
    for (const auto& c : cols_) in.emplace_back(c.name, c.type.id);
    v_ = v_.with_lazy(map_source(
        v_.lazy(), std::move(schema),
        [in = std::move(in), fn = std::move(fn)](
            dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            return fn(aligned(std::move(f), in));
        }));
    cols_ = std::move(next);
}

Status Applier::hidden(const duql::TermRef& t) {
    // A call inside a quantifier's condition reads the element, which
    // only the quantifier's own frame holds.
    dstd::Vector<const duql::Term*> inner;
    duql::for_each_term(*t, [&](const duql::Term& x) {
        if (const auto* q = std::get_if<duql::TQuant>(&x.node))
            duql::for_each_term(
                *q->cond, [&](const duql::Term& y) { inner.push_back(&y); });
    });
    // The vectorizer reads a list literal that contains() tests directly.
    dstd::Vector<const duql::Term*> literal;
    duql::for_each_term(*t, [&](const duql::Term& x) {
        if (const auto* c = std::get_if<duql::TCall>(&x.node);
            c && c->fn == duql::Fn::Contains &&
            std::holds_alternative<duql::TList>(c->args[0]->node))
            literal.push_back(c->args[0].get());
    });
    dstd::Vector<const duql::Term*> found;
    duql::for_each_term(*t, [&](const duql::Term& x) {
        const auto* l = std::get_if<duql::TLookup>(&x.node);
        if (std::holds_alternative<duql::TQuant>(x.node) || (l && !l->all) ||
            (duql::is_column_term(x) &&
             std::find(inner.begin(), inner.end(), &x) == inner.end() &&
             std::find(literal.begin(), literal.end(), &x) == literal.end()))
            found.push_back(&x);
    });
    for (auto it = found.rbegin(); it != found.rend(); ++it) {
        Status done;
        if (std::holds_alternative<duql::TQuant>((*it)->node))
            done = quantifier(t, *it);
        else if (duql::is_column_term(**it))
            done = call(t, *it);
        else
            done = lookup(t, *it);
        if (!done) return done;
    }
    return {};
}

Status Applier::object_fields(const duql::TField& f, const dstd::String& text,
                              bool values, dstd::Vector<dstd::String>& keys,
                              dstd::Vector<dataframe::Expr>& args,
                              dstd::Vector<dataframe::DataType>& types) {
    if (f.root != duql::FieldRoot::Record || f.neg_at != f.steps.size())
        return {};
    if (auto i = field_index(dstd::String(f.base))) {
        if (cols_[*i].type.id != dataframe::TypeId::Struct) return {};
        for (const auto& x : cols_[*i].type.fields) keys.emplace_back(x.name);
        args.push_back(col(cols_[*i].name));
        types.push_back(cols_[*i].type);
        return {};
    }
    dstd::String prefix = dstd::String(f.base + ".");
    if (schema_.args_fallback &&
        !std::any_of(cols_.begin(), cols_.end(),
                     [&](const auto& c) { return c.name.starts_with(prefix); }))
        prefix = "args." + prefix;
    dstd::Map<dstd::String, dstd::Vector<std::size_t>> under;
    for (std::size_t i = 0; i < cols_.size(); ++i) {
        if (!cols_[i].name.starts_with(prefix)) continue;
        const dstd::String rest = cols_[i].name.substr(prefix.size());
        under[rest.substr(0, rest.find_first_of(".["))].push_back(i);
    }
    for (const auto& [key, at] : under) {
        const dstd::String direct = prefix + key;
        keys.push_back(key);
        if (at.size() == 1 && cols_[at.front()].name == direct &&
            !cols_[at.front()].json) {
            args.push_back(col(direct));
            types.push_back(cols_[at.front()].type);
            continue;
        }
        if (values)
            return refuse<
                "'{}' reads '{}', which holds an object or list; only a scan "
                "filter evaluates it">(text, direct);
        dataframe::Expr any;
        for (const std::size_t i : at) {
            const dataframe::Expr has =
                dataframe::expr_is_null(col(cols_[i].name), false);
            any = any.valid() ? dataframe::expr_logical(
                                    dataframe::LogicalOp::Or, any, has)
                              : has;
        }
        args.push_back(dataframe::expr_select(
            any, dataframe::expr_lit_bool(true),
            dataframe::expr_lit_null(dataframe::TypeId::Bool)));
        types.push_back(dataframe::scalar(dataframe::TypeId::Bool));
    }
    return {};
}

Status Applier::call(const duql::TermRef& t, const duql::Term* term) {
    const auto* c = std::get_if<duql::TCall>(&term->node);
    const auto* ix = std::get_if<duql::TIndex>(&term->node);
    const auto* ls = std::get_if<duql::TList>(&term->node);
    if (std::any_of(cols_.begin(), cols_.end(), [&](const auto& x) {
            return (c && x.call == c) || (ix && x.index == ix) ||
                   (ls && x.list == ls);
        }))
        return {};
    const dstd::String text = dstd::String(duql::term_text(*term));
    dstd::Vector<const duql::Term*> operands;
    if (c)
        for (const auto& a : c->args) operands.push_back(a.get());
    else if (ix)
        operands = {ix->array.get(), ix->index.get()};
    else
        for (const auto& a : ls->items) operands.push_back(a.get());
    dstd::Vector<dataframe::Expr> args;
    dstd::Vector<dataframe::DataType> arg_types;
    dstd::Vector<dstd::String> keys;
    if (c && (c->fn == duql::Fn::Keys || c->fn == duql::Fn::Values)) {
        if (const auto* f = std::get_if<duql::TField>(&c->args[0]->node))
            if (auto done = object_fields(*f, text, c->fn == duql::Fn::Values,
                                          keys, args, arg_types);
                !done)
                return done;
    } else {
        for (const auto* a : operands) {
            if (const auto* inner = std::get_if<duql::TCall>(&a->node);
                inner && inner->fn == duql::Fn::ParseJson)
                return refuse<
                    "'{}' reads a parse_json() value, which only a scan "
                    "filter evaluates">(text);
            auto e = duql::vectorize(*a, cols_, schema_.args_fallback);
            if (!e) return refuse<"{} in '{}'">(e.error().message(), text);
            args.push_back(std::move(*e));
            if (ls && duql::is_null_term(*a, cols_, schema_.args_fallback)) {
                arg_types.push_back(
                    dataframe::scalar(dataframe::TypeId::Unknown));
            } else {
                auto inferred = dataframe::infer_type(args.back(), types());
                if (!inferred) return inferred.error();
                arg_types.push_back(std::move(*inferred));
            }
        }
    }
    const dstd::String name = "__duql_c_" + dstd::to_string(calls_++);
    auto mark = [&](duql::VectorColumn& col) {
        col.call = c;
        col.index = ix;
        col.list = ls;
    };
    // An operand of no known type: each batch types the call from its
    // own operand columns, as a plan that declared them so would.
    dstd::Vector<bool> per_batch(operands.size(), false);
    for (std::size_t i = 0; i < operands.size() && i < arg_types.size(); ++i)
        per_batch[i] = arg_types[i].id == dataframe::TypeId::Unknown &&
                       untyped(*operands[i]);
    if (std::find(per_batch.begin(), per_batch.end(), true) !=
        per_batch.end()) {
        dstd::Vector<duql::VectorColumn> next = cols_;
        next.push_back({name, dataframe::scalar(dataframe::TypeId::Unknown)});
        mark(next.back());
        map_rows(
            [term, args, keys, arg_types, per_batch, text, name](
                dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
                // Series*>
                dstd::Vector<const dataframe::Series*> inputs;
                for (const auto& x : f.columns) inputs.push_back(&x);
                dstd::Vector<dataframe::Series> values;
                dstd::Vector<dataframe::DataType> types = arg_types;
                for (std::size_t i = 0; i < args.size(); ++i) {
                    auto value = dataframe::eval(args[i], inputs);
                    if (!value) return value.error();
                    values.push_back(std::move(*value));
                    if (per_batch[i]) types[i] = values.back().data_type();
                }
                auto called = duql::call_type(*term, types);
                if (!called)
                    return common::make_error<"'{}': {}">(
                        common::ErrorCode::InvalidArgument, text,
                        called.error().message());
                dstd::Optional<dataframe::DataType> type = *called;
                const std::int64_t rows = f.num_rows();
                f.names.emplace_back(name);
                f.columns.push_back(
                    type ? duql::call_column(*term, values, keys, *type, rows)
                         : dataframe::Series::nulls(dataframe::TypeId::Bool,
                                                    rows));
                return f;
            },
            std::move(next), dstd::fmt<"call {} per batch = {}">(name, text));
        return {};
    }
    auto called = duql::call_type(*term, arg_types);
    if (!called) return refuse<"'{}': {}">(text, called.error().message());
    dstd::Optional<dataframe::DataType> type = *called;
    if (!type) {
        if (auto done = with_column(
                name, dataframe::expr_lit_null(dataframe::TypeId::Bool), text);
            !done)
            return done;
        cols_.back().type = dataframe::scalar(dataframe::TypeId::Unknown);
        mark(cols_.back());
        return {};
    }
    dstd::Vector<duql::VectorColumn> next = cols_;
    next.push_back({name, *type, c && c->fn == duql::Fn::ParseJson, false});
    mark(next.back());
    map_rows(
        [t, term, args, keys, type = *type,
         name](dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            // Series*>
            dstd::Vector<const dataframe::Series*> inputs;
            for (const auto& x : f.columns) inputs.push_back(&x);
            dstd::Vector<dataframe::Series> values;
            for (const auto& a : args) {
                auto value = dataframe::eval(a, inputs);
                if (!value) return value.error();
                values.push_back(std::move(*value));
            }
            const std::int64_t rows = f.num_rows();
            f.names.emplace_back(name);
            f.columns.push_back(
                duql::call_column(*term, values, keys, type, rows));
            return f;
        },
        std::move(next), dstd::fmt<"call {} = {}">(name, text));
    return {};
}

Status Applier::ready(const dstd::SharedPtr<const detail::BuildStep>& step) {
    if (!step) return {};
    auto ran = step->run(dstd::nullopt);
    if (!ran) return ran.error();
    return {};
}

Status Applier::quantifier(const duql::TermRef& t, const duql::Term* term) {
    const auto* q = &std::get<duql::TQuant>(term->node);
    if (std::any_of(cols_.begin(), cols_.end(),
                    [q](const auto& c) { return c.quant == q; }))
        return {};
    if (const auto* f = std::get_if<duql::TField>(&q->subject->node);
        f && f->root == duql::FieldRoot::Record && !array_field(*q->subject))
        return refuse<
            "'{}' walks a negative index; only a scan filter evaluates it">(
            duql::term_text(*term));
    if (const auto* f = array_field(*q->subject))
        if (const auto i = field_index(dstd::String(f->base));
            i && cols_[*i].json)
            return refuse<
                "'{}' reads '{}', whose elements mix types; only a scan "
                "filter evaluates it">(duql::term_text(*term), f->base);
    const dstd::String name = "__duql_q_" + dstd::to_string(quants_++);
    dstd::Vector<duql::VectorColumn> in = cols_;
    dstd::Vector<duql::VectorColumn> next = cols_;
    next.push_back(
        {name, dataframe::scalar(dataframe::TypeId::Bool), false, false, q});
    map_rows(
        [t, q, in = std::move(in), name, step = step_,
         fb = schema_.args_fallback](
            dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            if (auto bound = ready(step); !bound) return bound.error();
            // Series*>
            dstd::Vector<const dataframe::Series*> inputs;
            for (const auto& c : f.columns) inputs.push_back(&c);
            auto truth = duql::quantify(*q, inputs, in, fb);
            if (!truth) return refuse<"{}">(truth.error().message());
            f.names.emplace_back(name);
            f.columns.push_back(std::move(*truth));
            return f;
        },
        std::move(next),
        dstd::fmt<"quantify {} = {}">(name, duql::term_text(*term)));
    return {};
}

dataframe::DataType Applier::side_type(std::size_t side,
                                       dstd::StringView name) const {
    const auto& cols = ctx_.sides->columns[side];
    for (const auto& c : cols)
        if (c.name == name) return c.type;
    if (schema_.args_fallback)
        for (const auto& c : cols)
            if (c.name == "args." + dstd::String(name)) return c.type;
    return dataframe::scalar(dataframe::TypeId::Unknown);
}

dstd::Result<dstd::Vector<dstd::String>> Applier::key_columns(
    const duql::TermRef& t, const duql::TLookup& l, const dstd::String& id) {
    dstd::Vector<dstd::String> out;
    for (std::size_t k = 0; k < l.keys.size(); ++k) {
        const duql::TermRef key(t, l.keys[k].get());
        auto column =
            source({"", key, duql::term_text(*key)},
                   dstd::fmt<"__duql_lk{}_{}">(id, dstd::to_string(k)));
        if (!column) return column.error();
        out.push_back(std::move(*column));
    }
    return out;
}

dstd::Result<dataframe::LazyFrame> Applier::joined_side(
    const duql::TLookup& l, dstd::Vector<dstd::String>& right, bool& memory,
    const dstd::String& marker) {
    const auto& side_cols = ctx_.sides->columns[l.side];
    if (l.kind == duql::LookupKind::In) {
        if (side_cols.size() != l.keys.size())
            return refuse<
                "the sub-query '{}' gives {} columns; 'in' compares {}">(
                l.name, side_cols.size(), l.keys.size() - l.correlated);
        for (const auto& c : side_cols) right.push_back(c.name);
    } else {
        right.clear();
        for (const auto& t : l.target) right.emplace_back(t);
    }
    auto side = side_plan(l.side, memory);
    if (!side) return side.error();
    if (marker.empty()) return std::move(*side);
    return side->with_column(std_str(marker), dataframe::expr_lit_bool(true));
}

bool Applier::typed_keys(const duql::TLookup& l) const {
    const auto& side_cols = ctx_.sides->columns[l.side];
    if (side_cols.size() != l.keys.size()) return false;
    for (std::size_t k = 0; k < l.keys.size(); ++k) {
        const auto* f = whole_field(l.keys[k].get());
        const auto i = f ? index(f->base) : dstd::nullopt;
        if (!i) return false;
        const dataframe::DataType& a = cols_[*i].type;
        const dataframe::DataType& b = side_cols[k].type;
        const bool exact = is_integer(a.id) ||
                           a.id == dataframe::TypeId::String ||
                           a.id == dataframe::TypeId::Bool;
        if (!exact || a.id != b.id || a.is_json() || b.is_json()) return false;
    }
    return true;
}

Status Applier::semi_join(const duql::TermRef& t, const duql::TLookup& l) {
    const dstd::Vector<dstd::String> before = names();
    const dstd::String id = dstd::to_string(lookups_++);
    auto keys = key_columns(t, l, id);
    if (!keys) return keys.error();
    const dstd::Vector<dstd::String>& left = *keys;
    dstd::Vector<dstd::String> right;
    bool memory = false;
    auto joined = joined_side(l, right, memory, "");
    if (!joined) return joined.error();
    dataframe::LazyFrame side = std::move(*joined);
    lines.push_back(dstd::fmt<"semi join {} on {}{}">(
        l.name, duql_detail::joined(left),
        memory ? " (side from memory)" : " (side joined, sharing the scan)"));
    v_ =
        v_.with_lazy(v_.lazy().join(std::move(side), std_strs(left),
                                    std_strs(right), dataframe::JoinHow::Semi));
    if (names() != before) return keep(before);
    return {};
}

Status Applier::join_lookup(const duql::TermRef& t, const duql::Term* term) {
    const auto* l = &std::get<duql::TLookup>(term->node);
    const dstd::Vector<dstd::String> before = names();
    const dstd::String id = dstd::to_string(lookups_++);
    const dstd::String name = "__duql_l_" + id;
    auto keys = key_columns(t, *l, id);
    if (!keys) return keys.error();
    const dstd::Vector<dstd::String> left = std::move(*keys);
    dstd::Vector<dstd::String> right;
    bool memory = false;
    const bool in = l->kind == duql::LookupKind::In;
    const dstd::String matched = "__duql_lm_" + id;
    auto joined_lazy =
        joined_side(*l, right, memory, in ? matched : dstd::String());
    if (!joined_lazy) return joined_lazy.error();
    dataframe::LazyFrame side = std::move(*joined_lazy);
    lines.push_back(dstd::fmt<"{} {} on {}{}">(
        in ? "lookup join" : "nest join", l->name, joined(left),
        memory ? " (side from memory)" : " (side joined, sharing the scan)"));
    dstd::Optional<std::size_t> field;
    dataframe::DataType type = dataframe::scalar(dataframe::TypeId::Bool);
    if (in) {
        v_ = v_.with_lazy(v_.lazy().join(std::move(side), std_strs(left),
                                         std_strs(right),
                                         dataframe::JoinHow::Lookup));
        cols_.push_back({matched, dataframe::scalar(dataframe::TypeId::Bool)});
    } else {
        dstd::Vector<dataframe::Field> fields;
        const auto& side_cols = ctx_.sides->columns[l->side];
        for (std::size_t c = 0; c < side_cols.size(); ++c) {
            fields.push_back(dataframe::Field{std_str(side_cols[c].name),
                                              side_cols[c].type, true});
            if (side_cols[c].name == dstd::String(l->column)) field = c;
        }
        if (!field)
            return refuse<"'{}' has no column '{}'">(l->name, l->column);
        type = side_cols[*field].type;
        v_ = v_.with_lazy(
            v_.lazy().join(std::move(side), std_strs(left), std_strs(right),
                           dataframe::JoinHow::Nest, std_str(matched)));
        cols_.push_back({matched, dataframe::list_of(dataframe::struct_of(
                                      std::move(fields)))});
    }
    dstd::Vector<duql::VectorColumn> next = cols_;
    next.push_back({name, type, false, false, nullptr, l});
    dstd::SharedPtr<Sides> strong = main_ ? ctx_.sides : nullptr;
    dstd::WeakPtr<Sides> weak = ctx_.sides;
    const std::size_t subject = l->keys.size() - l->correlated;
    const bool negated = l->negated;
    const dstd::String sub = dstd::String(l->name);
    const dstd::Optional<std::size_t> empty = l->empty;
    map_rows(
        [=](dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            auto at = [&](const dstd::String& n) -> const dataframe::Series* {
                for (std::size_t i = 0; i < f.names.size(); ++i)
                    if (dstd::StringView(f.names[i]) == n) return &f.columns[i];
                return nullptr;
            };
            dstd::Vector<dataframe::Series> keys;
            for (const auto& k : left) {
                const dataframe::Series* s = at(k);
                if (!s)
                    return refuse<"the join of '{}' lost column '{}'">(sub, k);
                keys.push_back(s->materialize());
            }
            // Series*>
            dstd::Vector<const dataframe::Series*> ptrs;
            for (const auto& k : keys) ptrs.push_back(&k);
            const dataframe::Series* marked = at(matched);
            if (!marked)
                return refuse<"the join of '{}' lost column '{}'">(sub,
                                                                   matched);
            dataframe::Series out;
            if (in) {
                out = duql::in_column(ptrs, subject, marked->materialize(),
                                      negated);
            } else {
                dstd::SharedPtr<const dataframe::DataFrame> none;
                if (empty) {
                    dstd::SharedPtr<Sides> sides = strong;
                    if (!sides) {
                        auto owned = owner(weak);
                        if (!owned) return owned.error();
                        sides = std::move(*owned);
                    }
                    auto rows = sides->frame(*empty);
                    if (!rows) return rows.error();
                    none = std::move(*rows);
                }
                auto column = duql::scalar_column(dstd::String(sub), ptrs,
                                                  marked->materialize(), *field,
                                                  none.get());
                if (!column) return column.error();
                out = std::move(*column);
            }
            f.names.emplace_back(name);
            f.columns.push_back(std::move(out));
            return f;
        },
        std::move(next),
        dstd::fmt<"lookup {} = {}">(name, duql::term_text(*term)));
    dstd::Vector<dstd::String> keep_names = before;
    keep_names.push_back(name);
    return keep(std::move(keep_names));
}

Status Applier::lookup(const duql::TermRef& t, const duql::Term* term) {
    const auto* l = &std::get<duql::TLookup>(term->node);
    if (std::any_of(cols_.begin(), cols_.end(),
                    [l](const auto& c) { return c.lookup == l; }))
        return {};
    if (duql::joined(*l)) return join_lookup(t, term);
    dataframe::DataType type = dataframe::scalar(dataframe::TypeId::Bool);
    if (l->kind == duql::LookupKind::Arrow) {
        type = side_type(l->side, l->column);
    } else if (l->kind == duql::LookupKind::Scalar) {
        const auto& cols = ctx_.sides->columns[l->side];
        if (l->range == duql::RangeRead::Sum) {
            const dataframe::TypeId v = side_type(l->side, l->column).id;
            type = dataframe::scalar(v == dataframe::TypeId::Float32 ||
                                             v == dataframe::TypeId::Float64
                                         ? dataframe::TypeId::Float64
                                         : dataframe::TypeId::Int64);
        } else if (l->range == duql::RangeRead::Mean) {
            type = dataframe::scalar(dataframe::TypeId::Float64);
        } else if (l->range == duql::RangeRead::Count ||
                   l->range == duql::RangeRead::CountValues ||
                   l->range == duql::RangeRead::CountIf) {
            type = dataframe::scalar(dataframe::TypeId::Int64);
        } else if (!l->keys.empty())
            type = side_type(l->side, l->column);
        else if (!cols.empty())
            type = side_type(l->side, cols.front().name);
    }
    const dstd::String name = "__duql_l_" + dstd::to_string(lookups_++);
    dstd::Vector<duql::VectorColumn> in = cols_;
    dstd::Vector<duql::VectorColumn> next = cols_;
    next.push_back({name, type, false, false, nullptr, l});
    map_rows(
        [t, l, in = std::move(in), name, step = step_,
         fb = schema_.args_fallback](
            dataframe::DataFrame f) -> dstd::Result<dataframe::DataFrame> {
            if (auto bound = ready(step); !bound) return bound.error();
            // Series*>
            dstd::Vector<const dataframe::Series*> inputs;
            for (const auto& c : f.columns) inputs.push_back(&c);
            auto values = duql::lookup(*l, inputs, in, fb);
            if (!values) return refuse<"{}">(values.error().message());
            f.names.emplace_back(name);
            f.columns.push_back(std::move(*values));
            return f;
        },
        std::move(next),
        dstd::fmt<"lookup {} = {}">(name, duql::term_text(*term)));
    return {};
}

Status Applier::with_column(const dstd::String& name, const dataframe::Expr& e,
                            const dstd::String& text) {
    auto type = dataframe::infer_type(e, types());
    if (!type) return type.error();
    v_ = v_.with_column(std_str(name), e);
    if (auto i = index(name))
        cols_[*i] = {name, std::move(*type), false};
    else
        cols_.push_back({name, std::move(*type), false});
    lines.push_back(dstd::fmt<"with_column {} = {}">(name, text));
    return {};
}

Status Applier::keep(dstd::Vector<dstd::String> keep, bool rest) {
    dstd::Vector<duql::VectorColumn> next;
    for (const auto& n : keep) {
        const auto i = index(n);
        if (!i) return refuse<"has no column '{}'">(n);
        next.push_back(cols_[*i]);
    }
    lines.push_back(dstd::fmt<"select {}">(joined(keep)));
    if (rest) keep.emplace_back(dataframe::REST_COLUMN);
    v_ = v_.with_lazy(v_.lazy().select(std_strs(keep)));
    cols_ = std::move(next);
    return {};
}

void Applier::rename(dstd::Vector<dstd::String> to) {
    for (std::size_t i = 0; i < to.size(); ++i) cols_[i].name = to[i];
    lines.push_back("rename " + joined(to));
    v_ = v_.with_lazy(v_.lazy().rename(std_strs(to)));
}

dstd::Result<dstd::String> Applier::source(const duql::PipelineItem& it,
                                           dstd::String tmp) {
    const auto* f = std::get_if<duql::TField>(&it.term->node);
    if (f && f->base == it.name && f->neg_at == f->steps.size() &&
        index(it.name))
        return dstd::String(it.name);
    if (const auto* w = whole_field(it.term)) {
        if (index(w->base)) return dstd::String(w->base);
        if (arg_index(dstd::String(w->base)))
            return dstd::String("args." + w->base);
    }
    auto compiled = compile(it.term, dstd::String(it.text));
    if (!compiled) return compiled.error();
    if (auto done = with_column(tmp, *compiled, dstd::String(it.text)); !done)
        return done.error();
    return tmp;
}

Status Applier::project(const dstd::Vector<duql::PipelineItem>& items) {
    extras_.clear();
    dstd::Vector<dstd::String> sources;
    dstd::Vector<dstd::String> final_names;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const dstd::String tmp = "__duql_" + dstd::to_string(i);
        auto found = source(items[i], tmp);
        if (!found) return found.error();
        dstd::String from = std::move(*found);
        if (std::find(sources.begin(), sources.end(), from) != sources.end()) {
            if (auto done =
                    with_column(tmp, col(from), dstd::String(items[i].text));
                !done)
                return done;
            from = tmp;
        }
        sources.push_back(std::move(from));
        final_names.push_back(dstd::String(items[i].name));
    }
    if (auto done = keep(std::move(sources), false); !done) return done;
    if (names() != final_names) rename(std::move(final_names));
    return {};
}

Status Applier::stage(const duql::PipelineWhere& w) {
    leave_scan("where");
    if (const auto* l = std::get_if<duql::TLookup>(&w.condition->node);
        l && l->kind == duql::LookupKind::In && !l->negated &&
        duql::joined(*l) && typed_keys(*l))
        return semi_join(w.condition, *l);
    auto condition = compile(w.condition, dstd::String(w.text), true);
    if (!condition) return condition.error();
    v_ = v_.filter(std::move(*condition));
    lines.push_back(dstd::fmt<"filter after scan: {}">(w.text));
    return {};
}

Status Applier::stage(const duql::PipelineSelect& s) {
    leave_scan("select");
    return project(s.items);
}

Status Applier::stage(const duql::PipelineDerive& d) {
    leave_scan("derive");
    for (const auto& it : d.items) {
        auto compiled = compile(it.term, dstd::String(it.text));
        if (!compiled) return compiled.error();
        if (auto done = with_column(dstd::String(it.name), *compiled,
                                    dstd::String(it.text));
            !done)
            return done;
    }
    return {};
}

Status Applier::stage(const duql::PipelineDrop& d) {
    leave_scan("drop");
    cols_.erase(std::remove_if(cols_.begin(), cols_.end(),
                               [&](const duql::VectorColumn& c) {
                                   return std::any_of(
                                       d.names.begin(), d.names.end(),
                                       [&](const dstd::String& n) {
                                           return dstd::StringView(n) == c.name;
                                       });
                               }),
                cols_.end());
    lines.push_back(dstd::fmt<"drop {}">(joined(d.names)));
    v_ = v_.with_lazy(v_.lazy().drop(std_strs(strings_of(d.names))));
    return {};
}

Status Applier::stage(const duql::PipelineRename& r) {
    leave_scan("rename");
    dstd::Vector<dstd::String> from, to;
    for (const auto& [next, old] : r.pairs) {
        from.push_back(dstd::String(old));
        to.push_back(dstd::String(next));
    }
    dstd::Vector<dstd::String> after = names();
    for (std::size_t k = 0; k < from.size(); ++k)
        if (const auto i = index(from[k])) after[*i] = to[k];
    for (std::size_t i = 0; i < cols_.size(); ++i) cols_[i].name = after[i];
    lines.push_back(
        dstd::fmt<"rename_columns {} -> {}">(joined(from), joined(to)));
    v_ = v_.with_lazy(v_.lazy().rename_columns(std_strs(from), std_strs(to)));
    return {};
}

Status Applier::stage(const duql::PipelineDistinct& d) {
    leave_scan("distinct");
    if (!d.items.empty())
        if (auto done = project(d.items); !done) return done;
    v_ = v_.with_lazy(v_.unique());
    lines.push_back("unique");
    return {};
}

dstd::Result<Applier::SortColumns> Applier::sort_columns(
    const dstd::Vector<duql::PipelineSortKey>& keys) {
    SortColumns out;
    for (std::size_t k = 0; k < keys.size(); ++k) {
        const auto& key = keys[k];
        dstd::String name = dstd::String(key.key.name);
        if (!index(name)) {
            name = dstd::fmt<"__duql_sort_{}">(k);
            auto compiled = compile(key.key.term, dstd::String(key.key.text));
            if (!compiled) return compiled.error();
            if (auto done =
                    with_column(name, *compiled, dstd::String(key.key.text));
                !done)
                return done.error();
        }
        if (key.nulls_first) {
            dstd::String flag = dstd::fmt<"__duql_nulls_{}">(k);
            if (auto done =
                    with_column(flag, dataframe::expr_is_null(col(name)),
                                dstd::fmt<"{} is null">(key.key.text));
                !done)
                return done.error();
            out.by.push_back(std::move(flag));
            out.desc.push_back(true);
        }
        out.by.push_back(name);
        out.desc.push_back(key.descending);
        out.keys.push_back(name);
    }
    return out;
}

Status Applier::sort(const dstd::Vector<duql::PipelineSortKey>& keys) {
    const dstd::Vector<dstd::String> before = names();
    auto sorted = sort_columns(keys);
    if (!sorted) return sorted.error();
    SortColumns c = std::move(*sorted);
    lines.push_back(dstd::fmt<"sort_by_multi {}">(joined(c.by)));
    v_ = v_.sort_by_multi(std_strs(c.by), std::move(c.desc));
    if (names() != before) return keep(before);
    return {};
}

Status Applier::stage(const duql::PipelineSort& s) {
    leave_scan("sort");
    return sort(s.keys);
}

Status Applier::stage(const duql::PipelineTake& t) {
    leave_scan("take");
    v_ = v_.head(t.count);
    lines.push_back(dstd::fmt<"head {}">(t.count));
    return {};
}

Status Applier::stage(const duql::PipelineSkip& s) {
    leave_scan("skip");
    v_ = v_.slice(s.count, std::numeric_limits<std::int64_t>::max());
    lines.push_back(dstd::fmt<"slice {}">(s.count));
    return {};
}

Status Applier::stage(const duql::PipelineTakeBy& t) {
    leave_scan("take");
    const dstd::Vector<dstd::String> before = names();
    if (!t.order.empty())
        if (auto done = sort(t.order); !done) return done;
    dstd::Vector<dstd::String> keys;
    for (std::size_t k = 0; k < t.keys.size(); ++k) {
        auto column = source(t.keys[k], dstd::fmt<"__duql_by_{}">(k));
        if (!column) return column.error();
        keys.push_back(std::move(*column));
    }
    lines.push_back(dstd::fmt<"head_by {} {}">(joined(keys), t.count));
    v_ = v_.with_lazy(v_.lazy().head_by(std_strs(keys), t.count));
    if (names() != before) return keep(before);
    return {};
}

}  // namespace dftracer::utils::trace::views::duql_detail
