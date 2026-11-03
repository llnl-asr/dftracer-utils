// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/vectorize.h>
#include <dftracer/utils/index/plan/rowsets.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_duql_internal.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>

#include <algorithm>
#include <mutex>
#include <utility>

namespace dftracer::utils::trace::views {

namespace duql_detail {

Applier::Applier(View v, const index::RecordSchema& schema, const Ctx& ctx,
                 Reads reads, dstd::SharedPtr<const detail::BuildStep> step,
                 dstd::Vector<std::size_t> collected, bool main,
                 dstd::Optional<dstd::Vector<duql::VectorColumn>> frame)
    : collected_(std::move(collected)),
      main_(main),
      v_(std::move(v)),
      schema_(schema),
      roles_(ctx.roles),
      ctx_(ctx),
      step_(std::move(step)),
      fields_(std::move(reads.fields)),
      arrays_(std::move(reads.arrays)),
      closed_(reads.closed) {
    if (!frame) {
        columns_from_view(true);
        return;
    }
    cols_ = std::move(*frame);
    scan_ = false;
    blocker_ = "from";
}

void Applier::apply(const duql::PipelineStage& stage) {
    std::visit([this](const auto& s) { this->stage(s); }, stage);
    dstd::Vector<dstd::String> rest;
    for (const auto& c : cols_)
        if (!c.quant && !c.lookup && !c.call && !c.index && !c.list && !c.term)
            rest.push_back(c.name);
    if (rest.size() != cols_.size()) keep(std::move(rest));
}

void Applier::finish_columns() {
    dstd::Vector<dstd::String> rest;
    bool dropped = false;
    for (const auto& n : names()) {
        if (std::find(extras_.begin(), extras_.end(), n) != extras_.end())
            dropped = true;
        else
            rest.push_back(n);
    }
    if (dropped) keep(std::move(rest));
}

const dstd::Vector<SchemaLeaf>& Applier::tree() {
    if (!tree_) tree_ = detail::catalog_leaves(v_);
    return *tree_;
}

bool Applier::is_array(const dstd::String& field) const {
    return std::find(arrays_.begin(), arrays_.end(), field) != arrays_.end();
}

bool Applier::is_json(dstd::StringView name) const {
    for (const auto& spec : schema_.fields)
        if (spec.type == index::FieldType::Json &&
            (name == spec.name || name == spec.path ||
             (schema_.args_fallback && name.starts_with("args.") &&
              name.substr(5) == spec.name)))
            return true;
    return false;
}

dataframe::TypeId Applier::observed(dstd::StringView name) {
    if (!index_types_) {
        index_types_.emplace();
        for (const auto& l : tree())
            index_types_->emplace(l.path, observed_type(l.type));
    }
    const auto it = index_types_->find(dotted(name));
    return it == index_types_->end() ? dataframe::TypeId::Unknown : it->second;
}

// `p` as a View over its input: its build step, when it reads lookups, then
// its scan filter and projection, then its stages. With `table`, the scan
// ends, so the columns are the stages' own. The `main` plan owns the sides.
Built build(const Ctx& ctx, const duql::Pipeline& p, bool table,
            dstd::Vector<dstd::String>& lines, bool main, bool ordered) {
    View v = ctx.input(p.input);
    if (ordered && p.input.kind != duql::InputKind::Side) v = ctx.order(v);
    dstd::Optional<dstd::Vector<duql::VectorColumn>> side_cols;
    if (p.input.kind == duql::InputKind::Side) {
        const std::size_t i = p.input.side;
        side_cols = ctx.sides->columns[i];
        dataframe::Schema schema;
        for (const auto& c : *side_cols)
            schema.fields.push_back(
                dataframe::Field{dstd::String(c.name), c.type, true});
        v = v.with_lazy(side_source(main ? ctx.sides : nullptr, ctx.sides, i,
                                    std::move(schema)));
        lines.push_back(
            dstd::String("from " + ctx.sides->program.sides[i].name +
                         (ctx.sides->stored[i]
                              ? " (rows stored in the index)"
                              : " (its side runs when the query executes)")));
    }
    dstd::Vector<std::size_t> reads;
    term_reads(p, reads);
    dstd::Vector<std::size_t> sets;
    set_reads(p, sets);
    dstd::SharedPtr<detail::BuildStep> step;
    if (!reads.empty() || !sets.empty()) {
        struct Once {
            std::once_flag flag;
            Pushdown keys;
        };
        auto once = dstd::make_shared<Once>();
        step = dstd::make_shared<detail::BuildStep>();
        step->run = [once, strong = main ? ctx.sides : nullptr,
                     weak = dstd::WeakPtr<Sides>(ctx.sides),
                     pipeline = &p](dstd::Optional<duql::Query> query) {
            std::call_once(once->flag, [&] {
                once->keys =
                    (strong ? strong : owner(weak))->prepare(*pipeline);
            });
            return pushed(once->keys, std::move(query));
        };
        v = ctx.with_step(v, step);
        for (const auto i : reads)
            lines.push_back(dstd::String("reads " +
                                         ctx.sides->program.sides[i].name +
                                         " before the scan"));
        for (const auto i : sets)
            if (std::find(reads.begin(), reads.end(), i) == reads.end())
                lines.push_back(dstd::String("reads the distinct rows of " +
                                             ctx.sides->program.sides[i].name +
                                             " before the scan"));
    }
    if (p.filter) {
        lines.push_back(
            dstd::String("scan filter: " + p.filter_text + " (pushed)"));
        v = v.filter(duql::Query::from_node(duql::clone(*p.filter)));
    }
    each_pushed(p, [&](const duql::Term&, const duql::TField& f,
                       const duql::TLookup& l, const duql::Term*) {
        lines.push_back(dstd::String("scan filter: " + f.base +
                                     " in (keys of " + l.name + ") (pushed)"));
    });
    auto adds = [&](const duql::PipelineLookup& l) {
        dstd::Vector<dstd::String> out;
        if (const auto* into = std::get_if<duql::PipelineNest>(&l.mode)) {
            out.push_back(dstd::String(into->name));
            return out;
        }
        if (const auto* o = std::get_if<duql::PipelineOverlap>(&l.mode);
            o && !o->into.empty()) {
            out.push_back(dstd::String(o->into));
            return out;
        }
        if (l.kind == duql::syntax::LookupKind::Anti) return out;
        for (const auto& c : ctx.sides->columns[l.side]) out.push_back(c.name);
        return out;
    };
    Reads fields = referenced_fields(
        p.stages, adds,
        ctx.schema(v).decoder == dftracer::utils::index::Decoder::Path);
    if (!p.scan_select.empty()) {
        lines.push_back("scan select: " + joined(p.scan_select));
        dstd::Vector<dstd::String> select;
        dstd::Optional<dstd::Vector<SchemaLeaf>> tree;
        for (const auto& f : p.scan_select) {
            const bool array =
                std::find(fields.arrays.begin(), fields.arrays.end(),
                          dstd::String(f)) != fields.arrays.end();
            if (!array && p.stages.empty()) {
                select.push_back(dstd::String(f));
                continue;
            }
            if (!tree) tree = detail::catalog_leaves(v);
            for (auto& r :
                 scan_reads(*tree, dstd::String(f), array, fields.arrays,
                            ctx.schema(v).args_fallback))
                select.push_back(std::move(r));
        }
        v = v.select(std::move(select));
        fields.fields.clear();
    }
    if (p.stages.empty() && !table) {
        dstd::Vector<duql::VectorColumn> cols;
        for (const auto& f : v.output_schema().fields)
            cols.push_back({dstd::String(f.name), f.type});
        return {std::move(v), std::move(cols)};
    }
    const index::RecordSchema& schema = ctx.schema(v);
    Applier a(std::move(v), schema, ctx, std::move(fields), step, reads, main,
              std::move(side_cols));
    if (!p.scan_select.empty()) a.selected_at_scan();
    for (const auto& s : p.stages) a.apply(s);
    if (table) a.end_scan("union");
    a.finish_columns();
    for (auto& l : a.lines) lines.push_back(std::move(l));
    dstd::Vector<duql::VectorColumn> cols = a.columns();
    return {std::move(a).take(), std::move(cols)};
}

}  // namespace duql_detail

using namespace duql_detail;

View detail::view_of(detail::ScanPlan plan) { return View(std::move(plan)); }

View detail::with_named_columns(const View& v,
                                dstd::Vector<dataframe::Field> columns) {
    return v.reshape("select", [&](const detail::ScanPlan& p) {
        return scan::name_columns(p, std::move(columns));
    });
}

dstd::Pair<View, dstd::Vector<dstd::String>> View::duql_plan(
    dstd::StringView text, const duql::Params& params) const {
    const index::RecordSchema& schema = detail::plan_record_schema(*plan_);
    Ctx ctx;
    ctx.roles = index::duql_roles(schema);
    const duql::PluginCatalog plugins = registry_plugins();
    const duql::LeafPaths leaves = [this] {
        detail::scan::ensure_catalog(plan_);
        if (const auto file = detail::scan::file_without_catalog(plan_))
            refuse(
                "wildcard paths expand against the index's path catalog, "
                "which the index of " +
                *file +
                " does not hold; build the full index (dftracer_index or "
                "BatchIndexer) first");
        dstd::Vector<dstd::String> out;
        for (const auto& l : schema_tree()) out.emplace_back(l.path);
        return out;
    };
    auto p = duql::compile_program(text, params, &ctx.roles, schema.source,
                                   &plugins, &leaves);
    if (!p) refuse(p.error().format());
    reject_resolved(*p);
    ctx.sides = dstd::make_shared<Sides>(std::move(*p));
    ctx.order = [](const View& in) {
        return in.reshape("duql", [](const detail::ScanPlan& s) {
            return detail::scan::ordered(s);
        });
    };
    ctx.with_step = [](const View& in,
                       dstd::SharedPtr<const detail::BuildStep> step) {
        return in.reshape("duql", [&](const detail::ScanPlan& s) {
            auto next = dstd::make_shared<detail::ViewPlan>(*s);
            next->build_step = step;
            return detail::ScanPlan(std::move(next));
        });
    };
    ctx.input = [this](const duql::Input& in) -> View {
        switch (in.kind) {
            case duql::InputKind::Data:
                break;
            case duql::InputKind::All:
            case duql::InputKind::Side: {
                auto all = dstd::make_shared<detail::ViewPlan>();
                all->files = plan_->files;
                all->record_schema = plan_->record_schema;
                all->rollup_root = plan_->rollup_root;
                all->views_root = plan_->views_root;
                all->memory_budget = plan_->memory_budget;
                all->cancelled = plan_->cancelled;
                all->scan_counter = plan_->scan_counter;
                all->all_records = true;
                return detail::view_of(std::move(all));
            }
            case duql::InputKind::File: {
                const dstd::String index =
                    internal::determine_index_path(dstd::String(in.path), "");
                return View::from_file(
                    dstd::String(in.path),
                    dstd::String(index.data(), index.size()));
            }
        }
        return *this;
    };
    ctx.schema = [](const View& v) -> const index::RecordSchema& {
        return detail::plan_record_schema(*v.plan_);
    };
    Sides& sides = *ctx.sides;
    dstd::Vector<bool> needed(sides.program.sides.size(), false);
    dstd::Vector<std::size_t> reads;
    side_reads(sides.program.main, reads);
    for (const auto i : reads) needed[i] = true;
    for (std::size_t i = needed.size(); i-- > 0;) {
        if (!needed[i]) continue;
        dstd::Vector<std::size_t> more;
        side_reads(sides.program.sides[i].pipeline, more);
        for (const auto j : more) needed[j] = true;
    }
    dstd::Vector<std::size_t> asof;
    asof_reads(sides.program.main, asof);
    for (std::size_t i = 0; i < needed.size(); ++i)
        if (needed[i]) asof_reads(sides.program.sides[i].pipeline, asof);
    dstd::Vector<std::size_t> tables;
    dstd::Vector<std::size_t> sets;
    term_reads(sides.program.main, tables);
    set_reads(sides.program.main, sets);
    for (std::size_t i = 0; i < needed.size(); ++i)
        if (needed[i]) {
            term_reads(sides.program.sides[i].pipeline, tables);
            set_reads(sides.program.sides[i].pipeline, sets);
        }
    dstd::Vector<dstd::String> lines;
    for (std::size_t i = 0; i < needed.size(); ++i) {
        if (!needed[i]) continue;
        const duql::Side& side = sides.program.sides[i];
        dstd::Vector<dstd::String> side_lines;
        Built b = build(ctx, side.pipeline, false, side_lines, false,
                        std::find(asof.begin(), asof.end(), i) != asof.end());
        sides.keys[i] =
            cache_key(*b.view.plan_, side.pipeline.input.kind, side.key);
        if (side.rowset) {
            for (const auto& f : plan_->files)
                sides.files[i].push_back(
                    {dstd::String(dstd::StringView(f.file_path)),
                     f.index_path.empty()
                         ? internal::determine_index_path(f.file_path, "")
                         : dstd::String(dstd::StringView(f.index_path))});
            sides.stored[i] = index::plan::has_stored_rowsets(sides.files[i]);
        }
        sides.views[i] = std::move(b.view);
        sides.columns[i] = std::move(b.cols);
        // A column whose type only the data gives (a dftracer arg) takes it
        // from the side's rows: all of them when a table reads them anyway,
        // its distinct rows for a key set, else the first TYPE_PROBE_ROWS, so
        // a joined side stays uncapped.
        if (std::any_of(sides.columns[i].begin(), sides.columns[i].end(),
                        [](const duql::VectorColumn& c) {
                            return c.type.id == dataframe::TypeId::Unknown;
                        })) {
            const bool table =
                std::find(tables.begin(), tables.end(), i) != tables.end();
            const bool set =
                std::find(sets.begin(), sets.end(), i) != sets.end();
            const auto frame = table ? sides.frame(i)
                               : set ? sides.key_frame(i)
                                     : sides.probe(i);
            for (auto& c : sides.columns[i])
                for (std::size_t k = 0; k < frame->names.size(); ++k)
                    if (c.type.id == dataframe::TypeId::Unknown &&
                        dstd::StringView(frame->names[k]) == c.name)
                        c.type = frame->columns[k].data_type();
        }
        lines.push_back(dstd::String("side " + side.name + ": from ") +
                        input_text(side.pipeline.input, sides.program) + "; " +
                        (sides.stored[i] ? dstd::String("stored in the index")
                                         : cache_state(sides.keys[i])));
        if (!sides.stored[i])
            for (auto& l : side_lines) lines.push_back("  " + std::move(l));
    }
    Built main = build(ctx, sides.program.main, false, lines, true);
    return {std::move(main.view), std::move(lines)};
}

View View::duql(dstd::StringView text, const duql::Params& params) const {
    return duql_plan(text, params).first;
}

dstd::String View::explain_duql(dstd::StringView text,
                                const duql::Params& params) const {
    dstd::String out;
    for (const auto& l : duql_plan(text, params).second) {
        out += l;
        out += '\n';
    }
    return out;
}

}  // namespace dftracer::utils::trace::views
