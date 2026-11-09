// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/calendar.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/scope_exit.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/decorrelate.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/lower.h>
#include <dftracer/utils/duql/macros.h>
#include <dftracer/utils/duql/pattern.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/syntax/lexer.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/syntax/walk.h>
#include <dftracer/utils/duql/term.h>
#include <dftracer/utils/duql/wildcard.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

namespace dftracer::utils::duql {

namespace {

using namespace syntax;

using duql::LiteralNode;
using duql::MatchOp;
using duql::detail::QueryNodePtr;

struct AggInfo {
    duql::AggFn fn;
    dstd::StringView name;
    std::uint8_t min_args;
    std::uint8_t max_args;
};

constexpr AggInfo AGGREGATES[] = {
    {duql::AggFn::Count, "count", 0, 1},
    {duql::AggFn::CountIf, "count_if", 1, 1},
    {duql::AggFn::Sum, "sum", 1, 1},
    {duql::AggFn::Min, "min", 1, 1},
    {duql::AggFn::Max, "max", 1, 1},
    {duql::AggFn::Mean, "mean", 1, 1},
    {duql::AggFn::Var, "var", 1, 1},
    {duql::AggFn::Std, "std", 1, 1},
    {duql::AggFn::First, "first", 1, 1},
    {duql::AggFn::Last, "last", 1, 1},
    {duql::AggFn::Quantile, "quantile", 2, 2},
    {duql::AggFn::Histogram, "histogram", 1, 1},
    {duql::AggFn::Busy, "busy", 0, 0},
    {duql::AggFn::Concurrency, "concurrency", 0, 0},
    {duql::AggFn::Utilization, "utilization", 0, 0},
    {duql::AggFn::Active, "active", 0, 0},
    {duql::AggFn::CountDistinct, "count_distinct", 1, 1},
    {duql::AggFn::Collect, "collect", 1, 1},
    {duql::AggFn::Argmax, "arg_max", 2, 2},
    {duql::AggFn::Argmin, "arg_min", 2, 2},
    {duql::AggFn::Sketch, "sketch", 1, 1},
    {duql::AggFn::Merge, "merge", 1, 1},
};

struct WinInfo {
    duql::WinFn fn;
    dstd::StringView name;
    std::uint8_t min_args;
    std::uint8_t max_args;
    bool window_only;
};

constexpr WinInfo WINDOW_FUNCTIONS[] = {
    {duql::WinFn::RowNumber, "row_number", 0, 0, true},
    {duql::WinFn::Rank, "rank", 0, 0, true},
    {duql::WinFn::DenseRank, "dense_rank", 0, 0, true},
    {duql::WinFn::Lag, "lag", 1, 3, true},
    {duql::WinFn::Lead, "lead", 1, 3, true},
    {duql::WinFn::RunningSum, "running_sum", 1, 1, true},
    {duql::WinFn::RunningCount, "running_count", 0, 0, true},
    {duql::WinFn::RunningMin, "running_min", 1, 1, true},
    {duql::WinFn::RunningMax, "running_max", 1, 1, true},
    {duql::WinFn::RunningMean, "running_mean", 1, 1, true},
    {duql::WinFn::Ntile, "ntile", 1, 1, true},
    {duql::WinFn::Nth, "nth", 2, 2, true},
    {duql::WinFn::PercentRank, "percent_rank", 0, 0, true},
    {duql::WinFn::CumeDist, "cume_dist", 0, 0, true},
    {duql::WinFn::FillForward, "fill_forward", 1, 1, true},
    {duql::WinFn::Count, "count", 0, 1, false},
    {duql::WinFn::Sum, "sum", 1, 1, false},
    {duql::WinFn::Min, "min", 1, 1, false},
    {duql::WinFn::Max, "max", 1, 1, false},
    {duql::WinFn::Mean, "mean", 1, 1, false},
    {duql::WinFn::First, "first", 1, 1, false},
    {duql::WinFn::Last, "last", 1, 1, false},
    {duql::WinFn::Var, "var", 1, 1, false},
    {duql::WinFn::Std, "std", 1, 1, false},
    {duql::WinFn::Quantile, "quantile", 2, 2, false},
    {duql::WinFn::Histogram, "histogram", 1, 1, false},
    {duql::WinFn::CountIf, "count_if", 1, 1, false},
    {duql::WinFn::CountDistinct, "count_distinct", 1, 1, false},
    {duql::WinFn::Collect, "collect", 1, 1, false},
    {duql::WinFn::Argmax, "arg_max", 2, 2, false},
    {duql::WinFn::Argmin, "arg_min", 2, 2, false},
};

const WinInfo* window_info(dstd::StringView name) {
    for (const auto& w : WINDOW_FUNCTIONS)
        if (w.name == name) return &w;
    return nullptr;
}

bool listed(dstd::StringView name, const auto& names) {
    for (const auto& n : names)
        if (n == name) return true;
    return false;
}

const AggInfo* aggregate_info(dstd::StringView name) {
    for (const auto& a : AGGREGATES)
        if (a.name == name) return &a;
    return nullptr;
}

bool builtin_name(dstd::StringView name) {
    for (const auto& f : duql::FUNCTIONS)
        if (f.name == name) return true;
    return aggregate_info(name) || window_info(name) || name == "any" ||
           name == "all" || name == "bin" || name == "as_time" ||
           name == "to_seconds" || name == "now";
}

class Lowering {
    template <class T>
    using Result = dstd::Expected<T, duql::DuqlError>;
    using Fail = dstd::Unexpected<duql::DuqlError>;

    // One step of a flattened pipeline: a stage, or a union with a source.
    struct Step {
        const Stage* stage = nullptr;
        const From* source = nullptr;
    };

   public:
    Lowering(const duql::Params& params, dstd::StringView source,
             const duql::Roles* roles = nullptr,
             const duql::PluginCatalog* plugins = nullptr)
        : params_(params), src_(source), roles_(roles), plugins_(plugins) {}

    Result<detail::QueryNodePtr> filter(const syntax::Program& q) {
        view_ = false;
        if (!q.decls.empty())
            return fail(Span{}, declaration_message(q.decls.front()));
        if (!q.pipeline) return fail(Span{}, "Expected a filter");
        if (!q.pipeline->sources.empty())
            return fail(Span{}, "'from' runs only on a View pipeline");
        const syntax::Pipeline& p = *q.pipeline;
        if (p.stages.empty()) return fail(Span{}, "Expected a filter");
        for (std::size_t i = 0; i < p.stages.size(); ++i) {
            const Stage& s = p.stages[i];
            if (i == 0 && std::holds_alternative<Where>(s.node)) continue;
            return fail(s.span, "'" + stage_name(s) +
                                    "' is a pipeline stage; this takes a "
                                    "filter");
        }
        return condition(*std::get<Where>(p.stages.front().node).condition);
    }

    // `rowsets` are the source's, which the query's lets follow.
    Result<duql::Program> program(const syntax::Program& q,
                                  const dstd::Vector<Let>& rowsets) {
        dstd::Vector<const Let*> all;
        for (const auto& r : rowsets) {
            if (r.name == "data") continue;
            names_.push_back(r.name);
            rowsets_.push_back(r.name);
            key_text_ += "source " + r.name + " = " +
                         syntax::to_text(*r.pipeline) + ";\n";
            all.push_back(&r);
        }
        for (const auto& d : q.decls) {
            const auto* let = std::get_if<Let>(&d);
            if (!let) return fail(Span{}, declaration_message(d));
            if (let->name == "all" || let->name == "data")
                return fail(Span{}, "'" + let->name +
                                        "' names a built-in row set; give the "
                                        "let another name");
            if (listed(let->name, rowsets_))
                return fail(Span{}, "'" + let->name +
                                        "' names a row set of the source; "
                                        "give the let another name");
            if (listed(let->name, names_))
                return fail(Span{}, "'" + let->name + "' is declared twice");
            names_.push_back(let->name);
            key_text_ += "let " + let->name + " = " +
                         syntax::to_text(*let->pipeline) + ";\n";
            all.push_back(let);
        }
        dstd::Vector<dstd::String> bound;
        for (const auto& [name, value] : params_) {
            dstd::String text;
            if (const auto* list =
                    std::get_if<dstd::Vector<duql::LiteralValue>>(&value)) {
                for (const auto& v : *list)
                    text += (text.empty() ? "" : ", ") +
                            duql::term_text(*make_term(constant(v)));
                text = "[" + text + "]";
            } else {
                text = duql::term_text(
                    *make_term(constant(std::get<duql::LiteralValue>(value))));
            }
            bound.push_back(dstd::String(name) + "=" + text + ";\n");
        }
        std::sort(bound.begin(), bound.end());
        for (const auto& b : bound) key_text_ += b;
        for (const Let* let : all) {
            current_ = let->name;
            // A source's row sets read every record unless they say `from`;
            // a query's lets, like the query, read `data`.
            const bool rowset = listed(let->name, rowsets_);
            const auto side =
                add_side(let->name, *let->pipeline, false,
                         rowset ? duql::InputKind::All : duql::InputKind::Data);
            if (!side) return dstd::Unexpected(side.error());
            prog_.sides[*side].rowset = rowset;
            lets_.push_back({let, *side});
        }
        current_.clear();
        if (!q.pipeline) return fail(Span{}, "Expected a pipeline");
        duql::Input in;
        dstd::Vector<Step> steps;
        const auto flat =
            flatten(*q.pipeline, duql::InputKind::Data, in, steps);
        if (!flat) return dstd::Unexpected(flat.error());
        auto main = lower_steps(std::move(in), steps);
        if (!main) return dstd::Unexpected(main.error());
        prog_.main = std::move(*main);
        return std::move(prog_);
    }

    // Whether `n` reads an ISO-8601 time role, which the raw record holds as
    // text.
    bool reads_text_time(const duql::detail::QueryNode& n) const {
        if (!roles_ || !roles_->time_text) return false;
        if (duql::collect_fields(n).contains(roles_->time)) return true;
        return std::visit(
            [&](const auto& x) -> bool {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, duql::detail::AndNode> ||
                              std::is_same_v<T, duql::detail::OrNode>)
                    return reads_text_time(*x.left) ||
                           reads_text_time(*x.right);
                else if constexpr (std::is_same_v<T, duql::detail::NotNode>)
                    return reads_text_time(*x.operand);
                else if constexpr (std::is_same_v<T, duql::detail::ExprLeaf>) {
                    bool found = false;
                    duql::for_each_term_field(
                        *x.term, [&](const duql::TField& f) {
                            found = found || f.base == roles_->time;
                        });
                    return found;
                } else
                    return false;
            },
            n.data);
    }

    // One pipeline over `in`: `steps` in order, each a stage or a union
    // with a source.
    Result<duql::Pipeline> lower_steps(duql::Input in,
                                       const dstd::Vector<Step>& steps) const {
        auto units = std::move(col_units_);
        const bool record = record_units_;
        col_units_.clear();
        record_units_ = true;
        auto unit_scope = dstd::defer([&] {
            col_units_ = std::move(units);
            record_units_ = record;
        });
        duql::Pipeline out;
        out.input = std::move(in);
        // Rows of a row set are no scan: every stage runs on them.
        const bool scan = out.input.kind != duql::InputKind::Side;
        std::size_t i = 0;
        // A term that joins a sub-query's side runs after the scan; the
        // other terms of the leading `where`s filter the scan, a top-level
        // `k in (...)` from the side's distinct rows.
        dstd::Vector<duql::PipelineStage> joined;
        for (; scan && i < steps.size(); ++i) {
            const auto* w = steps[i].stage
                                ? std::get_if<Where>(&steps[i].stage->node)
                                : nullptr;
            if (!w) break;
            dstd::Vector<const Expr*> terms;
            conjuncts(*w->condition, terms);
            detail::QueryNodePtr node;
            dstd::Vector<const Expr*> after;
            for (const Expr* t : terms) {
                const auto* semi = std::get_if<In>(&t->node);
                const bool set =
                    semi && semi->subquery && !semi->negated && joins(*t);
                if (joins(*t) && !set) {
                    after.push_back(t);
                    continue;
                }
                key_set_ = set;
                auto n = condition(*t);
                key_set_ = false;
                if (!n) return dstd::Unexpected(n.error());
                node = node ? duql::detail::make_node(duql::detail::AndNode{
                                  std::move(node), std::move(*n)})
                            : std::move(*n);
            }
            if (node && reads_text_time(*node)) break;
            for (const Expr* t : after) {
                auto where = where_stage(*t);
                if (!where) return dstd::Unexpected(where.error());
                joined.push_back(std::move(*where));
            }
            if (!node) continue;
            out.filter = out.filter
                             ? duql::detail::make_node(duql::detail::AndNode{
                                   std::move(out.filter), std::move(node)})
                             : std::move(node);
        }
        if (out.filter) out.filter_text = duql::detail::to_string(*out.filter);
        const bool after_scan = !joined.empty();
        for (auto& st : joined) out.stages.push_back(std::move(st));
        if (scan && !after_scan && i < steps.size() && steps[i].stage)
            if (const auto* sel = std::get_if<Select>(&steps[i].stage->node);
                sel && plain_paths(*sel)) {
                for (const auto& it : sel->items) {
                    auto text = path_text(std::get<Path>(it.value->node),
                                          it.value->span);
                    if (!text) return dstd::Unexpected(text.error());
                    out.scan_select.push_back(std::move(*text));
                }
                ++i;
            }
        const bool leading =
            i == steps.size() || !steps[i].stage ||
            !std::holds_alternative<Select>(steps[i].stage->node);
        dstd::Optional<Span> bucket;
        bool open_pivot = false;
        bool called = false;
        for (std::size_t first = i; i < steps.size(); ++i) {
            if (called)
                return fail(steps[i].stage ? steps[i].stage->span : Span{},
                            "a stage after 'call' is not allowed: 'call' ends "
                            "the pipeline");
            if (!steps[i].stage) {
                if (open_pivot)
                    return fail(Span{},
                                "a stage after 'pivot' needs its columns: list "
                                "the values with 'pivot k in [...]'");
                auto other = union_of(*steps[i].source);
                if (!other) return dstd::Unexpected(other.error());
                out.stages.push_back(std::move(*other));
                continue;
            }
            const Stage& s = *steps[i].stage;
            if (open_pivot)
                return fail(s.span,
                            "a stage after 'pivot' needs its columns: list the "
                            "values with 'pivot k in [...]'");
            if (const auto* pv = std::get_if<Pivot>(&s.node))
                open_pivot = pv->values.empty();
            if (std::holds_alternative<CallTree>(s.node) &&
                (i != first || !leading))
                return fail(s.span, "'call_tree' follows only 'where' stages");
            if (std::holds_alternative<Bucket>(s.node)) {
                if (bucket)
                    return fail(s.span,
                                "a second 'bucket' before a 'group' or 'agg'");
                bucket = s.span;
            }
            if (std::holds_alternative<Group>(s.node) ||
                std::holds_alternative<Agg>(s.node))
                bucket.reset();
            if (const auto* t = std::get_if<Take>(&s.node);
                t && !t->last.empty()) {
                const auto from = count(t->count, s.span);
                if (!from) return dstd::Unexpected(from.error());
                const auto to = count(t->last, s.span);
                if (!to) return dstd::Unexpected(to.error());
                if (*from < 1 || *to < *from)
                    return fail(s.span, "'take a..b' needs 1 <= a <= b");
                out.stages.push_back(duql::PipelineSkip{*from - 1});
                out.stages.push_back(duql::PipelineTake{*to - *from + 1});
                continue;
            }
            called = std::holds_alternative<CallStage>(s.node);
            auto lowered = stage(s);
            if (!lowered) return dstd::Unexpected(lowered.error());
            for (auto& b : before_) out.stages.push_back(std::move(b));
            before_.clear();
            out.stages.push_back(std::move(*lowered));
            for (auto& a : after_) out.stages.push_back(std::move(a));
            after_.clear();
            const auto tracked = track_units(s);
            if (!tracked) return dstd::Unexpected(tracked.error());
        }
        if (bucket)
            return fail(*bucket, "'bucket' needs a 'group' or 'agg' after it");
        return out;
    }

    Result<dstd::Optional<LiteralNode>> value_of(const Expr& e) const {
        if (std::holds_alternative<Param>(e.node))
            return dstd::Optional<LiteralNode>{};
        return literal(e);
    }

   private:
    const duql::Params& params_;
    mutable dstd::StringView src_;
    // Units of the columns the stages lowered so far made; see column_unit.
    mutable dstd::Map<dstd::String, dstd::Optional<std::int64_t>, std::less<>>
        col_units_;
    mutable bool record_units_ = true;
    const duql::Roles* roles_;
    mutable dstd::Optional<std::int64_t> now_;
    const duql::PluginCatalog* plugins_;
    // While a window entry lowers: its calls, and whether a call's argument
    // is lowering.
    mutable dstd::Vector<duql::PipelineWinCall>* win_ = nullptr;
    mutable bool win_arg_ = false;
    mutable const dstd::Vector<SortKey>* win_order_ = nullptr;
    // While a group entry that is an expression lowers: the group's
    // aggregates, the name its first aggregate takes, and the hidden columns.
    mutable dstd::Vector<duql::PipelineAgg>* agg_ = nullptr;
    mutable dstd::String agg_name_;
    mutable dstd::Vector<dstd::String> agg_hidden_;
    // Stages a group lowering adds after itself.
    mutable dstd::Vector<duql::PipelineStage> after_;
    // While a derive or select entry lowers: where its plugin calls add their
    // stages, which run before the entry's stage, and the hidden columns.
    mutable dstd::Vector<duql::PipelineStage>* plug_ = nullptr;
    mutable dstd::Vector<duql::PipelineStage> before_;
    mutable dstd::Vector<dstd::String> plug_hidden_;
    mutable std::size_t plug_count_ = 0;
    mutable int side_depth_ = 0;
    // Quantifier conditions being lowered.
    mutable int quant_ = 0;
    // Whether a condition lowers, where an arrow compared with a value holds
    // when any matching row makes it hold.
    mutable bool cond_ = false;
    // Whether the `in` lowering next is a top-level term of the scan filter.
    mutable bool key_set_ = false;
    // Sub-queries being lowered.
    mutable int sub_ = 0;
    mutable int lookup_side_ = 0;
    mutable int lookups_ = 0;
    // False when lowering a filter, which has no other row sets to read.
    bool view_ = true;
    mutable duql::Program prog_;
    mutable std::size_t subs_ = 0;

    // The lets declared so far, each with its side; every let's name; the let
    // being lowered; and the declarations and parameters every side's cache
    // key starts with.
    struct LetSide {
        const Let* let = nullptr;
        std::size_t side = 0;
    };
    dstd::Vector<LetSide> lets_;
    dstd::Vector<dstd::String> names_;
    dstd::Vector<dstd::String> rowsets_;
    dstd::String current_;
    dstd::String key_text_;

    struct Flag {
        bool& flag;
        bool old;
        Flag(bool& f, bool v) : flag(f), old(f) { flag = v; }
        ~Flag() { flag = old; }
    };

    static dstd::String declaration_message(const Decl& d) {
        if (std::holds_alternative<Let>(d))
            return "'let' runs only on a View pipeline";
        if (std::holds_alternative<Def>(d))
            return "'def' runs only in a query, a source or a macro file";
        return "a 'source' belongs to a record schema; declare it in the "
               "schema's 'source'";
    }

