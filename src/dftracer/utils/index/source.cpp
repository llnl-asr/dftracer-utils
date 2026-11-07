// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/macros.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/term.h>
#include <dftracer/utils/index/source.h>

#include <algorithm>
#include <utility>

namespace dftracer::utils::index {

namespace {

std::int64_t ns_per(TimeUnit unit) {
    switch (unit) {
        case TimeUnit::Ns:
            return 1;
        case TimeUnit::Us:
            return 1000;
        case TimeUnit::Ms:
            return 1'000'000;
        case TimeUnit::S:
            return 1'000'000'000;
    }
    return 1000;
}

struct Member {
    dstd::String name;
    dstd::String text;
};

[[noreturn]] void bad(dstd::StringView origin, dstd::StringView why) {
    throw common::DFTUtilsException::cat(common::ErrorCode::InvalidArgument,
                                         "source of ", origin, ": ", why);
}

duql::syntax::Program parse_members(dstd::StringView text,
                                    dstd::StringView origin) {
    auto tree =
        duql::syntax::parse("source s {\n" + dstd::String(text) + "\n}");
    if (!tree) bad(origin, tree.error().format());
    return std::move(*tree);
}

// The members of `text` in order, each as canonical text.
dstd::Vector<Member> members(dstd::StringView text, dstd::StringView origin) {
    dstd::Vector<Member> out;
    if (text.empty()) return out;
    duql::syntax::Program tree = parse_members(text, origin);
    const auto& decl = std::get<duql::syntax::SourceDecl>(tree.decls.front());
    auto add = [&](dstd::String name, dstd::String member) {
        for (const auto& m : out)
            if (m.name == name) bad(origin, "'" + name + "' is declared twice");
        out.push_back({std::move(name), std::move(member)});
    };
    for (const auto& r : decl.rowsets) {
        if (r.name == "all")
            bad(origin, "'all' is every record; name the row set otherwise");
        if (r.name == "data") {
            const auto& p = *r.pipeline;
            if (!p.sources.empty() || p.stages.size() != 1 ||
                !std::holds_alternative<duql::syntax::Where>(
                    p.stages.front().node))
                bad(origin, "'data' must be one 'where'");
        }
        add(dstd::String(r.name),
            dstd::String(r.name + " = " + duql::syntax::to_text(*r.pipeline)));
    }
    for (const auto& d : decl.defs) {
        dstd::String params;
        if (!d.params.empty()) {
            params = "(";
            for (std::size_t i = 0; i < d.params.size(); ++i) {
                if (i) params += ", ";
                params += d.params[i];
            }
            params += ")";
        }
        add(dstd::String(d.name),
            dstd::String(
                "def " + d.name + params + " = " +
                (std::holds_alternative<duql::syntax::PipelinePtr>(d.body)
                     ? duql::syntax::to_text(
                           *std::get<duql::syntax::PipelinePtr>(d.body))
                     : duql::syntax::to_text(
                           *std::get<duql::syntax::ExprPtr>(d.body)))));
    }
    return out;
}

}  // namespace

duql::Roles duql_roles(const RecordSchema& s) {
    duql::Roles r;
    r.time_ns_per_unit = ns_per(s.roles.time_unit);
    r.time = s.roles.time;
    r.duration = s.roles.duration;
    r.schema = s.id;
    if (const FieldSpec* t = s.field_at(s.roles.time))
        r.time_text = t->type == FieldType::String;
    auto add = [&](const dstd::String& key, std::int64_t ns) {
        if (!key.empty()) r.fields.emplace(key, ns);
    };
    add(s.roles.time, ns_per(s.roles.time_unit));
    add(s.roles.duration, ns_per(s.roles.duration_unit));
    for (const auto& f : s.fields) {
        if (f.role != Role::Time && f.role != Role::Duration) continue;
        const std::int64_t ns = ns_per(f.unit.value_or(TimeUnit::Us));
        add(f.name, ns);
        add(f.path, ns);
    }
    return r;
}

dstd::String merge_source(dstd::StringView parent, dstd::StringView child,
                          dstd::StringView origin) {
    dstd::Vector<Member> out = members(parent, origin);
    for (auto& m : members(child, origin)) {
        bool replaced = false;
        for (auto& p : out)
            if (p.name == m.name) {
                p = std::move(m);
                replaced = true;
                break;
            }
        if (!replaced) out.push_back(std::move(m));
    }
    dstd::String text;
    for (const auto& m : out) text += (text.empty() ? "" : ";\n") + m.text;
    return text;
}

void check_source(const RecordSchema& schema, dstd::StringView origin) {
    if (schema.source.empty()) return;
    const duql::Roles roles = duql_roles(schema);
    auto p = duql::compile_program("where true", {}, &roles, schema.source);
    if (!p)
        bad(origin,
            "schema " + dstd::String(schema.id) + ": " + p.error().format());
    if (schema.never_index.empty()) return;
    // A stored row set reads values from the decoded events, which a
    // listed path does not keep.
    for (const auto& r : indexed_rowsets(schema))
        for (const auto& path : schema.never_index) {
            const bool selected =
                std::any_of(r.columns.begin(), r.columns.end(),
                            [&](const auto& c) { return c.second == path; });
            if (selected || (r.filter && r.filter->references(path)))
                bad(origin, "schema " + schema.id + ": row set " + r.name +
                                " reads " + path + ", which is never_index");
        }
}

dstd::String data_condition(const RecordSchema& schema) {
    if (schema.source.empty()) return {};
    duql::syntax::Program tree = parse_members(schema.source, schema.id);
    for (const auto& r :
         std::get<duql::syntax::SourceDecl>(tree.decls.front()).rowsets)
        if (r.name == "data")
            return dstd::String(duql::syntax::to_text(
                *std::get<duql::syntax::Where>(r.pipeline->stages.front().node)
                     .condition));
    return {};
}

bool source_args_fallback(const RecordSchema& schema) {
    if (schema.source.empty()) return false;
    duql::syntax::Program tree = parse_members(schema.source, schema.id);
    return duql::args_fallback(
        std::get<duql::syntax::SourceDecl>(tree.decls.front()).defs);
}

dstd::Vector<IndexedRowSet> indexed_rowsets(const RecordSchema& schema) {
    dstd::Vector<IndexedRowSet> out;
    if (schema.source.empty()) return out;
    const duql::Roles roles = duql_roles(schema);
    auto program =
        duql::compile_program("where true", {}, &roles, schema.source);
    if (!program) return out;
    for (const auto& side : program->sides) {
        if (!side.rowset) continue;
        const duql::Pipeline& p = side.pipeline;
        if (p.input.kind != duql::InputKind::All) continue;
        bool reads_sides = false;
        duql::for_each_pipeline_term(p, [&](const duql::Term& t) {
            reads_sides =
                reads_sides || std::holds_alternative<duql::TLookup>(t.node);
        });
        if (reads_sides) continue;
        IndexedRowSet r;
        r.name = side.name;
        if (p.filter)
            r.filter =
                duql::detail::QueryAccess::from_node(duql::clone(*p.filter));
        bool plain = true;
        std::size_t stages = p.stages.size();
        if (stages > 0)
            if (const auto* d =
                    std::get_if<duql::PipelineDistinct>(&p.stages.back());
                d && d->items.empty()) {
                r.distinct = true;
                --stages;
            }
        if (!p.scan_select.empty()) {
            if (stages != 0) continue;
            for (const auto& path : p.scan_select)
                r.columns.emplace_back(path, path);
        } else if (stages == 1) {
            const auto* sel = std::get_if<duql::PipelineSelect>(&p.stages[0]);
            if (!sel) continue;
            for (const auto& it : sel->items) {
                const auto* f = std::get_if<duql::TField>(&it.term->node);
                if (!f || f->root != duql::FieldRoot::Record ||
                    f->neg_at != f->steps.size()) {
                    plain = false;
                    break;
                }
                for (const auto& st : f->steps)
                    if (st.index) plain = false;
                r.columns.emplace_back(it.name, f->base);
            }
        } else {
            continue;
        }
        if (plain && !r.columns.empty()) out.push_back(std::move(r));
    }
    return out;
}

}  // namespace dftracer::utils::index
