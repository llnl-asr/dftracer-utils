#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/macros.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/term.h>
#include <dftracer/utils/index/source.h>

#include <utility>
#include <vector>

namespace dftracer::utils::index {

namespace {

namespace syn = duql::syntax;

std::int64_t ns_per(TimeUnit unit) {
    switch (unit) {
        case TimeUnit::NS:
            return 1;
        case TimeUnit::US:
            return 1000;
        case TimeUnit::MS:
            return 1'000'000;
        case TimeUnit::S:
            return 1'000'000'000;
    }
    return 1000;
}

struct Member {
    std::string name;
    std::string text;
};

[[noreturn]] void bad(std::string_view origin, const std::string& why) {
    throw DFTUtilsException::cat(ErrorCode::INVALID_ARGUMENT, "source of ",
                                 origin, ": ", why);
}

syn::Program parse_members(std::string_view text, std::string_view origin) {
    auto tree = syn::parse("source s {\n" + std::string(text) + "\n}");
    if (!tree) bad(origin, tree.error().format());
    return std::move(*tree);
}

// The members of `text` in order, each as canonical text.
std::vector<Member> members(std::string_view text, std::string_view origin) {
    std::vector<Member> out;
    if (text.empty()) return out;
    syn::Program tree = parse_members(text, origin);
    const auto& decl = std::get<syn::SourceDecl>(tree.decls.front());
    auto add = [&](std::string name, std::string member) {
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
                !std::holds_alternative<syn::Where>(p.stages.front().node))
                bad(origin, "'data' must be one 'where'");
        }
        add(r.name, r.name + " = " + syn::to_text(*r.pipeline));
    }
    for (const auto& d : decl.defs) {
        std::string params;
        if (!d.params.empty()) {
            params = "(";
            for (std::size_t i = 0; i < d.params.size(); ++i)
                params += (i ? ", " : "") + d.params[i];
            params += ")";
        }
        add(d.name, "def " + d.name + params + " = " +
                        (std::holds_alternative<syn::PipelinePtr>(d.body)
                             ? syn::to_text(*std::get<syn::PipelinePtr>(d.body))
                             : syn::to_text(*std::get<syn::ExprPtr>(d.body))));
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
        r.time_text = t->type == FieldType::STRING;
    auto add = [&](const std::string& key, std::int64_t ns) {
        if (!key.empty()) r.fields.emplace(key, ns);
    };
    add(s.roles.time, ns_per(s.roles.time_unit));
    add(s.roles.duration, ns_per(s.roles.duration_unit));
    for (const auto& f : s.fields) {
        if (f.role != Role::TIME && f.role != Role::DURATION) continue;
        const std::int64_t ns = ns_per(f.unit.value_or(TimeUnit::US));
        add(f.name, ns);
        add(f.path, ns);
    }
    return r;
}

std::string merge_source(std::string_view parent, std::string_view child,
                         std::string_view origin) {
    std::vector<Member> out = members(parent, origin);
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
    std::string text;
    for (const auto& m : out) text += (text.empty() ? "" : ";\n") + m.text;
    return text;
}

void check_source(const RecordSchema& schema, std::string_view origin) {
    if (schema.source.empty()) return;
    const duql::Roles roles = duql_roles(schema);
    auto p = duql::compile_program("where true", {}, &roles, schema.source);
    if (!p) bad(origin, "schema " + schema.id + ": " + p.error().format());
}

std::string data_condition(const RecordSchema& schema) {
    if (schema.source.empty()) return {};
    syn::Program tree = parse_members(schema.source, schema.id);
    for (const auto& r : std::get<syn::SourceDecl>(tree.decls.front()).rowsets)
        if (r.name == "data")
            return syn::to_text(
                *std::get<syn::Where>(r.pipeline->stages.front().node)
                     .condition);
    return {};
}

bool source_args_fallback(const RecordSchema& schema) {
    if (schema.source.empty()) return false;
    syn::Program tree = parse_members(schema.source, schema.id);
    return duql::args_fallback(
        std::get<syn::SourceDecl>(tree.decls.front()).defs);
}

std::vector<IndexedRowSet> indexed_rowsets(const RecordSchema& schema) {
    std::vector<IndexedRowSet> out;
    if (schema.source.empty()) return out;
    const duql::Roles roles = duql_roles(schema);
    auto program =
        duql::compile_program("where true", {}, &roles, schema.source);
    if (!program) return out;
    for (const auto& side : program->sides) {
        if (!side.rowset) continue;
        const duql::Pipeline& p = side.pipeline;
        if (p.input.kind != duql::InputKind::ALL) continue;
        bool reads_sides = false;
        duql::for_each_pipeline_term(p, [&](const duql::Term& t) {
            reads_sides =
                reads_sides || std::holds_alternative<duql::TLookup>(t.node);
        });
        if (reads_sides) continue;
        IndexedRowSet r;
        r.name = side.name;
        if (p.filter) r.filter = duql::Query::from_node(duql::clone(*p.filter));
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
                if (!f || f->root != duql::FieldRoot::RECORD ||
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