    Result<void> need_view(Span span, const dstd::String& what) const {
        if (!view_)
            return fail(span, what +
                                  " reads other rows, which only a View "
                                  "pipeline has; run it with View::duql");
        return {};
    }

    // `p` as its input and steps: a `from` naming a let reads that let's
    // input and runs its stages first; each later source is a union step.
    Result<void> flatten(const syntax::Pipeline& p, duql::InputKind first,
                         duql::Input& in, dstd::Vector<Step>& steps) const {
        if (p.sources.empty()) {
            in = {first, {}};
        } else {
            const auto r = read(p.sources.front(), in, steps);
            if (!r) return dstd::Unexpected(r.error());
        }
        for (std::size_t i = 1; i < p.sources.size(); ++i)
            steps.push_back({nullptr, &p.sources[i]});
        for (const auto& st : p.stages) steps.push_back({&st, nullptr});
        return {};
    }

    Result<void> read(const From& f, duql::Input& in,
                      dstd::Vector<Step>& steps) const {
        if (const auto* prm = std::get_if<Param>(&f.name)) {
            const auto v = bound(*prm, Span{});
            if (!v) return dstd::Unexpected(v.error());
            const auto* path = std::get_if<dstd::String>(&*v);
            if (!path)
                return fail(Span{}, "'from $" + prm->name +
                                        "' reads a file: bind it to a path "
                                        "string");
            in = {duql::InputKind::File, *path};
            return {};
        }
        const auto& name = std::get<dstd::String>(f.name);
        if (f.quoted) {
            in = {duql::InputKind::File, name};
        } else if (name == "all") {
            in = {duql::InputKind::All, {}};
        } else if (name == "data") {
            in = {duql::InputKind::Data, {}};
        } else if (listed(name, rowsets_)) {
            const auto let = let_of(name, Span{});
            if (!let) return dstd::Unexpected(let.error());
            in = {duql::InputKind::Side, {}, let->side};
        } else {
            const auto let = let_of(name, Span{});
            if (!let) return dstd::Unexpected(let.error());
            return flatten(*let->let->pipeline, duql::InputKind::Data, in,
                           steps);
        }
        return {};
    }

    // The let `name`, declared before this point.
    Result<LetSide> let_of(const dstd::String& name, Span span) const {
        for (const auto& l : lets_)
            if (l.let->name == name) return l;
        if (name == current_)
            return fail(span, "'" + name + "' refers to itself");
        if (listed(name, names_))
            return fail(span, "'" + name +
                                  "' is declared after its use; declare a let "
                                  "before what reads it");
        return fail(span,
                    "'" + name + "' is not a let or a row set of the source");
    }

    Result<duql::PipelineUnion> union_of(const From& f) const {
        duql::Input in;
        dstd::Vector<Step> steps;
        const auto r = read(f, in, steps);
        if (!r) return dstd::Unexpected(r.error());
        auto lowered = lower_steps(std::move(in), steps);
        if (!lowered) return dstd::Unexpected(lowered.error());
        return duql::PipelineUnion{
            dstd::make_shared<const duql::Pipeline>(std::move(*lowered))};
    }

    Result<duql::PipelineUnion> union_of(const syntax::Pipeline& p) const {
        duql::Input in;
        dstd::Vector<Step> steps;
        const auto r = flatten(p, duql::InputKind::Data, in, steps);
        if (!r) return dstd::Unexpected(r.error());
        auto lowered = lower_steps(std::move(in), steps);
        if (!lowered) return dstd::Unexpected(lowered.error());
        return duql::PipelineUnion{
            dstd::make_shared<const duql::Pipeline>(std::move(*lowered))};
    }

    // Lowers `p` as the row set `name`, reading `first` unless it says
    // `from`.
    Result<std::size_t> add_side(dstd::String name, const syntax::Pipeline& p,
                                 bool sub, duql::InputKind first) const {
        auto* win = win_;
        auto* agg = agg_;
        const bool win_arg = win_arg_;
        const int quant = quant_;
        const bool cond = cond_;
        auto* plug = plug_;
        auto before = std::move(before_);
        before_.clear();
        plug_ = nullptr;
        ++side_depth_;
        win_ = nullptr;
        agg_ = nullptr;
        win_arg_ = false;
        quant_ = 0;
        cond_ = false;
        if (sub) ++sub_;
        auto restore = dstd::defer([&] {
            if (sub) --sub_;
            plug_ = plug;
            before_ = std::move(before);
            --side_depth_;
            win_ = win;
            agg_ = agg;
            win_arg_ = win_arg;
            quant_ = quant;
            cond_ = cond;
        });
        duql::Input in;
        dstd::Vector<Step> steps;
        const auto flat = flatten(p, first, in, steps);
        if (!flat) return dstd::Unexpected(flat.error());
        auto lowered = lower_steps(std::move(in), steps);
        if (!lowered) return dstd::Unexpected(lowered.error());
        dstd::String key = key_text_ + (sub ? "sub " : "let " + name + " ") +
                           syntax::to_text(p);
        prog_.sides.push_back(
            {std::move(name), std::move(*lowered), std::move(key)});
        return prog_.sides.size() - 1;
    }

    // The number of columns a side gives, when its last stage fixes them.
    dstd::Optional<std::size_t> width(std::size_t side) const {
        const duql::Pipeline& p = prog_.sides[side].pipeline;
        if (p.stages.empty()) {
            if (p.scan_select.empty()) return dstd::nullopt;
            return p.scan_select.size();
        }
        const auto& last = p.stages.back();
        if (const auto* sel = std::get_if<duql::PipelineSelect>(&last))
            return sel->items.size();
        if (const auto* g = std::get_if<duql::PipelineGroup>(&last)) {
            const bool bucket = p.stages.size() > 1 &&
                                std::holds_alternative<duql::PipelineBucket>(
                                    p.stages[p.stages.size() - 2]);
            return g->keys.size() + g->aggs.size() + (bucket ? 1 : 0);
        }
        return dstd::nullopt;
    }

    dstd::SharedPtr<duql::LookupSlot> slot() const {
        return dstd::make_shared<duql::LookupSlot>();
    }

    // The key terms of `e`: the items of a tuple, else `e`.
    Result<dstd::Vector<duql::TermPtr>> key_terms(const Expr& e) const {
        dstd::Vector<duql::TermPtr> out;
        if (const auto* tup = std::get_if<Tuple>(&e.node)) {
            for (const auto& it : tup->items) {
                auto t = term(*it);
                if (!t) return dstd::Unexpected(t.error());
                out.push_back(std::move(*t));
            }
        } else {
            auto t = term(e);
            if (!t) return dstd::Unexpected(t.error());
            out.push_back(std::move(*t));
        }
        dstd::Optional<duql::DuqlError> error;
        for (const auto& k : out) {
            duql::for_each_term(*k, [&](const duql::Term& x) {
                if (error) return;
                if (const auto* f = std::get_if<duql::TField>(&x.node);
                    f && f->root != duql::FieldRoot::Record)
                    error = error_at(e.span,
                                     "a lookup key inside any() or all() "
                                     "reads the record, not '.' or '^.'");
            });
            if (error) return dstd::Unexpected(std::move(*error));
        }
        return out;
    }

    // The side a sub-query reads: a let's own when it is only `from let`.
    Result<void> side_of(const syntax::Pipeline& p, duql::TLookup& t) const {
        if (p.stages.empty() && p.sources.size() == 1)
            if (const auto* name =
                    std::get_if<dstd::String>(&p.sources[0].name);
                name && !p.sources[0].quoted && *name != "all" &&
                *name != "data") {
                t.name = *name;
                const auto let = let_of(*name, Span{});
                if (!let) return dstd::Unexpected(let.error());
                t.side = let->side;
                return {};
            }
        t.name = "__sub_" + dstd::int_text(subs_++);
        const auto side = add_side(t.name, p, true, duql::InputKind::Data);
        if (!side) return dstd::Unexpected(side.error());
        t.side = *side;
        return {};
    }

    // Lowers the correlated sub-query `p` into `t`: its side, and after the
    // keys `t` has the keys of the enclosing row. False when `p` is not
    // correlated.
    Result<bool> correlated(const syntax::Pipeline& p, bool scalar,
                            duql::TLookup& t) const {
        auto d = duql::decorrelate(p, scalar);
        if (!d.has_value()) {
            const duql::Refusal& r = d.error();
            return dstd::Unexpected(
                make_error(r.text, r.span.offset, r.span.length, r.message));
        }
        if (!*d) return false;
        const std::size_t subject = t.keys.size();
        const dstd::StringView outer = src_;
        src_ = (*d)->text;
        auto restore = dstd::defer([&] { src_ = outer; });
        auto outer_key = [&](const Expr& e) -> Result<void> {
            auto key = term(e);
            if (!key) return dstd::Unexpected(key.error());
            t.keys.push_back(std::move(*key));
            dstd::Optional<duql::DuqlError> error;
            duql::for_each_term(*t.keys.back(), [&](const duql::Term& x) {
                if (error) return;
                if (const auto* f = std::get_if<duql::TField>(&x.node);
                    f && f->root != duql::FieldRoot::Record)
                    error = error_at(e.span,
                                     "a correlated key inside any() or all() "
                                     "reads the record, not '.' or '^.'");
            });
            if (error) return dstd::Unexpected(std::move(*error));
            return {};
        };
        for (const auto& e : (*d)->outer) {
            const auto key = outer_key(*e);
            if (!key) return dstd::Unexpected(key.error());
        }
        if ((*d)->low) {
            const auto key = outer_key(*(*d)->low->outer);
            if (!key) return dstd::Unexpected(key.error());
            t.low = (*d)->low->open ? duql::TermOp::Gt : duql::TermOp::Ge;
        }
        if ((*d)->high) {
            const auto key = outer_key(*(*d)->high->outer);
            if (!key) return dstd::Unexpected(key.error());
            t.high = (*d)->high->open ? duql::TermOp::Lt : duql::TermOp::Le;
        }
        t.name = "__sub_" + dstd::int_text(subs_++);
        const auto side =
            add_side(t.name, *(*d)->side, true, duql::InputKind::Data);
        if (!side) return dstd::Unexpected(side.error());
        t.side = *side;
        if ((*d)->empty) {
            const auto empty =
                add_side("__sub_" + dstd::int_text(subs_++), *(*d)->empty, true,
                         duql::InputKind::Data);
            if (!empty) return dstd::Unexpected(empty.error());
            t.empty = *empty;
        }
        t.target = std::move((*d)->names);
        t.correlated = t.keys.size() - subject;
        t.range = (*d)->read;
        return true;
    }

    Result<duql::TLookup> semi_join(const In& n, Span span) const {
        const auto view = need_view(span, "a sub-query");
        if (!view) return dstd::Unexpected(view.error());
        duql::TLookup t;
        t.kind = duql::LookupKind::In;
        t.negated = n.negated;
        t.key_set = std::exchange(key_set_, false);
        auto keys = key_terms(*n.subject);
        if (!keys) return dstd::Unexpected(keys.error());
        t.keys = std::move(*keys);
        const std::size_t subject = t.keys.size();
        const auto joined = correlated(*n.subquery, false, t);
        if (!joined.has_value()) return dstd::Unexpected(joined.error());
        if (!*joined) {
            const auto side = side_of(*n.subquery, t);
            if (!side) return dstd::Unexpected(side.error());
        }
        const std::size_t added =
            t.target.size() + (t.range != duql::RangeRead::None ? 1 : 0);
        if (const auto w = width(t.side); w && *w != subject + added)
            return fail(
                span, "the sub-query gives " + dstd::int_text(*w - added) +
                          " columns; 'in' compares " + dstd::int_text(subject));
        t.slot = slot();
        return t;
    }

    Result<duql::TLookup> scalar(const Subquery& n, Span span) const {
        const auto view = need_view(span, "a sub-query");
        if (!view) return dstd::Unexpected(view.error());
        duql::TLookup t;
        t.kind = duql::LookupKind::Scalar;
        const auto joined = correlated(*n.pipeline, true, t);
        if (!joined.has_value()) return dstd::Unexpected(joined.error());
        if (*joined) {
            t.column = duql::CORRELATED_VALUE;
            t.slot = slot();
            return t;
        }
        const auto side = side_of(*n.pipeline, t);
        if (!side) return dstd::Unexpected(side.error());
        if (const auto w = width(t.side); w && *w != 1)
            return fail(span,
                        "a sub-query in an expression gives one column; this "
                        "one gives " +
                            dstd::int_text(*w));
        t.slot = slot();
        return t;
    }

    // `k -> s(id).path`; with `all`, every matching row's value.
    Result<duql::TLookup> arrow(const Arrow& a, Span span, bool all) const {
        const auto view = need_view(span, "'->'");
        if (!view) return dstd::Unexpected(view.error());
        duql::TLookup t;
        t.kind = duql::LookupKind::Arrow;
        t.all = all;
        t.name = a.rowset;
        const auto let = let_of(a.rowset, span);
        if (!let) return dstd::Unexpected(let.error());
        t.side = let->side;
        const auto* tup = std::get_if<Tuple>(&a.key->node);
        if (a.target_key) {
            if (tup)
                return fail(span,
                            "a tuple key finds the row set's columns by its "
                            "paths; drop '(...)' after '" +
                                a.rowset + "'");
            auto target = path_text(*a.target_key, span);
            if (!target) return dstd::Unexpected(target.error());
            t.target.push_back(std::move(*target));
        }
        auto name_of = [&](const Expr& k) -> Result<dstd::String> {
            if (const auto* p = std::get_if<Path>(&k.node))
                return path_text(*p, k.span);
            if (const auto* inner = std::get_if<Arrow>(&k.node))
                return path_text(inner->path, k.span);
            return fail(k.span, "name the key column of '" + a.rowset +
                                    "' for this key: write 'k -> " + a.rowset +
                                    "(id).path'");
        };
        auto keys = key_terms(*a.key);
        if (!keys) return dstd::Unexpected(keys.error());
        t.keys = std::move(*keys);
        if (tup) {
            for (const auto& it : tup->items) {
                auto name = name_of(*it);
                if (!name) return dstd::Unexpected(name.error());
                t.target.push_back(std::move(*name));
            }
        } else if (!a.target_key) {
            auto name = name_of(*a.key);
            if (!name) return dstd::Unexpected(name.error());
            t.target.push_back(std::move(*name));
        }
        auto column = path_text(a.path, span);
        if (!column) return dstd::Unexpected(column.error());
        t.column = std::move(*column);
        t.slot = slot();
        return t;
    }

    // In a condition, `subject op ...` with an arrow subject as a
    // quantifier over the matching rows' values; `cond` reads the value as
    // `.`.
    template <class Cond>
    Result<dstd::Optional<duql::TermPtr>> any_match(const Expr& subject,
                                                    Cond&& cond) const {
        const auto* a = std::get_if<Arrow>(&subject.node);
        if (!cond_ || !a) return dstd::Optional<duql::TermPtr>{};
        auto rows = arrow(*a, subject.span, true);
        if (!rows) return dstd::Unexpected(rows.error());
        auto held = cond();
        if (!held) return dstd::Unexpected(held.error());
        return dstd::Optional<duql::TermPtr>(make_term(duql::TQuant{
            false, make_term(std::move(*rows)), std::move(*held)}));
    }

    static bool plain_paths(const Select& sel) {
        for (const auto& it : sel.items) {
            const auto* path = std::get_if<Path>(&it.value->node);
            if (!path || !it.name.empty() ||
                negative_at(*path) != path->steps.size())
                return false;
        }
        return !sel.items.empty();
    }

    Result<duql::PipelineItem> item(dstd::String name,
                                    const Expr& value) const {
        auto t = term(value);
        if (!t) return dstd::Unexpected(t.error());
        dstd::String text = duql::term_text(**t);
        if (name.empty()) {
            const auto* path = std::get_if<Path>(&value.node);
            if (!path)
                return fail(value.span,
                            "name this expression: write 'name = expr' or "
                            "'expr as name'");
            auto named = path_text(*path, value.span);
            if (!named) return dstd::Unexpected(named.error());
            name = std::move(*named);
        }
        return duql::PipelineItem{std::move(name),
                                  dstd::to_shared_ptr(std::move(*t)),
                                  std::move(text)};
    }

    // The name an entry without one takes: empty for a path, else the
    // term's text.
    Result<dstd::String> entry_name(const Expr& e) const {
        if (std::holds_alternative<Path>(e.node)) return dstd::String{};
        auto t = term(e);
        if (!t) return dstd::Unexpected(t.error());
        return duql::term_text(**t);
    }

    Result<duql::PipelineItem> plugin_item(
        dstd::String name, const Expr& value,
        dstd::Vector<duql::PipelineStage>& sink) const {
        const auto prev = plug_;
        plug_ = side_depth_ == 0 ? &sink : nullptr;
        auto scope = dstd::defer([&] { plug_ = prev; });
        return item(std::move(name), value);
    }

    Result<Derive> parse_derive(const Parse& n) const {
        const Expr& at = *n.pattern;
        const auto regex_text = pattern(at);
        if (!regex_text) return dstd::Unexpected(regex_text.error());
        const auto compiled =
            compile(MatchOp::Regex, *regex_text, dstd::nullopt, at);
        if (!compiled) return dstd::Unexpected(compiled.error());
        Derive out;
        for (const auto& [group, name] : duql::capture_names(**compiled)) {
            Call c;
            c.name = "extract";
            const auto arg = [&](ExprNode node) {
                auto e = dstd::make_unique<Expr>();
                e->node = std::move(node);
                e->span = at.span;
                c.args.push_back(Arg{{}, std::move(e)});
            };
            arg(n.column);
            if (const auto* lit = std::get_if<Literal>(&at.node))
                arg(*lit);
            else
                arg(std::get<Param>(at.node));
            arg(Literal{static_cast<std::uint64_t>(group), {}});
            auto value = dstd::make_unique<Expr>();
            value->node = std::move(c);
            value->span = at.span;
            out.fields.push_back(Assign{name, std::move(value)});
        }
        if (out.fields.empty())
            return fail(at.span,
                        "'parse' needs a named group like (?<name>...)");
        return out;
    }

    Result<duql::PipelineStage> derive_stage(const Derive& n) const {
        dstd::Vector<dstd::Vector<duql::PipelineStage>> pre(n.fields.size());
        dstd::Vector<duql::PipelineItem> items;
        plug_hidden_.clear();
        bool any = false;
        for (std::size_t k = 0; k < n.fields.size(); ++k) {
            auto built =
                plugin_item(n.fields[k].name, *n.fields[k].value, pre[k]);
            if (!built) return dstd::Unexpected(built.error());
            items.push_back(std::move(*built));
            any = any || !pre[k].empty();
        }
        if (!any) return duql::PipelineDerive{std::move(items)};
        for (std::size_t k = 0; k < items.size(); ++k) {
            for (auto& st : pre[k]) before_.push_back(std::move(st));
            duql::PipelineDerive one;
            one.items.push_back(std::move(items[k]));
            before_.push_back(std::move(one));
        }
        return duql::PipelineDrop{std::move(plug_hidden_)};
    }

    duql::PluginKind plugin_kind(const dstd::String& name) const {
        return plugins_ && plugins_->kind ? plugins_->kind(name)
                                          : duql::PluginKind::None;
    }

    dstd::String unknown_function(const dstd::String& name) const {
        dstd::String out = "Unknown function '" + name + "'; ";
        if (!plugins_ || plugins_->namespaces.empty())
            return out + "no plugin is loaded";
        out += "loaded plugin namespaces: ";
        for (std::size_t i = 0; i < plugins_->namespaces.size(); ++i)
            out += (i ? ", " : "") + plugins_->namespaces[i];
        return out;
    }

    Result<duql::PipelineCall> call_stage(const Call& c, Span span) const {
        const duql::PluginKind kind = plugin_kind(c.name);
        if (kind == duql::PluginKind::None)
            return fail(span, unknown_function(c.name));
        if (kind != duql::PluginKind::Table)
            return fail(span, "'" + c.name +
                                  "' is not a table function; 'call' runs a "
                                  "table to table op");
        duql::PipelineCall out;
        out.op = c.name;
        out.text = text_of(span);
        for (const auto& a : c.args) {
            if (!a.name.empty())
                return fail(a.value->span, "'" + c.name +
                                               "' takes no named argument '" +
                                               a.name + "'");
            const auto v = literal(*a.value);
            if (!v) return dstd::Unexpected(v.error());
            if (!*v)
                return fail(a.value->span,
                            "an argument of 'call' is a literal or a "
                            "parameter");
            out.args.push_back((*v)->value);
        }
        return out;
    }

    Result<duql::TermPtr> plugin_call(const Call& c, Span span) const {
        const duql::PluginKind kind = plugin_kind(c.name);
        if (kind == duql::PluginKind::None)
            return fail(span, unknown_function(c.name));
        if (kind != duql::PluginKind::Column)
            return fail(span, "'" + c.name +
                                  "' is not a column function; a table "
                                  "function runs as 'call " +
                                  c.name + "(...)'");
        if (!plug_)
            return fail(
                span,
                "'" + c.name + "' is a plugin function; it stands in " +
                    (side_depth_ > 0 ? "a 'derive' or 'select' of the main "
                                       "pipeline, not in a 'let' or a sub-query"
                                     : "a 'derive' or 'select' entry"));
        if (c.args.empty())
            return fail(
                span, "'" + c.name + "' takes a column as its first argument");
        for (const auto& a : c.args) {
            if (!a.name.empty())
                return fail(a.value->span, "'" + c.name +
                                               "' takes no named argument '" +
                                               a.name + "'");
            if (std::holds_alternative<Duration>(a.value->node))
                return fail(a.value->span,
                            "'" + c.name + "' takes no duration argument");
        }
        auto* sink = plug_;
        plug_ = nullptr;
        duql::PipelinePlugin st;
        st.op = c.name;
        st.text = text_of(span);
        const dstd::String name = "__duql_p_" + dstd::int_text(plug_count_++);
        auto first = term(*c.args[0].value);
        if (!first) return dstd::Unexpected(first.error());
        st.arg = {name, dstd::to_shared_ptr(std::move(*first)),
                  text_of(*c.args[0].value)};
        plug_hidden_.push_back(name);
        for (std::size_t k = 1; k < c.args.size(); ++k) {
            const Expr& e = *c.args[k].value;
            const auto v = literal(e);
            if (!v) return dstd::Unexpected(v.error());
            if (*v && std::holds_alternative<dstd::String>((*v)->value)) {
                if (st.str)
                    return fail(e.span,
                                "'" + c.name + "' takes one text argument");
                st.str = std::get<dstd::String>((*v)->value);
            } else if (*v) {
                if (st.scalars.size() == 2)
                    return fail(e.span, "'" + c.name +
                                            "' takes at most 2 number "
                                            "arguments");
                st.scalars.push_back((*v)->value);
            } else {
                if (st.arg2)
                    return fail(e.span, "'" + c.name +
                                            "' takes one second column "
                                            "argument");
                const dstd::String second = name + "_b";
                auto other = term(e);
                if (!other) return dstd::Unexpected(other.error());
                st.arg2 = duql::PipelineItem{
                    second, dstd::to_shared_ptr(std::move(*other)), text_of(e)};
                plug_hidden_.push_back(second);
            }
        }
        plug_ = sink;
        sink->push_back(std::move(st));
        duql::TField f;
        f.base = name;
        f.steps.push_back({name, false, dstd::nullopt});
        f.neg_at = 1;
        return make_term(std::move(f));
    }

    Result<std::int64_t> count(const dstd::String& text, Span span) const {
        if (text.starts_with('$')) {
            const auto v = bound(Param{text.substr(1)}, span);
            if (!v) return dstd::Unexpected(v.error());
            if (const auto* i = std::get_if<std::int64_t>(&*v); i && *i >= 0)
                return *i;
            if (const auto* u = std::get_if<std::uint64_t>(&*v);
                u && *u <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()))
                return static_cast<std::int64_t>(*u);
            return fail(span, "a row count must be a non-negative integer");
        }
        std::int64_t n = 0;
        const auto r =
            dstd::from_chars(text.data(), text.data() + text.size(), n);
        if (r.ec != std::errc{} || n < 0)
            return fail(span, "a row count must be a non-negative integer");
        return n;
    }

    Result<duql::PipelineWhere> where_stage(const Expr& condition) const {
        const Flag in_condition(cond_, true);
        auto t = term(condition);
        if (!t) return dstd::Unexpected(t.error());
        dstd::String text = duql::term_text(**t);
        return duql::PipelineWhere{dstd::to_shared_ptr(std::move(*t)),
                                   std::move(text)};
    }

    static void conjuncts(const Expr& e, dstd::Vector<const Expr*>& out) {
        if (const auto* b = std::get_if<Binary>(&e.node);
            b && b->op == BinaryOp::And) {
            conjuncts(*b->left, out);
            conjuncts(*b->right, out);
            return;
        }
        out.push_back(&e);
    }

    // Whether `e` holds a sub-query whose side it reads through a join: one
    // under `in`, or one correlated by `==` keys alone. A sub-query that
    // does not decorrelate fails later, when it lowers.
    static bool joins(const Expr& e) {
        const auto* in = std::get_if<In>(&e.node);
        const auto* s = std::get_if<Subquery>(&e.node);
        if ((in && in->subquery) || s) {
            const auto d = duql::decorrelate(in ? *in->subquery : *s->pipeline,
                                             s != nullptr);
            if (!d.has_value()) return false;
            const auto& side = *d;
            return (in || side) && !(side && (side->low || side->high));
        }
        bool out = false;
        syntax::children(const_cast<Expr&>(e),
                         [&](ExprPtr& c) { out = out || joins(*c); });
        return out;
    }

    Result<duql::PipelineStage> stage(const Stage& s) const {
        return std::visit(
            [&](const auto& n) -> Result<duql::PipelineStage> {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, Where>) {
                    auto where = where_stage(*n.condition);
                    if (!where) return dstd::Unexpected(where.error());
                    return std::move(*where);
                } else if constexpr (std::is_same_v<T, Select>) {
                    duql::PipelineSelect out;
                    for (const auto& it : n.items) {
                        auto built = plugin_item(it.name, *it.value, before_);
                        if (!built) return dstd::Unexpected(built.error());
                        out.items.push_back(std::move(*built));
                    }
                    return out;
                } else if constexpr (std::is_same_v<T, Derive>) {
                    return derive_stage(n);
                } else if constexpr (std::is_same_v<T, Parse>) {
                    const auto derived = parse_derive(n);
                    if (!derived) return dstd::Unexpected(derived.error());
                    return derive_stage(*derived);
                } else if constexpr (std::is_same_v<T, Drop>) {
                    duql::PipelineDrop out;
                    for (const auto& path : n.paths) {
                        auto name = path_text(path, s.span);
                        if (!name) return dstd::Unexpected(name.error());
                        out.names.push_back(std::move(*name));
                    }
                    return out;
                } else if constexpr (std::is_same_v<T, Rename>) {
                    duql::PipelineRename out;
                    for (const auto& [name, path] : n.pairs) {
                        auto from = path_text(path, s.span);
                        if (!from) return dstd::Unexpected(from.error());
                        out.pairs.emplace_back(name, std::move(*from));
                    }
                    return out;
                } else if constexpr (std::is_same_v<T, Distinct>) {
                    duql::PipelineDistinct out;
                    for (const auto& k : n.keys) {
                        auto name = k.name.empty()
                                        ? entry_name(*k.value)
                                        : Result<dstd::String>(k.name);
                        if (!name) return dstd::Unexpected(name.error());
                        auto built = item(std::move(*name), *k.value);
                        if (!built) return dstd::Unexpected(built.error());
                        out.items.push_back(std::move(*built));
                    }
                    return out;
                } else if constexpr (std::is_same_v<T, syntax::Sort>) {
                    duql::PipelineSort out;
                    for (std::size_t k = 0; k < n.keys.size(); ++k) {
                        const auto& key = n.keys[k];
                        auto name = entry_name(*key.value);
                        if (!name) return dstd::Unexpected(name.error());
                        auto built = item(std::move(*name), *key.value);
                        if (!built) return dstd::Unexpected(built.error());
                        out.keys.push_back({std::move(*built), key.descending,
                                            key.nulls == NullsOrder::First});
                    }
                    return out;
                } else if constexpr (std::is_same_v<T, Take>) {
                    const auto rows = count(n.count, s.span);
                    if (!rows) return dstd::Unexpected(rows.error());
                    if (n.by.empty()) return duql::PipelineTake{*rows};
                    duql::PipelineTakeBy out;
                    out.count = *rows;
                    for (const auto& k : n.by) {
                        auto built = key_item(*k);
                        if (!built) return dstd::Unexpected(built.error());
                        out.keys.push_back(std::move(*built));
                    }
                    for (const auto& key : n.order) {
                        auto built = key_item(*key.value);
                        if (!built) return dstd::Unexpected(built.error());
                        out.order.push_back({std::move(*built), key.descending,
                                             key.nulls == NullsOrder::First});
                    }
                    return out;
                } else if constexpr (std::is_same_v<T, Skip>) {
                    const auto rows = count(n.count, s.span);
                    if (!rows) return dstd::Unexpected(rows.error());
                    return duql::PipelineSkip{*rows};
                } else if constexpr (std::is_same_v<T, Sample>) {
                    auto sampled = sample(n, s.span);
                    if (!sampled) return dstd::Unexpected(sampled.error());
                    return std::move(*sampled);
                } else if constexpr (std::is_same_v<T, Group>) {
                    duql::PipelineGroup out;
                    for (const auto& k : n.keys) {
                        auto built = item(k.name, *k.value);
                        if (!built) return dstd::Unexpected(built.error());
                        out.keys.push_back(std::move(*built));
                    }
                    auto aggs = aggregates(n.aggregates);
                    if (!aggs) return dstd::Unexpected(aggs.error());
                    out.aggs = std::move(*aggs);
                    return out;
                } else if constexpr (std::is_same_v<T, Agg>) {
                    duql::PipelineGroup out;
                    auto aggs = aggregates(n.aggregates);
                    if (!aggs) return dstd::Unexpected(aggs.error());
                    out.aggs = std::move(*aggs);
                    return out;
                } else if constexpr (std::is_same_v<T, TimeRange>) {
                    const auto role =
                        require_role(s.span, "time_range", true, n.overlap);
                    if (!role) return dstd::Unexpected(role.error());
                    constexpr double INF =
                        std::numeric_limits<double>::infinity();
                    double low = -INF;
                    double high = INF;
                    if (n.low) {
                        const auto v = time_value(*n.low);
                        if (!v) return dstd::Unexpected(v.error());
                        low = *v;
                    }
                    if (n.high) {
                        const auto v = time_value(*n.high);
                        if (!v) return dstd::Unexpected(v.error());
                        high = *v;
                    }
                    duql::PipelineTimeRange out{
                        low, high, n.overlap, nullptr, {}};
                    if (!(out.low <= out.high))
                        return fail(s.span, "'time_range' needs low <= high");
                    const auto ts = [&] { return role_field(roles_->time); };
                    duql::TermPtr lower;
                    if (n.low)
                        lower = n.overlap
                                    ? make_term(duql::TBinary{
                                          duql::TermOp::Gt,
                                          make_term(duql::TBinary{
                                              duql::TermOp::Add, ts(),
                                              role_field(roles_->duration)}),
                                          number_term(out.low)})
                                    : make_term(
                                          duql::TBinary{duql::TermOp::Ge, ts(),
                                                        number_term(out.low)});
                    duql::TermPtr upper;
                    if (n.high)
                        upper = make_term(duql::TBinary{duql::TermOp::Lt, ts(),
                                                        number_term(out.high)});
                    out.condition = dstd::to_shared_ptr(
                        lower && upper
                            ? make_term(duql::TBinary{duql::TermOp::And,
                                                      std::move(lower),
                                                      std::move(upper)})
                            : std::move(lower ? lower : upper));
                    out.text = duql::term_text(*out.condition);
                    return out;
                } else if constexpr (std::is_same_v<T, Bucket>) {
                    const auto role =
                        require_role(s.span, "bucket", true, false);
                    if (!role) return dstd::Unexpected(role.error());
                    const auto width_value = time_value(*n.width);
                    if (!width_value)
                        return dstd::Unexpected(width_value.error());
                    const double width = *width_value;
                    if (!(width > 0))
                        return fail(n.width->span,
                                    "a bucket width must be positive");
                    dstd::Optional<double> every;
                    if (n.every) {
                        const auto step = time_value(*n.every);
                        if (!step) return dstd::Unexpected(step.error());
                        every = *step;
                        if (!(*every > 0))
                            return fail(n.every->span,
                                        "a bucket step ('every') must be "
                                        "positive");
                    }
                    double origin = 0.0;
                    if (n.at) {
                        const auto at = time_value(*n.at);
                        if (!at) return dstd::Unexpected(at.error());
                        origin = *at;
                    }
                    duql::TermPtr time = role_field(roles_->time);
                    if (origin != 0)
                        time = make_term(duql::TBinary{duql::TermOp::Sub,
                                                       std::move(time),
                                                       number_term(origin)});
                    duql::TermPtr key = make_term(duql::TBinary{
                        duql::TermOp::Mul,
                        make_term(duql::TBinary{duql::TermOp::Idiv,
                                                std::move(time),
                                                number_term(width)}),
                        number_term(width)});
                    if (origin != 0)
                        key = make_term(duql::TBinary{duql::TermOp::Add,
                                                      std::move(key),
                                                      number_term(origin)});
                    dstd::String text = duql::term_text(*key);
                    duql::PipelineBucket out{
                        width,
                        {},
                        {},
                        n.fill_mode,
                        n.fill,
                        dstd::to_shared_ptr(std::move(key)),
                        std::move(text),
                        n.as.empty() ? dstd::String("bucket") : n.as,
                        every,
                        origin};
                    if (n.low) {
                        const auto low = time_value(*n.low);
                        if (!low) return dstd::Unexpected(low.error());
                        out.low = *low;
                        const auto high = time_value(*n.high);
                        if (!high) return dstd::Unexpected(high.error());
                        out.high = *high;
                        if (!(*out.low <= *out.high))
                            return fail(s.span,
                                        "'fill from lo to hi' needs lo <= hi");
                    }
                    return out;
                } else if constexpr (std::is_same_v<T, CallTree>) {
                    const auto role =
                        require_role(s.span, "call_tree", true, true);
                    if (!role) return dstd::Unexpected(role.error());
                    return duql::PipelineCallTree{};
                } else if constexpr (std::is_same_v<T, Session>) {
                    auto built = session(n, s.span);
                    if (!built) return dstd::Unexpected(built.error());
                    return std::move(*built);
                } else if constexpr (std::is_same_v<T, Window>) {
                    auto built = window(n);
                    if (!built) return dstd::Unexpected(built.error());
                    return std::move(*built);
                } else if constexpr (std::is_same_v<T, Expand>) {
                    if (negative_at(n.path) != n.path.steps.size())
                        return fail(s.span,
                                    "'expand' takes a path without a negative "
                                    "index");
                    duql::PipelineExpand out;
                    auto path = path_text(n.path, s.span);
                    if (!path) return dstd::Unexpected(path.error());
                    out.path = std::move(*path);
                    out.name = n.as;
                    for (const auto& step : n.path.steps)
                        if (n.as.empty() && !step.key.empty())
                            out.name = step.key;
                    out.index = n.with_index;
                    out.keep_empty = n.keep_empty;
                    if (out.name == out.index)
                        return fail(s.span,
                                    "'expand' names the element and its index "
                                    "'" +
                                        out.name + "'");
                    return out;
                } else if constexpr (std::is_same_v<T, Pivot>) {
                    duql::PipelinePivot out;
                    auto key = key_item(*n.key);
                    if (!key) return dstd::Unexpected(key.error());
                    out.key = std::move(*key);
                    out.fixed = !n.values.empty();
                    for (const auto& v : n.values) {
                        const auto* lit = std::get_if<Literal>(&v->node);
                        if (lit && std::holds_alternative<Null>(lit->value)) {
                            out.values.push_back({duql::TNull{}});
                            continue;
                        }
                        const auto value = literal(*v);
                        if (!value) return dstd::Unexpected(value.error());
                        if (!*value)
                            return fail(v->span,
                                        "a pivot value is a literal or a "
                                        "parameter");
                        out.values.push_back(constant((*value)->value));
                    }
                    out.labels = n.labels;
                    auto aggs = aggregates(n.aggregates, "pivot");
                    if (!aggs) return dstd::Unexpected(aggs.error());
                    out.aggs = std::move(*aggs);
                    return out;
                } else if constexpr (std::is_same_v<T, Unpivot>) {
                    duql::PipelineUnpivot out;
                    for (const auto& path : n.paths) {
                        auto field = path_text(path, s.span);
                        if (!field) return dstd::Unexpected(field.error());
                        out.fields.push_back(std::move(*field));
                    }
                    out.key = n.key_name;
                    out.value = n.value_name;
                    if (out.key == out.value)
                        return fail(s.span,
                                    "'unpivot' names its key and value '" +
                                        out.key + "'");
                    return out;
                } else if constexpr (std::is_same_v<T, Lookup>) {
                    auto looked =
                        lookup(n.rowset, n.side.get(), n.keys, s.span);
                    if (!looked) return dstd::Unexpected(looked.error());
                    duql::PipelineLookup out = std::move(*looked);
                    out.kind = n.kind;
                    if (!n.into.empty()) out.mode = duql::PipelineNest{n.into};
                    return out;
                } else if constexpr (std::is_same_v<T, OverlapLookup>) {
                    const auto role =
                        require_role(s.span, "lookup ... overlap", true, true);
                    if (!role) return dstd::Unexpected(role.error());
                    auto looked =
                        lookup(n.rowset, n.side.get(), n.keys, s.span);
                    if (!looked) return dstd::Unexpected(looked.error());
                    duql::PipelineLookup out = std::move(*looked);
                    duql::PipelineOverlap o;
                    o.time = role_item(roles_->time);
                    o.duration = role_item(roles_->duration);
                    o.time_column = roles_->time;
                    o.duration_column = roles_->duration;
                    if (const auto d = roles_->fields.find(roles_->duration);
                        d != roles_->fields.end())
                        o.scale = static_cast<double>(d->second) /
                                  static_cast<double>(roles_->time_ns_per_unit);
                    o.into = n.into;
                    out.mode = std::move(o);
                    return out;
                } else if constexpr (std::is_same_v<T, AsofLookup>) {
                    auto looked =
                        lookup(n.rowset, n.side.get(), n.keys, s.span);
                    if (!looked) return dstd::Unexpected(looked.error());
                    duql::PipelineLookup out = std::move(*looked);
                    duql::PipelineAsof asof;
                    const auto& [t, c] = n.time;
                    auto column = key_column(
                        *t, c.get(), n.rowset.empty() ? "(from ...)" : n.rowset,
                        "asof");
                    if (!column) return dstd::Unexpected(column.error());
                    asof.column = std::move(*column);
                    auto time = key_item(*t);
                    if (!time) return dstd::Unexpected(time.error());
                    asof.time = std::move(*time);
                    asof.direction = n.direction;
                    if (n.within) {
                        const auto tol = tolerance(*n.within, *t);
                        if (!tol) return dstd::Unexpected(tol.error());
                        asof.tolerance = *tol;
                    }
                    out.mode = std::move(asof);
                    return out;
                } else if constexpr (std::is_same_v<T, Union>) {
                    auto other = union_of(*n.other);
                    if (!other) return dstd::Unexpected(other.error());
                    return std::move(*other);
                } else if constexpr (std::is_same_v<T, CallStage>) {
                    auto call = call_stage(n.call, s.span);
                    if (!call) return dstd::Unexpected(call.error());
                    return std::move(*call);
                } else if constexpr (std::is_same_v<T, Use>) {
                    return fail(s.span, "Unknown stage or pipeline macro '" +
                                            n.call.name + "'");
                } else {
                    static_assert(sizeof(T) == 0, "unhandled stage");
                }
            },
            s.node);
    }

    // The side's column a key or time pairs with: `named` when the key gives
    // one, else the row's own path.
    Result<dstd::String> key_column(const Expr& k, const Expr* k2,
                                    const dstd::String& rowset,
                                    const char* clause) const {
        const Expr& named = k2 ? *k2 : k;
        const auto* p = std::get_if<Path>(&named.node);
        if (!p)
            return fail(named.span,
                        k2 ? dstd::String("'") + clause +
                                 " k == c' names a column of the row set with c"
                           : "name the row set's " + dstd::String(clause) +
                                 " column: 'lookup " + rowset + " ... " +
                                 clause + " expr == column'");
        return path_text(*p, named.span);
    }

    Result<duql::PipelineLookup> lookup(
        const dstd::String& rowset, const syntax::Pipeline* inline_side,
        const dstd::Vector<dstd::Pair<ExprPtr, ExprPtr>>& keys,
        Span span) const {
        duql::PipelineLookup out;
        dstd::String shown = rowset;
        if (inline_side) {
            shown = "(from ...)";
            out.name = "__lookup_" + dstd::int_text(lookups_++);
            ++lookup_side_;
            auto restore = dstd::defer([&] { --lookup_side_; });
            const auto side =
                add_side(out.name, *inline_side, true, duql::InputKind::Data);
            if (!side) return dstd::Unexpected(side.error());
            out.side = *side;
        } else {
            const auto let = let_of(rowset, span);
            if (!let) return dstd::Unexpected(let.error());
            out.side = let->side;
            out.name = rowset;
        }
        for (const auto& [k, k2] : keys) {
            auto key = key_item(*k);
            if (!key) return dstd::Unexpected(key.error());
            auto column = key_column(*k, k2.get(), shown, "on");
            if (!column) return dstd::Unexpected(column.error());
            out.keys.emplace_back(std::move(*key), std::move(*column));
        }
        return out;
    }

    Result<duql::PipelineItem> key_item(const Expr& e) const {
        auto name = entry_name(e);
        if (!name) return dstd::Unexpected(name.error());
        return item(std::move(*name), e);
    }

    Result<duql::PipelineSample> sample(const Sample& n, Span span) const {
        duql::PipelineSample out;
        out.percent = n.percent;
        if (n.amount.starts_with('$')) {
            const auto param = bound(Param{n.amount.substr(1)}, span);
            if (!param) return dstd::Unexpected(param.error());
            const auto v = number(*param);
            if (!v)
                return fail(span, "a sample size parameter must be a number");
            out.amount = *v;
        } else {
            const auto r = dstd::from_chars_double(
                n.amount.data(), n.amount.data() + n.amount.size(), out.amount);
            if (r.ec != std::errc{}) return fail(span, "Invalid sample size");
        }
        if (n.percent) {
            if (!(out.amount >= 0 && out.amount <= 100))
                return fail(span, "a sample percentage must be in [0, 100]");
        } else if (std::trunc(out.amount) != out.amount) {
            return fail(span, "a sample row count must be an integer");
        }
        if (n.seed.starts_with('$')) {
            const auto seed = count(n.seed, span);
            if (!seed) return dstd::Unexpected(seed.error());
            out.seed = static_cast<decltype(out.seed)>(*seed);
        } else if (!n.seed.empty() &&
                   dstd::from_chars(n.seed.data(),
                                    n.seed.data() + n.seed.size(), out.seed)
                           .ec != std::errc{}) {
            return fail(span, "Invalid seed");
        }
        return out;
    }

    Result<duql::PipelineSession> session(const Session& n, Span span) const {
        const auto role = require_role(span, "session", true, false);
        if (!role) return dstd::Unexpected(role.error());
        duql::PipelineSession out;
        for (const auto& k : n.keys) {
            auto built = item(k.name, *k.value);
            if (!built) return dstd::Unexpected(built.error());
            out.keys.push_back(std::move(*built));
        }
        const auto gap = time_value(*n.gap);
        if (!gap) return dstd::Unexpected(gap.error());
        out.gap = *gap;
        if (!(out.gap >= 0))
            return fail(n.gap->span, "a session gap is not negative");
        if (n.max) {
            const auto longest = time_value(*n.max);
            if (!longest) return dstd::Unexpected(longest.error());
            out.span = *longest;
            if (!(out.span > 0))
                return fail(n.max->span, "a session 'max' must be positive");
        }
        out.time = dstd::to_shared_ptr(role_field(roles_->time));
        if (!roles_->duration.empty()) {
            duql::TermPtr dur = role_field(roles_->duration);
            if (const auto d = roles_->fields.find(roles_->duration);
                d != roles_->fields.end() &&
                d->second != roles_->time_ns_per_unit)
                dur = make_term(duql::TBinary{
                    duql::TermOp::Mul, std::move(dur),
                    number_term(
                        static_cast<double>(d->second) /
                        static_cast<double>(roles_->time_ns_per_unit))});
            out.end = dstd::to_shared_ptr(make_term(duql::TBinary{
                duql::TermOp::Add, role_field(roles_->time), std::move(dur)}));
        }
        out.name = n.as.empty() ? dstd::String("session") : n.as;
        out.text = duql::term_text(*out.time) +
                   (out.end ? " end " + duql::term_text(*out.end) : "");
        return out;
    }

    Result<void> require_role(Span span, dstd::StringView stage, bool time,
                              bool duration) const {
        const auto missing = [&](dstd::StringView role) {
            const dstd::String schema =
                roles_ && !roles_->schema.empty()
                    ? "record schema '" + roles_->schema + "'"
                    : dstd::String("this record schema");
            return fail(span, "'" + dstd::String(stage) + "' needs the " +
                                  dstd::String(role) + " role, which " +
                                  schema + " does not bind");
        };
        if (time && (!roles_ || roles_->time.empty())) return missing("time");
        if (duration && (!roles_ || roles_->duration.empty()))
            return missing("duration");
        return {};
    }

    // A time in the unit of the time role: a number, a parameter, or a
    // duration.
    // A time from numbers, parameters and durations, folded over `+`, `-`,
    // `*`, `/` and negation.
    Result<double> time_value(const Expr& e) const {
        if (const auto* b = std::get_if<Binary>(&e.node)) {
            const auto l = time_value(*b->left);
            if (!l) return dstd::Unexpected(l.error());
            const auto r = time_value(*b->right);
            if (!r) return dstd::Unexpected(r.error());
            switch (b->op) {
                case BinaryOp::Add:
                    return *l + *r;
                case BinaryOp::Sub:
                    return *l - *r;
                case BinaryOp::Mul:
                    return *l * *r;
                case BinaryOp::Div:
                    if (*r == 0) return fail(e.span, "a time divides by zero");
                    return *l / *r;
                default:
                    break;
            }
        }
        if (const auto* u = std::get_if<Unary>(&e.node);
            u && u->op == UnaryOp::Neg &&
            !std::holds_alternative<Literal>(u->operand->node)) {
            const auto v = time_value(*u->operand);
            if (!v) return dstd::Unexpected(v.error());
            return -*v;
        }
        if (const auto* d = std::get_if<Duration>(&e.node)) {
            const auto per = unit_ns(d->unit);
            if (!per)
                return fail(e.span, "Unknown duration unit '" + d->unit + "'");
            double amount = 0;
            if (dstd::from_chars_double(d->amount.data(),
                                        d->amount.data() + d->amount.size(),
                                        amount)
                    .ec != std::errc{})
                return fail(e.span, "Invalid duration");
            return amount * static_cast<double>(*per) /
                   static_cast<double>(roles_->time_ns_per_unit);
        }
        const auto v = literal(e);
        if (!v) return dstd::Unexpected(v.error());
        if (*v)
            if (auto x = number((*v)->value)) return *x;
        return fail(e.span,
                    "Expected a time: a number, a parameter, a duration, or "
                    "'+', '-', '*' or '/' over them");
    }

    // `within d` in the units of the time key `time`: a number as written, a
    // duration converted like a comparison with `time`, or a parameter.
    Result<double> tolerance(const Expr& w, const Expr& time,
                             dstd::StringView what = "within") const {
        dstd::Optional<double> v;
        if (std::holds_alternative<Duration>(w.node)) {
            const auto unit = unit_of(time);
            if (!unit) return dstd::Unexpected(unit.error());
            if (!*unit)
                return fail(w.span,
                            "'" + dstd::String(what) +
                                "' with a duration needs a time key with a "
                                "time or duration role; give a number in the "
                                "key's units");
            const auto c = duration(w, time);
            if (!c) return dstd::Unexpected(c.error());
            if (const auto* u = std::get_if<std::uint64_t>(&c->value))
                v = static_cast<double>(*u);
            else if (const auto* d = std::get_if<double>(&c->value))
                v = *d;
        } else {
            const auto lit = literal(w);
            if (!lit) return dstd::Unexpected(lit.error());
            if (*lit) v = number((*lit)->value);
        }
        if (!v)
            return fail(w.span, "'" + dstd::String(what) +
                                    "' takes a number in the time key's "
                                    "units, a duration or a parameter");
        if (!(*v >= 0) || !std::isfinite(*v))
            return fail(w.span, "'" + dstd::String(what) +
                                    "' must be a finite non-negative "
                                    "distance");
        return *v;
    }

    duql::TermPtr number_term(double v) const {
        if (std::trunc(v) == v && v >= 0 && v < 1.8e19)
            return make_term(duql::TConst{static_cast<std::uint64_t>(v)});
        if (std::trunc(v) == v && v < 0 && v > -9.2e18)
            return make_term(duql::TConst{static_cast<std::int64_t>(v)});
        return make_term(duql::TConst{v});
    }

    duql::PipelineItem role_item(const dstd::String& path) const {
        duql::TermPtr t = role_field(path);
        dstd::String text = duql::term_text(*t);
        return {{}, dstd::to_shared_ptr(std::move(t)), std::move(text)};
    }

    duql::TermPtr role_field(const dstd::String& path) const {
        duql::TField f;
        f.base = path;
        std::size_t at = 0;
        while (at <= path.size()) {
            const std::size_t dot = std::min(path.find('.', at), path.size());
            f.steps.push_back(
                {path.substr(at, dot - at), false, dstd::nullopt});
            at = dot + 1;
        }
        f.neg_at = f.steps.size();
        return make_term(std::move(f));
    }

    static dstd::Optional<double> number(const duql::LiteralValue& v) {
        if (const auto* i = std::get_if<std::int64_t>(&v))
            return static_cast<double>(*i);
        if (const auto* u = std::get_if<std::uint64_t>(&v))
            return static_cast<double>(*u);
        if (const auto* d = std::get_if<double>(&v)) return *d;
        return dstd::nullopt;
    }

    dstd::String text_of(Span span) const {
        return dstd::String(src_.substr(span.offset, span.length));
    }

    Result<duql::PipelineWindow> window(const Window& n) const {
        duql::PipelineWindow out;
        const auto* outer_order = win_order_;
        win_order_ = &n.order;
        auto restore = dstd::defer([&] { win_order_ = outer_order; });
        for (const auto& k : n.partition) {
            auto built = item(k.name, *k.value);
            if (!built) return dstd::Unexpected(built.error());
            out.keys.push_back(std::move(*built));
        }
        for (const auto& key : n.order) {
            auto built = key_item(*key.value);
            if (!built) return dstd::Unexpected(built.error());
            out.order.push_back({std::move(*built), key.descending,
                                 key.nulls == NullsOrder::First});
        }
        for (const auto& a : n.fields) {
            const std::size_t before = out.calls.size();
            win_ = &out.calls;
            auto t = term(*a.value);
            win_ = nullptr;
            if (!t) return dstd::Unexpected(t.error());
            if (out.calls.size() == before)
                return fail(a.value->span,
                            "each entry of a 'window' block uses a window "
                            "function, such as 'r = row_number()' or "
                            "'g = ts - lag(ts)'");
            out.items.push_back({a.name, dstd::to_shared_ptr(std::move(*t)),
                                 text_of(a.value->span)});
        }
        return out;
    }

    Result<dstd::Optional<std::int64_t>> whole_number(const Expr& e) const {
        const auto v = literal(e);
        if (!v) return dstd::Unexpected(v.error());
        if (!*v) return dstd::Optional<std::int64_t>{};
        if (const auto* i = std::get_if<std::int64_t>(&(*v)->value))
            return *i >= 0 ? dstd::Optional<std::int64_t>(*i) : dstd::nullopt;
        if (const auto* u = std::get_if<std::uint64_t>(&(*v)->value))
            return *u <= static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max())
                       ? dstd::Optional<std::int64_t>(
                             static_cast<std::int64_t>(*u))
                       : dstd::nullopt;
        return dstd::Optional<std::int64_t>{};
    }

    Result<duql::WinFrame> frame_of(const Over& o, Span span) const {
        duql::WinFrame f;
        if (o.rows) {
            const auto n = whole_number(*o.width);
            if (!n) return dstd::Unexpected(n.error());
            if (!*n || **n < 1)
                return fail(o.width->span,
                            "'over N rows' takes a positive integer, written "
                            "as a literal or a parameter");
            f.rows = **n;
            return f;
        }
        if (!win_order_ || win_order_->empty())
            return fail(span,
                        "'over' with a width needs a 'sort' key in the "
                        "'window' block");
        if (win_order_->size() > 1)
            return fail(span,
                        "'over' with a width needs exactly one sort key, got " +
                            dstd::int_text(win_order_->size()));
        const SortKey& key = win_order_->front();
        if (key.descending)
            return fail(span,
                        "'over' with a width needs an ascending sort key");
        f.range = true;
        const auto width = tolerance(*o.width, *key.value, "over");
        if (!width) return dstd::Unexpected(width.error());
        f.width = *width;
        if (std::trunc(f.width) != f.width || f.width >= 9.2e18)
            return fail(o.width->span,
                        "'over' width is a whole number in the sort key's "
                        "units");
        return f;
    }

    Result<duql::TermPtr> over_term(const Over& o, Span span) const {
        const auto& c = std::get<Call>(o.call->node);
        if (!win_ || win_arg_)
            return fail(span,
                        "'over' stands after a window function in a 'window' "
                        "block");
        const WinInfo* w = window_info(c.name);
        bool framed = false;
        if (w) switch (w->fn) {
                case duql::WinFn::Sum:
                case duql::WinFn::Mean:
                case duql::WinFn::Count:
                case duql::WinFn::CountIf:
                case duql::WinFn::CountDistinct:
                case duql::WinFn::Collect:
                case duql::WinFn::Argmax:
                case duql::WinFn::Argmin:
                case duql::WinFn::Var:
                case duql::WinFn::Std:
                case duql::WinFn::Quantile:
                    framed = true;
                    break;
                case duql::WinFn::Min:
                case duql::WinFn::Max:
                    framed = c.args.size() == 1;
                    break;
                default:
                    break;
            }
        if (!framed)
            return fail(span,
                        "'" + c.name +
                            "' takes no frame; 'over' follows sum, mean, min, "
                            "max, count, count_if, count_distinct, collect, "
                            "arg_max, arg_min, var, std or quantile");
        auto call = window_call(c, span, &o);
        if (!call) return dstd::Unexpected(call.error());
        return std::move(**call);
    }

    // A call of a window function inside a window entry, as a field of the
    // hidden column it fills; nullopt for any other function.
    Result<dstd::Optional<duql::TermPtr>> window_call(
        const Call& c, Span span, const Over* over = nullptr) const {
        const WinInfo* w = window_info(c.name);
        if (!win_) {
            if (w && w->window_only)
                return fail(span, "'" + c.name +
                                      "' is a window function; it stands in a "
                                      "'window' block");
            return dstd::Optional<duql::TermPtr>{};
        }
        if (win_arg_ && (w || aggregate_info(c.name)))
            return fail(span,
                        "a window function's argument holds no window "
                        "function or aggregate");
        if (win_arg_) return dstd::Optional<duql::TermPtr>{};
        if (!w) {
            if (aggregate_info(c.name))
                return fail(
                    span, "'" + c.name + "' does not run in a 'window' block");
            return dstd::Optional<duql::TermPtr>{};
        }
        if ((w->fn == duql::WinFn::Min || w->fn == duql::WinFn::Max) &&
            c.args.size() > 1)
            return dstd::Optional<duql::TermPtr>{};
        const std::size_t n = c.args.size();
        if (n < w->min_args || n > w->max_args)
            return fail(span, arity(c.name, w->min_args, w->max_args, n));
        for (const auto& arg : c.args)
            if (!arg.name.empty())
                return fail(arg.value->span, "'" + c.name +
                                                 "' takes no named argument '" +
                                                 arg.name + "'");
        const bool arg_by =
            w->fn == duql::WinFn::Argmax || w->fn == duql::WinFn::Argmin;
        duql::PipelineWinCall call;
        call.column = "__duql_w_" + dstd::int_text(win_->size());
        call.fn = w->fn;
        call.text = text_of(span);
        if (n > 0 && w->fn != duql::WinFn::Ntile) {
            win_arg_ = true;
            auto arg = term(*c.args[0].value);
            if (!arg) return dstd::Unexpected(arg.error());
            call.arg = dstd::to_shared_ptr(std::move(*arg));
            if (arg_by) {
                auto by = term(*c.args[1].value);
                if (!by) return dstd::Unexpected(by.error());
                call.by = dstd::to_shared_ptr(std::move(*by));
            }
            win_arg_ = false;
        }
        if (n > 1 && w->fn == duql::WinFn::Quantile) {
            const Expr& level = *c.args[1].value;
            const auto v = literal(level);
            if (!v) return dstd::Unexpected(v.error());
            const auto q = *v ? number((*v)->value) : dstd::nullopt;
            if (!q || !(*q >= 0 && *q <= 1))
                return fail(level.span,
                            "a quantile level is a number in [0, 1], written "
                            "as a literal or a parameter");
            call.q = *q;
        } else if (w->fn == duql::WinFn::Ntile || w->fn == duql::WinFn::Nth) {
            const Expr& at = *c.args[w->fn == duql::WinFn::Ntile ? 0 : 1].value;
            const auto k = whole_number(at);
            if (!k) return dstd::Unexpected(k.error());
            if (!*k || **k < 1)
                return fail(at.span,
                            "'" + c.name +
                                "' takes a positive integer, written as a "
                                "literal or a parameter");
            call.offset = **k;
        } else if (n > 1 && !arg_by) {
            const Expr& at = *c.args[1].value;
            const auto k = whole_number(at);
            if (!k) return dstd::Unexpected(k.error());
            if (!*k)
                return fail(at.span,
                            "a lag or lead distance is a non-negative "
                            "integer, written as a literal or a parameter");
            call.offset = **k;
        }
        if (n > 2) {
            const Expr& d = *c.args[2].value;
            const auto lit = literal(d);
            if (!lit) return dstd::Unexpected(lit.error());
            if (!*lit)
                return fail(d.span,
                            "a lag or lead default is a literal or a "
                            "parameter");
            auto fallback = term(d);
            if (!fallback) return dstd::Unexpected(fallback.error());
            call.fallback = dstd::to_shared_ptr(std::move(*fallback));
        }
        if (over) {
            const auto frame = frame_of(*over, span);
            if (!frame) return dstd::Unexpected(frame.error());
            call.frame = *frame;
        }
        duql::TField f;
        f.base = call.column;
        f.steps.push_back({call.column, false, dstd::nullopt});
        f.neg_at = 1;
        win_->push_back(std::move(call));
        return dstd::Optional<duql::TermPtr>(make_term(std::move(f)));
    }

    Result<dstd::Vector<duql::PipelineAgg>> aggregates(
        const dstd::Vector<Assign>& block,
        dstd::StringView stage = "group") const {
        dstd::Vector<duql::PipelineAgg> out;
        duql::PipelineDerive derive;
        agg_hidden_.clear();
        for (const auto& a : block) {
            const Expr& e = *a.value;
            const auto* c = std::get_if<Call>(&e.node);
            if (c && aggregate_info(c->name) && aggregate_arity(*c)) {
                auto built = aggregate(*c, e.span, a.name);
                if (!built) return dstd::Unexpected(built.error());
                out.push_back(std::move(*built));
                continue;
            }
            if (c && plugin_kind(c->name) == duql::PluginKind::Aggregate) {
                if (stage == "pivot")
                    return fail(e.span, plugin_aggregate_stage(c->name));
                auto built = plugin_aggregate(*c, e.span, a.name);
                if (!built) return dstd::Unexpected(built.error());
                out.push_back(std::move(*built));
                continue;
            }
            const std::size_t before = out.size();
            if (stage != "pivot") {
                agg_ = &out;
                agg_name_ = a.name;
                auto t = term(e);
                agg_ = nullptr;
                if (!t) return dstd::Unexpected(t.error());
                if (out.size() > before)
                    derive.items.push_back({a.name,
                                            dstd::to_shared_ptr(std::move(*t)),
                                            text_of(e.span)});
            }
            if (out.size() == before)
                return fail(e.span, stage == "pivot"
                                        ? "each entry of a 'pivot' block is "
                                          "one aggregate call, such as 'n = "
                                          "count()'"
                                        : "each entry of a '" +
                                              dstd::String(stage) +
                                              "' block holds an aggregate "
                                              "call, such as 'n = count()' or "
                                              "'ms = sum(dur) / 1000'");
        }
        if (!derive.items.empty()) {
            after_.push_back(std::move(derive));
            if (!agg_hidden_.empty())
                after_.push_back(duql::PipelineDrop{std::move(agg_hidden_)});
        }
        agg_hidden_.clear();
        return out;
    }

    // Whether `c`, named as an aggregate, has an argument count it takes.
    static bool aggregate_arity(const Call& c) {
        const AggInfo* info = aggregate_info(c.name);
        return c.args.size() >= info->min_args &&
               c.args.size() <= info->max_args;
    }

    // An aggregate call inside a group entry that is an expression, as a
    // field of the column it fills; nullopt for any other call.
    Result<dstd::Optional<duql::TermPtr>> entry_aggregate(const Call& c,
                                                          Span span) const {
        const bool plug = plugin_kind(c.name) == duql::PluginKind::Aggregate;
        if (plug && !agg_) return fail(span, plugin_aggregate_stage(c.name));
        if (!agg_ || (!plug && !aggregate_info(c.name)))
            return dstd::Optional<duql::TermPtr>{};
        const bool is_fn =
            std::any_of(std::begin(duql::FUNCTIONS), std::end(duql::FUNCTIONS),
                        [&](const auto& f) { return f.name == c.name; });
        if (!plug && is_fn && !aggregate_arity(c))
            return dstd::Optional<duql::TermPtr>{};
        dstd::String name = agg_name_;
        if (name.empty()) {
            name = "__duql_a_" + dstd::int_text(agg_hidden_.size());
            agg_hidden_.push_back(name);
        }
        agg_name_.clear();
        auto* sink = agg_;
        agg_ = nullptr;
        auto built =
            plug ? plugin_aggregate(c, span, name) : aggregate(c, span, name);
        agg_ = sink;
        if (!built) return dstd::Unexpected(built.error());
        sink->push_back(std::move(*built));
        duql::TField f;
        f.base = name;
        f.steps.push_back({name, false, dstd::nullopt});
        f.neg_at = 1;
        return dstd::Optional<duql::TermPtr>(make_term(std::move(f)));
    }

    static dstd::String plugin_aggregate_stage(const dstd::String& name) {
        return "'" + name +
               "' is a plugin aggregate; it stands in a 'group' or 'agg' "
               "block";
    }

    Result<duql::PipelineAgg> plugin_aggregate(const Call& c, Span span,
                                               const dstd::String& name) const {
        if (c.args.empty())
            return fail(
                span, "'" + c.name + "' takes a column as its first argument");
        for (const auto& a : c.args) {
            if (!a.name.empty())
                return fail(a.value->span, "'" + c.name +
                                               "' takes no named argument '" +
                                               a.name + "'");
            if (std::holds_alternative<Duration>(a.value->node))
                return fail(a.value->span,
                            "'" + c.name + "' takes no duration argument");
        }
        duql::PipelineAgg agg;
        agg.name = name;
        agg.fn = duql::AggFn::Plugin;
        agg.plugin = c.name;
        auto first = term(*c.args[0].value);
        if (!first) return dstd::Unexpected(first.error());
        agg.arg = dstd::to_shared_ptr(std::move(*first));
        agg.text = c.name + "(" + duql::term_text(*agg.arg);
        for (std::size_t k = 1; k < c.args.size(); ++k) {
            const Expr& e = *c.args[k].value;
            const auto v = literal(e);
            if (!v) return dstd::Unexpected(v.error());
            if (!*v)
                return fail(e.span,
                            "an operand of '" + c.name +
                                "' after the column is a number literal or "
                                "a parameter");
            if (!number((*v)->value))
                return fail(e.span,
                            "an operand of '" + c.name + "' is a number");
            agg.params.push_back((*v)->value);
            agg.text += ", " + text_of(e);
        }
        agg.text += ")";
        return agg;
    }

    Result<duql::PipelineAgg> aggregate(const Call& call, Span span,
                                        const dstd::String& name) const {
        const Call* c = &call;
        const AggInfo* info = aggregate_info(c->name);
        const std::size_t n = c->args.size();
        if (n < info->min_args || n > info->max_args)
            return fail(span,
                        arity(c->name, info->min_args, info->max_args, n));
        for (const auto& arg : c->args)
            if (!arg.name.empty())
                return fail(arg.value->span, "'" + c->name +
                                                 "' takes no named argument '" +
                                                 arg.name + "'");
        if (duql::is_occupancy(info->fn)) {
            const auto role = require_role(span, c->name, true, true);
            if (!role) return dstd::Unexpected(role.error());
        }
        duql::PipelineAgg agg;
        agg.name = name;
        agg.fn = info->fn;
        agg.text = c->name + "(";
        if (n > 0) {
            const Expr* first = c->args[0].value.get();
            const auto* inner = std::get_if<Call>(&first->node);
            if (info->fn == duql::AggFn::Quantile && inner &&
                inner->name == "merge") {
                if (inner->args.size() != 1 || !inner->args[0].name.empty())
                    return fail(first->span,
                                arity("merge", 1, 1, inner->args.size()));
                agg.merged = true;
                first = inner->args[0].value.get();
            }
            auto arg = term(*first);
            if (!arg) return dstd::Unexpected(arg.error());
            agg.arg = dstd::to_shared_ptr(std::move(*arg));
            agg.text += agg.merged ? "merge(" + duql::term_text(*agg.arg) + ")"
                                   : duql::term_text(*agg.arg);
        }
        if (info->fn == duql::AggFn::Argmax ||
            info->fn == duql::AggFn::Argmin) {
            auto by = term(*c->args[1].value);
            if (!by) return dstd::Unexpected(by.error());
            agg.by = dstd::to_shared_ptr(std::move(*by));
            agg.text += ", " + duql::term_text(*agg.by);
        }
        if (info->fn == duql::AggFn::Quantile) {
            const Expr& level = *c->args[1].value;
            const auto v = literal(level);
            if (!v) return dstd::Unexpected(v.error());
            const auto q = *v ? number((*v)->value) : dstd::nullopt;
            if (!q || !(*q >= 0 && *q <= 1))
                return fail(level.span,
                            "a quantile level is a number in [0, 1], written "
                            "as a literal or a parameter");
            agg.q = *q;
            agg.text += ", " + duql::term_text(*make_term(duql::TConst{*q}));
        }
        agg.text += ")";
        return agg;
    }

    static dstd::String stage_name(const Stage& s) {
        static constexpr const char* NAMES[] = {
            "where",    "derive",     "select",    "drop",   "rename",
            "distinct", "group",      "agg",       "window", "pivot",
            "unpivot",  "sort",       "take",      "skip",   "sample",
            "expand",   "lookup",     "lookup",    "lookup", "union",
            "call",     "time_range", "call_tree", "bucket", "session",
            "parse",    "use"};
        static_assert(std::size(NAMES) == std::variant_size_v<StageNode>);
        return NAMES[s.node.index()];
    }

    static dstd::Optional<std::int64_t> unit_ns(dstd::StringView unit) {
        if (unit == "ns") return 1;
        if (unit == "us") return 1000;
        if (unit == "ms") return 1'000'000;
        if (unit == "s") return 1'000'000'000;
        if (unit == "m") return 60'000'000'000;
        if (unit == "h") return 3'600'000'000'000;
        if (unit == "d") return 86'400'000'000'000;
        return dstd::nullopt;
    }

    // Nanoseconds per unit of an expression over a field with a time or
    // duration role. `+`, `-`, `??`, negation and the value arguments of
    // coalesce, if, case, min, max and abs keep a unit when their units
    // agree; a product or quotient of a timed field has none and fails, since
    // a duration beside it could mean either scale.
    // `strict` fails on a scaled timed field; otherwise it has no unit.
    Result<dstd::Optional<std::int64_t>> unit_of(const Expr& e,
                                                 bool strict = true) const {
        using Unit = dstd::Optional<std::int64_t>;
        if (!roles_) return Unit{};
        if (const auto* p = std::get_if<Path>(&e.node)) {
            if (p->root != PathRoot::Record) return Unit{};
            const auto name = path_text(*p, e.span);
            if (!name) return dstd::Unexpected(name.error());
            return column_unit(*name);
        }
        if (const auto* b = std::get_if<Binary>(&e.node)) {
            switch (b->op) {
                case BinaryOp::Add:
                case BinaryOp::Sub:
                case BinaryOp::Coalesce:
                    return common_unit({b->left.get(), b->right.get()}, strict);
                case BinaryOp::Mul:
                case BinaryOp::Div:
                case BinaryOp::Idiv:
                case BinaryOp::Mod:
                    if (strict) {
                        const auto l = unit_of(*b->left);
                        if (!l) return dstd::Unexpected(l.error());
                        bool scaled = l->has_value();
                        if (!scaled) {
                            const auto r = unit_of(*b->right);
                            if (!r) return dstd::Unexpected(r.error());
                            scaled = r->has_value();
                        }
                        if (scaled)
                            return fail(
                                e.span,
                                "'" + text_of(e) +
                                    "' scales a field with a time or duration "
                                    "role, so a duration beside it has no "
                                    "clear unit; compare the field itself, or "
                                    "give the unit with as_time(" +
                                    text_of(e) + ", \"ms\")");
                    }
                    return Unit{};
                default:
                    return Unit{};
            }
        }
        if (const auto* u = std::get_if<Unary>(&e.node)) {
            if (u->op == UnaryOp::Neg) return unit_of(*u->operand, strict);
            return Unit{};
        }
        if (const auto* c = std::get_if<Call>(&e.node)) {
            if (c->name == "as_time" || c->name == "now")
                return Unit(roles_->time_ns_per_unit);
            if ((c->name == "bin" || c->name == "abs") && !c->args.empty())
                return unit_of(*c->args[0].value, strict);
            dstd::Vector<const Expr*> values;
            if (c->name == "coalesce" || c->name == "min" || c->name == "max") {
                for (const Arg& a : c->args) values.push_back(a.value.get());
            } else if (c->name == "if" && c->args.size() == 3) {
                values = {c->args[1].value.get(), c->args[2].value.get()};
            } else if (c->name == "case") {
                for (std::size_t i = 1; i < c->args.size(); i += 2)
                    values.push_back(c->args[i].value.get());
                if (c->args.size() % 2 == 1)
                    values.push_back(c->args.back().value.get());
            }
            if (!values.empty()) return common_unit(values, strict);
        }
        return Unit{};
    }

    // The unit the timed ones of `parts` share; none when none is timed or
    // two differ. A part with no unit (a number, a duration) takes it.
    Result<dstd::Optional<std::int64_t>> common_unit(
        const dstd::Vector<const Expr*>& parts, bool strict = true) const {
        dstd::Optional<std::int64_t> unit;
        for (const Expr* p : parts) {
            if (!p) continue;
            const auto u = unit_of(*p, strict);
            if (!u) return dstd::Unexpected(u.error());
            if (!*u) continue;
            if (unit && *unit != **u) return dstd::Optional<std::int64_t>{};
            unit = *u;
        }
        return unit;
    }

    // The unit of column `name`: its entry in `col_units_`, else, while the
    // rows still hold the records, the role of the record field.
    dstd::Optional<std::int64_t> column_unit(dstd::StringView name) const {
        if (const auto c = col_units_.find(name); c != col_units_.end())
            return c->second;
        if (!record_units_) return dstd::nullopt;
        auto it = roles_->fields.find(name);
        if (it == roles_->fields.end() && name.starts_with("args."))
            it = roles_->fields.find(name.substr(5));
        if (it == roles_->fields.end()) return dstd::nullopt;
        return it->second;
    }

    // The unit an aggregate keeps: that of its argument for the ones whose
    // result is a value of it.
    Result<dstd::Optional<std::int64_t>> aggregate_unit(const Expr& e) const {
        const auto* c = std::get_if<Call>(&e.node);
        if (!c || c->args.empty()) return dstd::Optional<std::int64_t>{};
        static constexpr dstd::StringView KEEP[] = {
            "sum", "min", "max", "mean", "first", "last", "quantile", "std"};
        for (dstd::StringView k : KEEP)
            if (c->name == k) return unit_of(*c->args[0].value, false);
        return dstd::Optional<std::int64_t>{};
    }

    Result<dstd::String> column_name(const Item& it) const {
        if (!it.name.empty()) return it.name;
        if (const auto* p = std::get_if<Path>(&it.value->node))
            return path_text(*p, it.value->span);
        return dstd::String{};
    }

    // Updates the column units for the stage `s`, lowered just now; the
    // units it reads are those before it.
    Result<void> track_units(const Stage& s) const {
        using Units = decltype(col_units_);
        auto set = [&](Units& into, const dstd::String& name,
                       dstd::Optional<std::int64_t> u) {
            if (!name.empty()) into[name] = u;
        };
        return std::visit(
            [&](const auto& n) -> Result<void> {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, Derive> ||
                              std::is_same_v<T, Window>) {
                    for (const auto& a : n.fields) {
                        const auto u = unit_of(*a.value, false);
                        if (!u) return dstd::Unexpected(u.error());
                        set(col_units_, a.name, *u);
                    }
                } else if constexpr (std::is_same_v<T, Parse>) {
                    const auto derived = parse_derive(n);
                    if (!derived) return dstd::Unexpected(derived.error());
                    for (const auto& a : derived->fields)
                        set(col_units_, a.name, dstd::nullopt);
                } else if constexpr (std::is_same_v<T, Select>) {
                    Units next;
                    for (const auto& it : n.items) {
                        const auto name = column_name(it);
                        if (!name) return dstd::Unexpected(name.error());
                        const auto u = unit_of(*it.value, false);
                        if (!u) return dstd::Unexpected(u.error());
                        set(next, *name, *u);
                    }
                    col_units_ = std::move(next);
                    record_units_ = false;
                } else if constexpr (std::is_same_v<T, Distinct>) {
                    if (n.keys.empty()) return {};
                    Units next;
                    for (const auto& k : n.keys) {
                        const auto name = column_name(k);
                        if (!name) return dstd::Unexpected(name.error());
                        const auto u = unit_of(*k.value, false);
                        if (!u) return dstd::Unexpected(u.error());
                        set(next, *name, *u);
                    }
                    col_units_ = std::move(next);
                    record_units_ = false;
                } else if constexpr (std::is_same_v<T, Group> ||
                                     std::is_same_v<T, Agg>) {
                    Units next;
                    if constexpr (std::is_same_v<T, Group>)
                        for (const auto& k : n.keys) {
                            const auto name = column_name(k);
                            if (!name) return dstd::Unexpected(name.error());
                            const auto u = unit_of(*k.value, false);
                            if (!u) return dstd::Unexpected(u.error());
                            set(next, *name, *u);
                        }
                    for (const auto& a : n.aggregates) {
                        const auto u = aggregate_unit(*a.value);
                        if (!u) return dstd::Unexpected(u.error());
                        set(next, a.name, *u);
                    }
                    col_units_ = std::move(next);
                    record_units_ = false;
                } else if constexpr (std::is_same_v<T, Drop>) {
                    for (const auto& p : n.paths) {
                        auto name = path_text(p, s.span);
                        if (!name) return dstd::Unexpected(name.error());
                        col_units_[std::move(*name)] = dstd::nullopt;
                    }
                } else if constexpr (std::is_same_v<T, Rename>) {
                    Units moved;
                    for (const auto& [name, path] : n.pairs) {
                        const auto from = path_text(path, s.span);
                        if (!from) return dstd::Unexpected(from.error());
                        moved[name] = column_unit(*from);
                    }
                    for (const auto& [name, path] : n.pairs) {
                        auto from = path_text(path, s.span);
                        if (!from) return dstd::Unexpected(from.error());
                        col_units_[std::move(*from)] = dstd::nullopt;
                    }
                    for (auto& [name, u] : moved) col_units_[name] = u;
                } else if constexpr (std::is_same_v<T, Expand>) {
                    const auto path = path_text(n.path, s.span);
                    if (!path) return dstd::Unexpected(path.error());
                    set(col_units_, *path, dstd::nullopt);
                    set(col_units_, n.as, dstd::nullopt);
                    if (!n.path.steps.empty())
                        set(col_units_, n.path.steps.back().key, dstd::nullopt);
                    set(col_units_, n.with_index, dstd::nullopt);
                } else if constexpr (std::is_same_v<T, Session>) {
                    set(col_units_, n.as.empty() ? "session" : n.as,
                        dstd::nullopt);
                } else if constexpr (std::is_same_v<T, Bucket>) {
                    set(col_units_, n.as.empty() ? "bucket" : n.as,
                        dstd::nullopt);
                } else if constexpr (std::is_same_v<T, Lookup> ||
                                     std::is_same_v<T, OverlapLookup>) {
                    set(col_units_, n.into, dstd::nullopt);
                } else if constexpr (std::is_same_v<T, Pivot> ||
                                     std::is_same_v<T, Unpivot> ||
                                     std::is_same_v<T, Union> ||
                                     std::is_same_v<T, CallStage> ||
                                     std::is_same_v<T, CallTree>) {
                    col_units_.clear();
                    record_units_ = false;
                }
                return {};
            },
            s.node);
    }

    dstd::String text_of(const Expr& e) const {
        return dstd::String(src_.substr(e.span.offset, e.span.length));
    }

    // The duration `dur` in the unit of `other`: an integer when exact.
    Result<duql::TConst> duration(const Expr& dur, const Expr& other) const {
        const auto& d = std::get<Duration>(dur.node);
        const auto per = unit_ns(d.unit);
        if (!per)
            return fail(dur.span, "Unknown duration unit '" + d.unit + "'");
        double amount = 0;
        const auto r = dstd::from_chars_double(
            d.amount.data(), d.amount.data() + d.amount.size(), amount);
        if (r.ec != std::errc{}) return fail(dur.span, "Invalid duration");
        const auto unit = unit_of(other);
        if (!unit) return dstd::Unexpected(unit.error());
        if (!*unit)
            return fail(other.span,
                        "'" +
                            dstd::String(src_.substr(other.span.offset,
                                                     other.span.length)) +
                            "' has no time or duration role to give the "
                            "duration a unit; write as_time(x, \"ns\") to give "
                            "it one");
        const double v =
            amount * static_cast<double>(*per) / static_cast<double>(**unit);
        if (std::trunc(v) == v && v < 1.8e19)
            return duql::TConst{static_cast<std::uint64_t>(v)};
        return duql::TConst{v};
    }

    Result<duql::TermPtr> operand(const Expr& e, const Expr& other) const {
        if (std::holds_alternative<Duration>(e.node)) {
            auto c = duration(e, other);
            if (!c) return dstd::Unexpected(c.error());
            return make_term(std::move(*c));
        }
        if (const auto* u = std::get_if<Unary>(&e.node);
            u && u->op == UnaryOp::Neg &&
            std::holds_alternative<Duration>(u->operand->node)) {
            auto built = duration(*u->operand, other);
            if (!built) return dstd::Unexpected(built.error());
            duql::TConst c = std::move(*built);
            if (const auto* v = std::get_if<std::uint64_t>(&c.value);
                v && *v <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()))
                c.value = -static_cast<std::int64_t>(*v);
            else if (const auto* d = std::get_if<double>(&c.value))
                c.value = -*d;
            else
                c.value =
                    -static_cast<double>(std::get<std::uint64_t>(c.value));
            return make_term(std::move(c));
        }
        return term(e);
    }

    // bin(t, d), to_seconds(t) and as_time(x, unit) as arithmetic in the
    // units of the schema's roles.
    Result<dstd::Optional<duql::TermPtr>> time_call(const Call& c,
                                                    Span span) const {
        if (c.name != "bin" && c.name != "to_seconds" && c.name != "as_time" &&
            c.name != "now")
            return dstd::Optional<duql::TermPtr>{};
        if (!roles_)
            return fail(span, "'" + c.name +
                                  "' needs the record schema's time roles; "
                                  "run the query on a View");
        const std::size_t want = c.name == "now"          ? 0u
                                 : c.name == "to_seconds" ? 1u
                                                          : 2u;
        if (c.args.size() != want)
            return fail(span, arity(c.name, want, want, c.args.size()));
        if (c.name == "now") {
            if (!now_) {
                const auto ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                now_ = static_cast<std::int64_t>(ns) / roles_->time_ns_per_unit;
            }
            return dstd::Optional<duql::TermPtr>(
                make_term(duql::TConst{*now_}));
        }
        const Expr& x = *c.args[0].value;
        if (c.name == "bin") {
            const Expr& w = *c.args[1].value;
            auto subject = term(x);
            if (!subject) return dstd::Unexpected(subject.error());
            auto step = operand(w, x);
            if (!step) return dstd::Unexpected(step.error());
            auto scale = operand(w, x);
            if (!scale) return dstd::Unexpected(scale.error());
            return dstd::Optional<duql::TermPtr>(make_term(duql::TBinary{
                duql::TermOp::Mul,
                make_term(duql::TBinary{duql::TermOp::Idiv, std::move(*subject),
                                        std::move(*step)}),
                std::move(*scale)}));
        }
        if (c.name == "to_seconds") {
            const auto unit = unit_of(x);
            if (!unit) return dstd::Unexpected(unit.error());
            if (!*unit)
                return fail(x.span,
                            "to_seconds() takes a field with a time or "
                            "duration role");
            auto subject = term(x);
            if (!subject) return dstd::Unexpected(subject.error());
            return dstd::Optional<duql::TermPtr>(make_term(duql::TBinary{
                duql::TermOp::Div, std::move(*subject),
                make_term(duql::TConst{1e9 / static_cast<double>(**unit)})}));
        }
        const auto unit_text = pattern(*c.args[1].value);
        if (!unit_text) return dstd::Unexpected(unit_text.error());
        const auto per = unit_ns(*unit_text);
        if (!per)
            return fail(c.args[1].value->span,
                        "as_time() takes the unit \"ns\", \"us\", \"ms\", "
                        "\"s\", \"m\", \"h\" or \"d\"");
        const std::int64_t time = roles_->time_ns_per_unit;
        auto subject = term(x);
        if (!subject) return dstd::Unexpected(subject.error());
        if (*per % time == 0)
            return dstd::Optional<duql::TermPtr>(make_term(duql::TBinary{
                duql::TermOp::Mul, std::move(*subject),
                make_term(duql::TConst{std::uint64_t(*per / time)})}));
        return dstd::Optional<duql::TermPtr>(make_term(
            duql::TBinary{duql::TermOp::Div, std::move(*subject),
                          make_term(duql::TConst{static_cast<double>(time) /
                                                 static_cast<double>(*per)})}));
    }

    static dstd::String arity(dstd::StringView name, std::size_t lo,
                              std::size_t hi, std::size_t got) {
        auto args = [](std::size_t k) {
            return dstd::int_text(k) + (k == 1 ? " argument" : " arguments");
        };
        dstd::String want = lo == hi ? args(lo)
                            : hi == SIZE_MAX
                                ? "at least " + args(lo)
                                : dstd::int_text(lo) + " to " + args(hi);
        return "'" + dstd::String(name) + "' takes " + want + ", got " +
               dstd::int_text(got);
    }

    duql::DuqlError error_at(Span span, dstd::String msg) const {
        return make_error(src_, span.offset, span.length, std::move(msg));
    }

    Fail fail(Span span, dstd::String msg) const {
        return Fail(error_at(span, std::move(msg)));
    }

    Result<detail::QueryNodePtr> condition(const Expr& e) const {
        if (const auto* u = std::get_if<Unary>(&e.node)) {
            if (u->op == UnaryOp::Not) {
                auto operand = condition(*u->operand);
                if (!operand) return dstd::Unexpected(operand.error());
                return duql::detail::make_node(
                    duql::detail::NotNode{std::move(*operand)});
            }
        } else if (const auto* b = std::get_if<Binary>(&e.node)) {
            switch (b->op) {
                case BinaryOp::Or: {
                    auto left = condition(*b->left);
                    if (!left) return dstd::Unexpected(left.error());
                    auto right = condition(*b->right);
                    if (!right) return dstd::Unexpected(right.error());
                    return duql::detail::make_node(duql::detail::OrNode{
                        std::move(*left), std::move(*right)});
                }
                case BinaryOp::And: {
                    auto left = condition(*b->left);
                    if (!left) return dstd::Unexpected(left.error());
                    auto right = condition(*b->right);
                    if (!right) return dstd::Unexpected(right.error());
                    return duql::detail::make_node(duql::detail::AndNode{
                        std::move(*left), std::move(*right)});
                }
                case BinaryOp::Eq:
                case BinaryOp::Ne:
                case BinaryOp::Lt:
                case BinaryOp::Le:
                case BinaryOp::Gt:
                case BinaryOp::Ge: {
                    auto n = compare(*b);
                    if (!n) return dstd::Unexpected(n.error());
                    if (*n) return std::move(*n);
                    break;
                }
                case BinaryOp::Regex:
                case BinaryOp::Iregex:
                case BinaryOp::Nregex:
                case BinaryOp::Niregex: {
                    auto f = as_field(*b->left);
                    if (!f) return dstd::Unexpected(f.error());
                    if (*f) {
                        const auto [op, negated] = regex_op(b->op);
                        const auto text = pattern(*b->right);
                        if (!text) return dstd::Unexpected(text.error());
                        return match(std::move(**f), op, *text, negated,
                                     *b->right);
                    }
                    break;
                }
                default:
                    break;
            }
        } else if (const auto* in = std::get_if<In>(&e.node)) {
            if (!in->subquery) {
                auto n = membership(*in);
                if (!n) return dstd::Unexpected(n.error());
                if (*n) return std::move(*n);
            }
        } else if (const auto* l = std::get_if<Like>(&e.node)) {
            auto f = as_field(*l->subject);
            if (!f) return dstd::Unexpected(f.error());
            if (*f) {
                const auto escape = escape_char(l->escape, e.span);
                if (!escape) return dstd::Unexpected(escape.error());
                return match(std::move(**f),
                             l->icase ? MatchOp::Ilike : MatchOp::Like,
                             l->pattern, l->negated, e, *escape);
            }
        } else if (const auto* c = std::get_if<Contains>(&e.node)) {
            auto field = path_text(c->path, e.span);
            if (!field) return dstd::Unexpected(field.error());
            return match(duql::detail::FieldNode{std::move(*field), c->any},
                         MatchOp::Icontains, c->text, c->negated, e);
        }
        const Flag in_condition(cond_, true);
        auto t = term(e);
        if (!t) return dstd::Unexpected(t.error());
        dstd::String text = duql::term_text(**t);
        return duql::detail::make_node(duql::detail::ExprLeaf{
            dstd::to_shared_ptr(std::move(*t)), std::move(text)});
    }

    static dstd::Pair<MatchOp, bool> regex_op(BinaryOp op) {
        const bool icase = op == BinaryOp::Iregex || op == BinaryOp::Niregex;
        const bool negated = op == BinaryOp::Nregex || op == BinaryOp::Niregex;
        return {icase ? MatchOp::Iregex : MatchOp::Regex, negated};
    }

    Result<void> reject_null(const Expr& e) const {
        if (const auto* lit = std::get_if<Literal>(&e.node))
            if (std::holds_alternative<Null>(lit->value))
                return fail(e.span,
                            "a comparison with null is never true; write "
                            "'x is null' or 'x is not null'");
        return {};
    }

    // `field op value` as a literal leaf, or null when the condition has
    // another shape.
    Result<detail::QueryNodePtr> compare(const Binary& b) const {
        const auto left_null = reject_null(*b.left);
        if (!left_null) return dstd::Unexpected(left_null.error());
        const auto right_null = reject_null(*b.right);
        if (!right_null) return dstd::Unexpected(right_null.error());
        auto f = as_field(*b.left);
        if (!f) return dstd::Unexpected(f.error());
        if (!*f) return detail::QueryNodePtr{};
        if (std::holds_alternative<Duration>(b.right->node)) {
            const auto c = duration(*b.right, *b.left);
            if (!c) return dstd::Unexpected(c.error());
            LiteralNode lit;
            if (const auto* u = std::get_if<std::uint64_t>(&c->value))
                lit.value = *u;
            else
                lit.value = std::get<double>(c->value);
            return duql::detail::make_node(duql::detail::CompareNode{
                std::move(**f), compare_op(b.op), lit});
        }
        auto value = literal(*b.right);
        if (!value) return dstd::Unexpected(value.error());
        if (!*value) return detail::QueryNodePtr{};
        return duql::detail::make_node(duql::detail::CompareNode{
            std::move(**f), compare_op(b.op), std::move(**value)});
    }

    static duql::CompareOp compare_op(BinaryOp op) {
        switch (op) {
            case BinaryOp::Ne:
                return duql::CompareOp::Ne;
            case BinaryOp::Lt:
                return duql::CompareOp::Lt;
            case BinaryOp::Le:
                return duql::CompareOp::Le;
            case BinaryOp::Gt:
                return duql::CompareOp::Gt;
            case BinaryOp::Ge:
                return duql::CompareOp::Ge;
            default:
                return duql::CompareOp::Eq;
        }
    }

    Result<void> reject_null_items(const In& in) const {
        for (const auto& item : in.list)
            if (const auto* lit = std::get_if<Literal>(&item->node))
                if (std::holds_alternative<Null>(lit->value))
                    return fail(item->span,
                                "null in an 'in' list is never matched and "
                                "makes 'not in' never true; test 'x is null' "
                                "apart");
        return {};
    }

    // A literal `in` list on a field as a literal leaf, or null.
    Result<detail::QueryNodePtr> membership(const In& in) const {
        const auto items = reject_null_items(in);
        if (!items) return dstd::Unexpected(items.error());
        auto f = as_field(*in.subject);
        if (!f) return dstd::Unexpected(f.error());
        if (!*f) return detail::QueryNodePtr{};
        duql::detail::ArrayNode arr;
        for (const auto& item : in.list) {
            auto value = literal(*item);
            if (!value) return dstd::Unexpected(value.error());
            if (!*value) return detail::QueryNodePtr{};
            arr.elements.push_back(std::move(**value));
        }
        arr.set = duql::make_in_set(arr);
        if (in.negated)
            return duql::detail::make_node(
                duql::detail::NotInNode{std::move(**f), std::move(arr)});
        return duql::detail::make_node(
            duql::detail::InNode{std::move(**f), std::move(arr)});
    }

    Result<dstd::String> pattern(
        const Expr& e, const char* what = "Expected a string pattern") const {
        if (const auto* lit = std::get_if<Literal>(&e.node))
            if (const auto* s = std::get_if<dstd::String>(&lit->value))
                return *s;
        if (const auto* p = std::get_if<Param>(&e.node)) {
            const auto value = bound(*p, e.span);
            if (!value) return dstd::Unexpected(value.error());
            if (const auto* s = std::get_if<dstd::String>(&*value)) return *s;
        }
        return fail(e.span, what);
    }

    Result<duql::PatternPtr> compile(MatchOp op, const dstd::String& pattern,
                                     dstd::Optional<char> escape,
                                     const Expr& at) const {
        duql::PatternResult r = [&] {
            switch (op) {
                case MatchOp::Like:
                    return duql::compile_like(pattern, false, escape);
                case MatchOp::Ilike:
                    return duql::compile_like(pattern, true, escape);
                case MatchOp::Regex:
                    return duql::compile_regex(pattern, false);
                case MatchOp::Iregex:
                    return duql::compile_regex(pattern, true);
                case MatchOp::Icontains:
                    break;
            }
            return duql::compile_contains(pattern, true);
        }();
        if (!r)
            return fail(at.span, "Invalid pattern at offset " +
                                     dstd::int_text(r.error().offset) + ": " +
                                     r.error().message);
        return std::move(*r);
    }

    Result<dstd::Optional<char>> escape_char(
        const dstd::Optional<dstd::String>& esc, Span span) const {
        if (!esc) return dstd::Optional<char>{};
        if (esc->size() != 1) return fail(span, "'escape' takes one character");
        return dstd::Optional<char>((*esc)[0]);
    }

    Result<detail::QueryNodePtr> match(
        duql::detail::FieldNode f, MatchOp op, dstd::String pattern,
        bool negated, const Expr& at,
        dstd::Optional<char> escape = dstd::nullopt) const {
        duql::detail::MatchNode node;
        auto compiled = compile(op, pattern, escape, at);
        if (!compiled) return dstd::Unexpected(compiled.error());
        node.compiled = std::move(*compiled);
        node.field = std::move(f);
        node.op = op;
        node.pattern = std::move(pattern);
        node.negated = negated;
        node.escape = escape;
        return duql::detail::make_node(std::move(node));
    }

    // The field a literal leaf tests: a record path without a negative index,
    // or any(path).
    Result<dstd::Optional<duql::detail::FieldNode>> as_field(
        const Expr& e) const {
        const Path* p = std::get_if<Path>(&e.node);
        bool any = false;
        if (!p)
            if (const auto* c = std::get_if<Call>(&e.node))
                if (c->name == "any" && c->args.size() == 1 &&
                    c->args[0].name.empty()) {
                    p = std::get_if<Path>(&c->args[0].value->node);
                    any = p != nullptr;
                }
        if (!p || negative_at(*p) != p->steps.size())
            return dstd::Optional<duql::detail::FieldNode>{};
        auto text = path_text(*p, e.span);
        if (!text) return dstd::Unexpected(text.error());
        return dstd::Optional<duql::detail::FieldNode>(
            duql::detail::FieldNode{std::move(*text), any});
    }

    static std::size_t negative_at(const Path& p) {
        for (std::size_t i = 0; i < p.steps.size(); ++i)
            if (p.steps[i].index && *p.steps[i].index < 0) return i;
        return p.steps.size();
    }

    Result<dstd::String> path_text(const Path& p, Span span) const {
        return steps_text(p, span, p.steps.size());
    }

    Fail reject_root(const Path& p, Span span) const {
        if (p.root == PathRoot::Enclosing && lookup_side_ > 0)
            return fail(span, "a lookup side reads no enclosing row ('^.')");
        if (p.root == PathRoot::Enclosing && sub_ > 0)
            return fail(span,
                        "'^.' reads the enclosing row, which a sub-query "
                        "cannot see; to read a row of a let by key write 'k -> "
                        "set.path'");
        return fail(span,
                    "'.' and '^.' paths stand inside any(p, e) or all(p, e)");
    }

    Result<dstd::String> steps_text(const Path& p, Span span,
                                    std::size_t end) const {
        if (p.root != PathRoot::Record) return reject_root(p, span);
        for (const auto& step : p.steps)
            if (step.wildcard())
                return fail(span,
                            "a wildcard path needs a source with a field "
                            "catalog");
        dstd::String out;
        for (std::size_t i = 0; i < end; ++i) {
            const auto& step = p.steps[i];
            if (!step.key.empty() || !step.index) {
                if (!out.empty()) out += '.';
                out += step.key;
            }
            if (step.index) {
                out += '[';
                out += dstd::int_text(*step.index);
                out += ']';
            }
        }
        return out;
    }

    static duql::TermPtr make_term(decltype(duql::Term::node) node) {
        return dstd::make_unique<const duql::Term>(duql::Term{std::move(node)});
    }

    static duql::TConst constant(const duql::LiteralValue& v) {
        return std::visit([](const auto& x) { return duql::TConst{x}; }, v);
    }

    static duql::TermOp term_op(BinaryOp op) {
        switch (op) {
            case BinaryOp::Or:
                return duql::TermOp::Or;
            case BinaryOp::And:
                return duql::TermOp::And;
            case BinaryOp::Eq:
                return duql::TermOp::Eq;
            case BinaryOp::Ne:
                return duql::TermOp::Ne;
            case BinaryOp::Lt:
                return duql::TermOp::Lt;
            case BinaryOp::Le:
                return duql::TermOp::Le;
            case BinaryOp::Gt:
                return duql::TermOp::Gt;
            case BinaryOp::Ge:
                return duql::TermOp::Ge;
            case BinaryOp::Add:
                return duql::TermOp::Add;
            case BinaryOp::Sub:
                return duql::TermOp::Sub;
            case BinaryOp::Mul:
                return duql::TermOp::Mul;
            case BinaryOp::Div:
                return duql::TermOp::Div;
            case BinaryOp::Idiv:
                return duql::TermOp::Idiv;
            case BinaryOp::Mod:
                return duql::TermOp::Mod;
            case BinaryOp::Coalesce:
                return duql::TermOp::Coalesce;
            case BinaryOp::Regex:
            case BinaryOp::Iregex:
            case BinaryOp::Nregex:
            case BinaryOp::Niregex:
                break;
        }
        return duql::TermOp::Eq;
    }

    Result<duql::TermPtr> term(const Expr& e) const {
        return std::visit(
            [&](const auto& n) -> Result<duql::TermPtr> {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, Literal>) {
                    return std::visit(
                        [&](const auto& v) {
                            using V = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<V, Null>)
                                return make_term(duql::TConst{duql::TNull{}});
                            else
                                return make_term(duql::TConst{v});
                        },
                        n.value);
                } else if constexpr (std::is_same_v<T, Duration>) {
                    return fail(e.span,
                                "a duration takes its unit from the field it "
                                "meets in a comparison, 'between', arithmetic "
                                "or bin()");
                } else if constexpr (std::is_same_v<T, Path>) {
                    return field_term(n, e.span);
                } else if constexpr (std::is_same_v<T, Param>) {
                    const auto v = bound(n, e.span);
                    if (!v) return dstd::Unexpected(v.error());
                    return make_term(constant(*v));
                } else if constexpr (std::is_same_v<T, Unary>) {
                    if (n.op == UnaryOp::Not) {
                        auto operand = term(*n.operand);
                        if (!operand) return dstd::Unexpected(operand.error());
                        return make_term(duql::TUnary{duql::TermOp::Not,
                                                      std::move(*operand)});
                    }
                    const auto v = literal(e);
                    if (!v) return dstd::Unexpected(v.error());
                    if (*v) return make_term(constant((*v)->value));
                    auto operand = term(*n.operand);
                    if (!operand) return dstd::Unexpected(operand.error());
                    return make_term(
                        duql::TUnary{duql::TermOp::Neg, std::move(*operand)});
                } else if constexpr (std::is_same_v<T, Binary>) {
                    return binary_term(n);
                } else if constexpr (std::is_same_v<T, In>) {
                    if (n.subquery) {
                        auto join = semi_join(n, e.span);
                        if (!join) return dstd::Unexpected(join.error());
                        return make_term(std::move(*join));
                    }
                    const auto nulls = reject_null_items(n);
                    if (!nulls) return dstd::Unexpected(nulls.error());
                    auto matched =
                        any_match(*n.subject, [&]() -> Result<duql::TermPtr> {
                            duql::TIn in{element(), {}, n.negated};
                            for (const auto& item : n.list) {
                                auto t = term(*item);
                                if (!t) return dstd::Unexpected(t.error());
                                in.list.push_back(std::move(*t));
                            }
                            return make_term(std::move(in));
                        });
                    if (!matched) return dstd::Unexpected(matched.error());
                    if (*matched) return std::move(**matched);
                    auto subject = term(*n.subject);
                    if (!subject) return dstd::Unexpected(subject.error());
                    duql::TIn t{std::move(*subject), {}, n.negated};
                    for (const auto& item : n.list) {
                        auto value = term(*item);
                        if (!value) return dstd::Unexpected(value.error());
                        t.list.push_back(std::move(*value));
                    }
                    return make_term(std::move(t));
                } else if constexpr (std::is_same_v<T, Contains>) {
                    if (!n.any) {
                        auto field = field_term(n.path, e.span);
                        if (!field) return dstd::Unexpected(field.error());
                        auto compiled = compile(MatchOp::Icontains, n.text,
                                                dstd::nullopt, e);
                        if (!compiled)
                            return dstd::Unexpected(compiled.error());
                        return make_term(duql::TMatch{
                            std::move(*field), MatchOp::Icontains, n.text,
                            n.negated, dstd::nullopt, std::move(*compiled)});
                    }
                    if (quant_ > 0) return fail(e.span, NESTED_QUANTIFIER);
                    auto field = field_term(n.path, e.span);
                    if (!field) return dstd::Unexpected(field.error());
                    auto compiled =
                        compile(MatchOp::Icontains, n.text, dstd::nullopt, e);
                    if (!compiled) return dstd::Unexpected(compiled.error());
                    return make_term(duql::TQuant{
                        false, std::move(*field),
                        or_false(make_term(duql::TMatch{
                            element(), MatchOp::Icontains, n.text, n.negated,
                            dstd::nullopt, std::move(*compiled)}))});
                } else if constexpr (std::is_same_v<T, Between>) {
                    auto matched =
                        any_match(*n.subject, [&]() -> Result<duql::TermPtr> {
                            auto low = operand(*n.low, *n.subject);
                            if (!low) return dstd::Unexpected(low.error());
                            auto high = operand(*n.high, *n.subject);
                            if (!high) return dstd::Unexpected(high.error());
                            return make_term(
                                duql::TBetween{element(), std::move(*low),
                                               std::move(*high), n.negated});
                        });
                    if (!matched) return dstd::Unexpected(matched.error());
                    if (*matched) return std::move(**matched);
                    auto subject = term(*n.subject);
                    if (!subject) return dstd::Unexpected(subject.error());
                    auto low = operand(*n.low, *n.subject);
                    if (!low) return dstd::Unexpected(low.error());
                    auto high = operand(*n.high, *n.subject);
                    if (!high) return dstd::Unexpected(high.error());
                    return make_term(
                        duql::TBetween{std::move(*subject), std::move(*low),
                                       std::move(*high), n.negated});
                } else if constexpr (std::is_same_v<T, Like>) {
                    const MatchOp op = n.icase ? MatchOp::Ilike : MatchOp::Like;
                    const auto esc = escape_char(n.escape, e.span);
                    if (!esc) return dstd::Unexpected(esc.error());
                    auto matched =
                        any_match(*n.subject, [&]() -> Result<duql::TermPtr> {
                            auto compiled = compile(op, n.pattern, *esc, e);
                            if (!compiled)
                                return dstd::Unexpected(compiled.error());
                            return make_term(duql::TMatch{
                                element(), op, n.pattern, n.negated, *esc,
                                std::move(*compiled)});
                        });
                    if (!matched) return dstd::Unexpected(matched.error());
                    if (*matched) return std::move(**matched);
                    auto subject = term(*n.subject);
                    if (!subject) return dstd::Unexpected(subject.error());
                    auto compiled = compile(op, n.pattern, *esc, e);
                    if (!compiled) return dstd::Unexpected(compiled.error());
                    return make_term(duql::TMatch{std::move(*subject), op,
                                                  n.pattern, n.negated, *esc,
                                                  std::move(*compiled)});
                } else if constexpr (std::is_same_v<T, Is>) {
                    auto subject = term(*n.subject);
                    if (!subject) return dstd::Unexpected(subject.error());
                    return make_term(
                        duql::TIs{std::move(*subject), n.missing, n.negated});
                } else if constexpr (std::is_same_v<T, Arrow>) {
                    auto rows = arrow(n, e.span, false);
                    if (!rows) return dstd::Unexpected(rows.error());
                    return make_term(std::move(*rows));
                } else if constexpr (std::is_same_v<T, Call>) {
                    return call_term(n, e.span);
                } else if constexpr (std::is_same_v<T, Over>) {
                    return over_term(n, e.span);
                } else if constexpr (std::is_same_v<T, List>) {
                    duql::TList t;
                    for (const auto& item : n.items) {
                        auto value = term(*item);
                        if (!value) return dstd::Unexpected(value.error());
                        t.items.push_back(std::move(*value));
                    }
                    return make_term(std::move(t));
                } else if constexpr (std::is_same_v<T, Tuple>) {
                    return fail(e.span,
                                "a tuple stands only before 'in (from ...)' or "
                                "'->'");
                } else if constexpr (std::is_same_v<T, Index>) {
                    const auto* base = std::get_if<Path>(&n.base->node);
                    auto field = field_term(*base, e.span);
                    if (!field) return dstd::Unexpected(field.error());
                    auto index = term(*n.index);
                    if (!index) return dstd::Unexpected(index.error());
                    return make_term(
                        duql::TIndex{std::move(*field), std::move(*index)});
                } else {
                    auto sub = scalar(n, e.span);
                    if (!sub) return dstd::Unexpected(sub.error());
                    return make_term(std::move(*sub));
                }
            },
            e.node);
    }

    static constexpr const char* NESTED_QUANTIFIER =
        "a quantifier inside another quantifier is not supported";

    static duql::TermPtr element() {
        duql::TField f;
        f.root = duql::FieldRoot::Element;
        return dstd::make_unique<const duql::Term>(duql::Term{std::move(f)});
    }

    // `t ?? false`: the legacy any() leaf counts an unknown element as false.
    duql::TermPtr or_false(duql::TermPtr t) const {
        return make_term(duql::TBinary{duql::TermOp::Coalesce, std::move(t),
                                       make_term(duql::TConst{false})});
    }

    Result<duql::TermPtr> field_term(const Path& p, Span span) const {
        if (agg_)
            return fail(span,
                        "a field in a 'group' or 'agg' entry stands inside an "
                        "aggregate, such as 'sum(dur)'");
        if (p.root != PathRoot::Record) {
            if (quant_ == 0) return reject_root(p, span);
            if (p.root == PathRoot::Current) {
                duql::TField f;
                f.root = duql::FieldRoot::Element;
                f.steps = p.steps;
                return make_term(std::move(f));
            }
            auto t = field_term(Path{PathRoot::Record, p.steps}, span);
            if (!t) return dstd::Unexpected(t.error());
            auto f = std::get<duql::TField>((*t)->node);
            f.root = duql::FieldRoot::Outer;
            return make_term(std::move(f));
        }
        const std::size_t neg = negative_at(p);
        duql::TField f;
        auto base = steps_text(p, span, neg);
        if (!base) return dstd::Unexpected(base.error());
        f.base = std::move(*base);
        if (neg < p.steps.size() && !p.steps[neg].key.empty()) {
            if (!f.base.empty()) f.base += '.';
            f.base += p.steps[neg].key;
        }
        f.steps = p.steps;
        f.neg_at = neg;
        return make_term(std::move(f));
    }

    Result<duql::TermPtr> binary_term(const Binary& b) const {
        switch (b.op) {
            case BinaryOp::Regex:
            case BinaryOp::Iregex:
            case BinaryOp::Nregex:
            case BinaryOp::Niregex: {
                const auto [op, negated] = regex_op(b.op);
                auto text = pattern(*b.right);
                if (!text) return dstd::Unexpected(text.error());
                dstd::String p = std::move(*text);
                auto built = compile(op, p, dstd::nullopt, *b.right);
                if (!built) return dstd::Unexpected(built.error());
                auto compiled = std::move(*built);
                auto matched =
                    any_match(*b.left, [&]() -> Result<duql::TermPtr> {
                        return make_term(duql::TMatch{element(), op, p, negated,
                                                      dstd::nullopt, compiled});
                    });
                if (!matched) return dstd::Unexpected(matched.error());
                if (*matched) return std::move(**matched);
                auto subject = term(*b.left);
                if (!subject) return dstd::Unexpected(subject.error());
                return make_term(
                    duql::TMatch{std::move(*subject), op, std::move(p), negated,
                                 dstd::nullopt, std::move(compiled)});
            }
            case BinaryOp::Eq:
            case BinaryOp::Ne:
            case BinaryOp::Lt:
            case BinaryOp::Le:
            case BinaryOp::Gt:
            case BinaryOp::Ge: {
                const auto left_null = reject_null(*b.left);
                if (!left_null) return dstd::Unexpected(left_null.error());
                const auto right_null = reject_null(*b.right);
                if (!right_null) return dstd::Unexpected(right_null.error());
                auto q = quantified(b);
                if (!q) return dstd::Unexpected(q.error());
                if (*q) return std::move(**q);
                if (std::holds_alternative<Arrow>(b.left->node)) {
                    auto t = any_match(*b.left, [&]() -> Result<duql::TermPtr> {
                        auto right = operand(*b.right, *b.left);
                        if (!right) return dstd::Unexpected(right.error());
                        return make_term(duql::TBinary{term_op(b.op), element(),
                                                       std::move(*right)});
                    });
                    if (!t) return dstd::Unexpected(t.error());
                    if (*t) return std::move(**t);
                } else {
                    auto t =
                        any_match(*b.right, [&]() -> Result<duql::TermPtr> {
                            auto left = operand(*b.left, *b.right);
                            if (!left) return dstd::Unexpected(left.error());
                            return make_term(duql::TBinary{
                                term_op(b.op), std::move(*left), element()});
                        });
                    if (!t) return dstd::Unexpected(t.error());
                    if (*t) return std::move(**t);
                }
                break;
            }
            default:
                break;
        }
        auto left = operand(*b.left, *b.right);
        if (!left) return dstd::Unexpected(left.error());
        auto right = operand(*b.right, *b.left);
        if (!right) return dstd::Unexpected(right.error());
        return make_term(
            duql::TBinary{term_op(b.op), std::move(*left), std::move(*right)});
    }

    static const Call* quantified_side(const Expr& e) {
        const auto* c = std::get_if<Call>(&e.node);
        if (c && (c->name == "any" || c->name == "all") &&
            c->args.size() == 1 && c->args[0].name.empty())
            return c;
        return nullptr;
    }

    // `any(p) op v` as `any(p, (. op v) ?? false)`, and `all(p) op v` as
    // `all(p, . op v)`; nullopt when neither side is quantified.
    Result<dstd::Optional<duql::TermPtr>> quantified(const Binary& b) const {
        const Call* l = quantified_side(*b.left);
        const Call* r = quantified_side(*b.right);
        if (!l && !r) return dstd::Optional<duql::TermPtr>{};
        const Call* c = l ? l : r;
        if (l && r)
            return fail(c->args[0].value->span,
                        "compare a quantified array with a value");
        if (quant_ > 0) return fail(c->args[0].value->span, NESTED_QUANTIFIER);
        const Expr& other = l ? *b.right : *b.left;
        auto value = term(other);
        if (!value) return dstd::Unexpected(value.error());
        duql::TermPtr cond =
            l ? make_term(
                    duql::TBinary{term_op(b.op), element(), std::move(*value)})
              : make_term(
                    duql::TBinary{term_op(b.op), std::move(*value), element()});
        const bool all = c->name == "all";
        if (!all) cond = or_false(std::move(cond));
        auto subject = term(*c->args[0].value);
        if (!subject) return dstd::Unexpected(subject.error());
        return dstd::Optional<duql::TermPtr>(
            make_term(duql::TQuant{all, std::move(*subject), std::move(cond)}));
    }

    Result<duql::TermPtr> quantifier(const Call& c, Span span) const {
        if (c.args.size() == 1)
            return fail(span,
                        "'" + c.name + "(p)' compares with a value, as in '" +
                            c.name + "(p) == v'; with a condition write '" +
                            c.name + "(p, e)'");
        if (c.args.size() != 2)
            return fail(span, arity(c.name, 2, 2, c.args.size()));
        for (const auto& a : c.args)
            if (!a.name.empty())
                return fail(a.value->span, "'" + c.name +
                                               "' takes no named argument '" +
                                               a.name + "'");
        if (quant_ > 0) return fail(span, NESTED_QUANTIFIER);
        auto subject = term(*c.args[0].value);
        if (!subject) return dstd::Unexpected(subject.error());
        ++quant_;
        auto cond = term(*c.args[1].value);
        --quant_;
        if (!cond) return dstd::Unexpected(cond.error());
        return make_term(duql::TQuant{c.name == "all", std::move(*subject),
                                      std::move(*cond)});
    }

    Result<duql::TermPtr> call_term(const Call& c, Span span) const {
        auto timed = time_call(c, span);
        if (!timed) return dstd::Unexpected(timed.error());
        if (*timed) return std::move(**timed);
        if (c.name == "any" || c.name == "all") return quantifier(c, span);
        auto windowed = window_call(c, span);
        if (!windowed) return dstd::Unexpected(windowed.error());
        if (*windowed) return std::move(**windowed);
        auto aggregated = entry_aggregate(c, span);
        if (!aggregated) return dstd::Unexpected(aggregated.error());
        if (*aggregated) return std::move(**aggregated);
        const duql::FnInfo* info = nullptr;
        for (const auto& f : duql::FUNCTIONS)
            if (f.name == c.name) info = &f;
        if (!info && aggregate_info(c.name))
            return fail(span, "'" + c.name +
                                  "' is an aggregate; it stands in a 'group' "
                                  "or 'agg' block");
        if (!info && c.name.find('.') != dstd::String::npos)
            return plugin_call(c, span);
        if (!info) return fail(span, "Unknown function '" + c.name + "'");
        const std::size_t n = c.args.size();
        if (info->fn == duql::Fn::Case && n % 2 == 0)
            return fail(span,
                        "'case' takes pairs of a condition and a value, then "
                        "a default: an odd number of arguments, got " +
                            dstd::int_text(n));
        if (n < info->min_args ||
            (info->max_args != duql::VARIADIC && n > info->max_args))
            return fail(span,
                        arity(c.name, info->min_args,
                              info->max_args == duql::VARIADIC ? SIZE_MAX
                                                               : info->max_args,
                              n));
        duql::TCall t{info->fn, {}, nullptr, nullptr};
        for (const auto& a : c.args) {
            if (!a.name.empty())
                return fail(a.value->span, "'" + c.name +
                                               "' takes no named argument '" +
                                               a.name + "'");
            auto arg = term(*a.value);
            if (!arg) return dstd::Unexpected(arg.error());
            t.args.push_back(std::move(*arg));
        }
        if (info->fn == duql::Fn::Exists &&
            !std::holds_alternative<duql::TField>(t.args[0]->node))
            return fail(c.args[0].value->span, "exists() takes a path");
        if (info->fn == duql::Fn::Join) {
            const auto separator =
                pattern(*c.args[1].value, "join() takes a string separator");
            if (!separator) return dstd::Unexpected(separator.error());
        }
        if (info->fn == duql::Fn::Extract) {
            const Expr& at = *c.args[1].value;
            const auto regex = pattern(at);
            if (!regex) return dstd::Unexpected(regex.error());
            auto compiled = compile(MatchOp::Regex, *regex, dstd::nullopt, at);
            if (!compiled) return dstd::Unexpected(compiled.error());
            t.pattern = std::move(*compiled);
            const std::size_t groups = duql::capture_count(*t.pattern);
            if (t.args.size() > 2)
                if (const auto* g = std::get_if<duql::TConst>(&t.args[2]->node))
                    if (const auto* want =
                            std::get_if<std::uint64_t>(&g->value);
                        want && *want > groups)
                        return fail(c.args[2].value->span,
                                    "extract() group " + dstd::int_text(*want) +
                                        " is past the pattern's " +
                                        dstd::int_text(groups) + " groups");
        }
        if (info->fn == duql::Fn::RegexReplace) {
            const Expr& at = *c.args[1].value;
            const Expr& to = *c.args[2].value;
            const auto regex = pattern(at);
            if (!regex) return dstd::Unexpected(regex.error());
            auto compiled = compile(MatchOp::Regex, *regex, dstd::nullopt, at);
            if (!compiled) return dstd::Unexpected(compiled.error());
            t.pattern = std::move(*compiled);
            const auto text =
                pattern(to, "regex_replace() takes a string replacement");
            if (!text) return dstd::Unexpected(text.error());
            auto sub = duql::compile_substitution(*t.pattern, *text);
            if (!sub)
                return fail(to.span, "Invalid replacement at offset " +
                                         dstd::int_text(sub.error().offset) +
                                         ": " + sub.error().message);
            t.substitution =
                dstd::make_shared<const duql::Substitution>(std::move(*sub));
        }
        if (info->fn == duql::Fn::DatePart ||
            info->fn == duql::Fn::FormatTime) {
            const dstd::String name = c.name + "()";
            const auto unit = unit_of(*c.args[0].value);
            if (!unit) return dstd::Unexpected(unit.error());
            if (!*unit)
                return fail(c.args[0].value->span,
                            name +
                                " needs a time: a field with a time or "
                                "duration role, or as_time(x, unit)");
            t.ns_per_unit = **unit;
            const Expr& at = *c.args[1].value;
            const auto text =
                pattern(at, info->fn == duql::Fn::DatePart
                                ? "date_part() takes a string part"
                                : "format_time() takes a string format");
            if (!text) return dstd::Unexpected(text.error());
            if (info->fn == duql::Fn::DatePart) {
                common::DatePart part;
                if (!common::parse_date_part(*text, &part))
                    return fail(at.span,
                                "Unknown date part '" + *text +
                                    "'; use year, month, day, hour, minute, "
                                    "second, millisecond, microsecond, "
                                    "nanosecond, day_of_week, day_of_year, "
                                    "quarter, iso_week or iso_year");
                t.part = static_cast<std::int32_t>(part);
            } else if (const auto bad = common::invalid_time_format(*text);
                       !bad.empty()) {
                return fail(at.span, "Unknown time format directive '" +
                                         dstd::String(bad) +
                                         "' in format_time()");
            }
            t.args[1] = make_term(duql::TConst{*text});
        }
        return make_term(std::move(t));
    }

    Result<duql::LiteralValue> bound(const Param& p, Span span) const {
        const auto it = params_.find(p.name);
        if (it == params_.end())
            return fail(span, "No value bound to parameter $" + p.name);
        if (const auto* v = std::get_if<duql::LiteralValue>(&it->second))
            return *v;
        return fail(span, "$" + p.name +
                              " holds a list; a list parameter goes after "
                              "'in'");
    }

    // A literal, a parameter, or a negated numeric literal, as the value of a
    // query node.
    Result<dstd::Optional<LiteralNode>> literal(const Expr& e) const {
        if (const auto* p = std::get_if<Param>(&e.node)) {
            auto v = bound(*p, e.span);
            if (!v) return dstd::Unexpected(v.error());
            return dstd::Optional<LiteralNode>(LiteralNode{std::move(*v)});
        }
        if (const auto* u = std::get_if<Unary>(&e.node)) {
            if (u->op != UnaryOp::Neg) return dstd::Optional<LiteralNode>{};
            const auto* lit = std::get_if<Literal>(&u->operand->node);
            if (!lit) return dstd::Optional<LiteralNode>{};
            if (const auto* v = std::get_if<std::uint64_t>(&lit->value)) {
                if (*v == 0)
                    return dstd::Optional<LiteralNode>(
                        LiteralNode{std::uint64_t{0}});
                if (*v > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max()))
                    return fail(e.span,
                                "Invalid integer: '-" + lit->number_text + "'");
                return dstd::Optional<LiteralNode>(
                    LiteralNode{-static_cast<std::int64_t>(*v)});
            }
            if (const auto* d = std::get_if<double>(&lit->value))
                return dstd::Optional<LiteralNode>(LiteralNode{-*d});
            return dstd::Optional<LiteralNode>{};
        }
        const auto* lit = std::get_if<Literal>(&e.node);
        if (!lit) return dstd::Optional<LiteralNode>{};
        return std::visit(
            [&](const auto& v) -> dstd::Optional<LiteralNode> {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, Null>)
                    return dstd::nullopt;
                else
                    return LiteralNode{v};
            },
            lit->value);
    }
};

}  // namespace

namespace {

// The macros in scope for a query with `defs`, after the source's `source`.
dftracer::utils::dstd::Expected<MacroScopes, duql::DuqlError> scopes_of(
    dstd::Vector<Def>& defs, dstd::StringView text,
    dstd::Vector<duql::Macro> source) {
    auto own = duql::macros_of(defs, text, "the query");
    if (!own) return dftracer::utils::dstd::Unexpected(own.error());
    auto from_path = duql::path_macros();
    if (!from_path)
        return dftracer::utils::dstd::Unexpected(
            syntax::make_error(text, 0, 0, from_path.error().c_str()));
    MacroScopes scopes{std::move(*own), std::move(source),
                       std::move(*from_path)};
    for (const auto& scope : scopes)
        for (const auto& m : scope) {
            if (syntax::is_stage_word(m.name))
                return dftracer::utils::dstd::Unexpected(
                    make_error(text, 0, 0,
                               "macro '" + m.name + "' in " + m.origin +
                                   " has the name of a stage"));
            if (builtin_name(m.name))
                return dftracer::utils::dstd::Unexpected(make_error(
                    text, 0, 0,
                    "macro '" + m.name + "' in " + m.origin +
                        " has the name of a built-in function; rename it"));
        }
    return scopes;
}

dftracer::utils::dstd::Expected<void, duql::DuqlError> expand(
    syntax::Pipeline& p, const MacroScopes& scopes, dstd::StringView text) {
    return duql::expand_macros(p, scopes, text, builtin_name);
}

// Fills `x in $p` from the list and `x like $p` from the string bound to `p`.
dftracer::utils::dstd::Expected<void, duql::DuqlError> bind_params(
    syntax::Pipeline& p, const duql::Params& params, dstd::StringView text) {
    dstd::Optional<duql::DuqlError> error;
    auto value = [&](const dstd::String& name,
                     Span span) -> const duql::ParamValue* {
        const auto it = params.find(name);
        if (it == params.end()) {
            error = make_error(text, span.offset, span.length,
                               "No value bound to parameter $" + name);
            return nullptr;
        }
        return &it->second;
    };
    dstd::Function<void(ExprPtr&)> visit = [&](ExprPtr& e) {
        if (!e || error) return;
        if (auto* in = std::get_if<In>(&e->node);
            in && !in->list_param.empty()) {
            const auto* v = value(in->list_param, e->span);
            if (!v) return;
            const auto* list = std::get_if<dstd::Vector<duql::LiteralValue>>(v);
            if (!list) {
                error = make_error(text, e->span.offset, e->span.length,
                                   "'in $" + in->list_param +
                                       "' needs a list bound to $" +
                                       in->list_param);
                return;
            }
            in->list.clear();
            for (const auto& item : *list) {
                auto lit = dstd::make_unique<Expr>();
                lit->span = e->span;
                lit->node =
                    Literal{std::visit(
                                [](const auto& x) -> decltype(Literal::value) {
                                    return x;
                                },
                                item),
                            {}};
                in->list.push_back(std::move(lit));
            }
        }
        if (auto* l = std::get_if<Like>(&e->node);
            l && !l->pattern_param.empty()) {
            const auto* v = value(l->pattern_param, e->span);
            if (!v) return;
            const auto* lit = std::get_if<duql::LiteralValue>(v);
            const auto* s = lit ? std::get_if<dstd::String>(lit) : nullptr;
            if (!s) {
                error = make_error(text, e->span.offset, e->span.length,
                                   "'like $" + l->pattern_param +
                                       "' needs a string bound to $" +
                                       l->pattern_param);
                return;
            }
            l->pattern = *s;
        }
        syntax::children(*e, visit);
        if (error) return;
        if (auto* ix = std::get_if<Index>(&e->node)) {
            const auto* prm = std::get_if<Param>(&ix->index->node);
            auto* base = std::get_if<Path>(&ix->base->node);
            if (!prm || !base) return;
            const auto* v = value(prm->name, ix->index->span);
            if (!v) return;
            const auto* lit = std::get_if<duql::LiteralValue>(v);
            dstd::Optional<std::int64_t> at;
            if (lit) {
                if (const auto* i = std::get_if<std::int64_t>(lit))
                    at = *i;
                else if (const auto* u = std::get_if<std::uint64_t>(lit);
                         u && *u <= std::uint64_t{INT64_MAX})
                    at = static_cast<std::int64_t>(*u);
            }
            if (!at) {
                error = make_error(text, ix->index->span.offset,
                                   ix->index->span.length,
                                   "an array index parameter holds an "
                                   "integer; $" +
                                       prm->name + " does not");
                return;
            }
            PathStep step;
            step.index = *at;
            base->steps.push_back(std::move(step));
            ExprPtr folded = std::move(ix->base);
            folded->span = e->span;
            e = std::move(folded);
        }
    };
    syntax::each_slot(p, visit);
    if (error) return dftracer::utils::dstd::Unexpected(std::move(*error));
    return {};
}

}  // namespace

dftracer::utils::dstd::Expected<duql::Program, duql::DuqlError> compile_program(
    dstd::StringView text, const duql::Params& params, const duql::Roles* roles,
    dstd::StringView source, const duql::PluginCatalog* plugins,
    const LeafPaths* leaves) {
    auto tree = syntax::parse(text);
    if (!tree) return dftracer::utils::dstd::Unexpected(tree.error());
    dstd::Vector<Def> defs = duql::take_defs(tree->decls);
    dstd::Vector<Let> rowsets;
    dstd::Vector<duql::Macro> source_macros;
    bool fallback = false;
    const dstd::String source_text =
        source.empty() ? dstd::String()
                       : "source s {\n" + dstd::String(source) + "\n}";
    dstd::Optional<syntax::Program> source_tree;
    if (!source.empty()) {
        auto st = syntax::parse(source_text);
        if (!st) return dftracer::utils::dstd::Unexpected(st.error());
        source_tree = std::move(*st);
        auto& decl = std::get<SourceDecl>(source_tree->decls.front());
        auto m = duql::macros_of(decl.defs, source_text, "the source");
        if (!m) return dftracer::utils::dstd::Unexpected(m.error());
        source_macros = std::move(*m);
        fallback = duql::args_fallback(decl.defs);
        for (auto& r : decl.rowsets)
            rowsets.push_back({r.name, std::move(r.pipeline)});
    }
    auto scopes = scopes_of(defs, text, source_macros);
    if (!scopes) return dftracer::utils::dstd::Unexpected(scopes.error());
    auto from_path = duql::path_macros();
    if (!from_path)
        return dftracer::utils::dstd::Unexpected(
            syntax::make_error(text, 0, 0, from_path.error().c_str()));
    const MacroScopes source_scopes{source_macros, std::move(*from_path)};
    for (auto& r : rowsets)
        if (auto ok = expand(*r.pipeline, source_scopes, source_text); !ok)
            return dftracer::utils::dstd::Unexpected(ok.error());
    for (auto& d : tree->decls)
        if (auto* let = std::get_if<Let>(&d)) {
            if (auto ok = expand(*let->pipeline, *scopes, text); !ok)
                return dftracer::utils::dstd::Unexpected(ok.error());
            if (auto ok = bind_params(*let->pipeline, params, text); !ok)
                return dftracer::utils::dstd::Unexpected(ok.error());
        }
    if (tree->pipeline) {
        if (auto ok = expand(*tree->pipeline, *scopes, text); !ok)
            return dftracer::utils::dstd::Unexpected(ok.error());
        if (auto ok = bind_params(*tree->pipeline, params, text); !ok)
            return dftracer::utils::dstd::Unexpected(ok.error());
    }
    if (leaves) {
        for (auto& d : tree->decls)
            if (auto* let = std::get_if<Let>(&d))
                if (auto ok = expand_wildcards(*let->pipeline, *leaves,
                                               fallback, text);
                    !ok)
                    return dftracer::utils::dstd::Unexpected(ok.error());
        if (tree->pipeline)
            if (auto ok =
                    expand_wildcards(*tree->pipeline, *leaves, fallback, text);
                !ok)
                return dftracer::utils::dstd::Unexpected(ok.error());
    }
    auto out = Lowering(params, text, roles, plugins).program(*tree, rowsets);
    if (!out) return dftracer::utils::dstd::Unexpected(out.error());
    out->args_fallback = fallback;
    return out;
}

void for_each_pipeline_term(const duql::Pipeline& p,
                            const dstd::Function<void(const duql::Term&)>& fn) {
    auto on = [&](const duql::TermRef& t) {
        if (t) duql::for_each_term(*t, fn);
    };
    auto items = [&](const dstd::Vector<duql::PipelineItem>& v) {
        for (const auto& it : v) on(it.term);
    };
    auto keys = [&](const dstd::Vector<duql::PipelineSortKey>& v) {
        for (const auto& k : v) on(k.key.term);
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
                    } else if constexpr (std::is_same_v<
                                             T, duql::detail::ExprLeaf>) {
                        duql::for_each_term(*x.term, fn);
                    }
                },
                n.data);
        };
    if (p.filter) node(*p.filter);
    for (const auto& stage : p.stages)
        std::visit(
            [&](const auto& s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, duql::PipelineWhere>) {
                    on(s.condition);
                } else if constexpr (std::is_same_v<T, duql::PipelineSelect> ||
                                     std::is_same_v<T, duql::PipelineDerive> ||
                                     std::is_same_v<T,
                                                    duql::PipelineDistinct>) {
                    items(s.items);
                } else if constexpr (std::is_same_v<T, duql::PipelineSort>) {
                    keys(s.keys);
                } else if constexpr (std::is_same_v<T, duql::PipelineTakeBy>) {
                    items(s.keys);
                    keys(s.order);
                } else if constexpr (std::is_same_v<T, duql::PipelineGroup>) {
                    items(s.keys);
                    for (const auto& a : s.aggs) {
                        on(a.arg);
                        on(a.by);
                    }
                } else if constexpr (std::is_same_v<T,
                                                    duql::PipelineTimeRange>) {
                    on(s.condition);
                } else if constexpr (std::is_same_v<T, duql::PipelineBucket>) {
                    on(s.key);
                } else if constexpr (std::is_same_v<T, duql::PipelineWindow>) {
                    items(s.keys);
                    keys(s.order);
                    for (const auto& c : s.calls) {
                        on(c.arg);
                        on(c.by);
                    }
                    items(s.items);
                } else if constexpr (std::is_same_v<T, duql::PipelinePivot>) {
                    on(s.key.term);
                    for (const auto& a : s.aggs) {
                        on(a.arg);
                        on(a.by);
                    }
                } else if constexpr (std::is_same_v<T, duql::PipelineLookup>) {
                    for (const auto& k : s.keys) on(k.first.term);
                    if (const auto* a =
                            std::get_if<duql::PipelineAsof>(&s.mode))
                        on(a->time.term);
                    if (const auto* o =
                            std::get_if<duql::PipelineOverlap>(&s.mode)) {
                        on(o->time.term);
                        on(o->duration.term);
                    }
                } else if constexpr (std::is_same_v<T, duql::PipelineUnion>) {
                    for_each_pipeline_term(*s.other, fn);
                }
            },
            stage);
}

dftracer::utils::dstd::Expected<duql::ParamValue, duql::DuqlError> parse_param(
    dstd::StringView text) {
    auto tree = syntax::parse(text);
    if (!tree) return dftracer::utils::dstd::Unexpected(tree.error());
    if (tree->decls.empty() && tree->pipeline &&
        tree->pipeline->sources.empty() && tree->pipeline->stages.size() == 1)
        if (const auto* w =
                std::get_if<Where>(&tree->pipeline->stages.front().node)) {
            const duql::Params none;
            const Lowering l(none, text);
            if (const auto* list = std::get_if<List>(&w->condition->node)) {
                dstd::Vector<duql::LiteralValue> out;
                bool ok = true;
                for (const auto& item : list->items) {
                    auto lit = l.value_of(*item);
                    if (!lit)
                        return dftracer::utils::dstd::Unexpected(lit.error());
                    if (!*lit) {
                        ok = false;
                        break;
                    }
                    out.push_back(std::move((*lit)->value));
                }
                if (ok) return duql::ParamValue{std::move(out)};
            } else {
                auto lit = l.value_of(*w->condition);
                if (!lit) return dftracer::utils::dstd::Unexpected(lit.error());
                if (*lit) return duql::ParamValue{std::move((*lit)->value)};
            }
        }
    return dftracer::utils::dstd::Unexpected(make_error(
        text, 0, text.size(),
        "a parameter value must be a number, a string, true, false or a "
        "list of them"));
}

dftracer::utils::dstd::Expected<duql::detail::QueryNodePtr, duql::DuqlError>
lower_filter(syntax::Program program, const duql::Params& params,
             dstd::StringView source) {
    dstd::Vector<Def> defs = duql::take_defs(program.decls);
    auto scopes = scopes_of(defs, source, {});
    if (!scopes) return dftracer::utils::dstd::Unexpected(scopes.error());
    if (program.pipeline) {
        if (auto ok = expand(*program.pipeline, *scopes, source); !ok)
            return dftracer::utils::dstd::Unexpected(ok.error());
        if (auto ok = bind_params(*program.pipeline, params, source); !ok)
            return dftracer::utils::dstd::Unexpected(ok.error());
    }
    return Lowering(params, source).filter(program);
}

}  // namespace dftracer::utils::duql
