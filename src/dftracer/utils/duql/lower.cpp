#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/duql/fields.h>
#include <dftracer/utils/duql/lower.h>
#include <dftracer/utils/duql/macros.h>
#include <dftracer/utils/duql/pattern.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <dftracer/utils/duql/pipeline.h>
#include <dftracer/utils/duql/syntax/lexer.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/term.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace dftracer::utils::duql {

namespace {

using namespace syntax;

using duql::LiteralNode;
using duql::MatchOp;
using duql::QueryNodePtr;

struct Failure {
    duql::DuqlError error;
};

struct AggInfo {
    duql::AggFn fn;
    std::string_view name;
    std::uint8_t min_args;
    std::uint8_t max_args;
};

constexpr AggInfo AGGREGATES[] = {
    {duql::AggFn::COUNT, "count", 0, 1},
    {duql::AggFn::COUNT_IF, "count_if", 1, 1},
    {duql::AggFn::SUM, "sum", 1, 1},
    {duql::AggFn::MIN, "min", 1, 1},
    {duql::AggFn::MAX, "max", 1, 1},
    {duql::AggFn::MEAN, "mean", 1, 1},
    {duql::AggFn::VAR, "var", 1, 1},
    {duql::AggFn::STD, "std", 1, 1},
    {duql::AggFn::FIRST, "first", 1, 1},
    {duql::AggFn::LAST, "last", 1, 1},
    {duql::AggFn::QUANTILE, "quantile", 2, 2},
    {duql::AggFn::HISTOGRAM, "histogram", 1, 1},
    {duql::AggFn::BUSY, "busy", 0, 0},
    {duql::AggFn::CONCURRENCY, "concurrency", 0, 0},
    {duql::AggFn::UTILIZATION, "utilization", 0, 0},
    {duql::AggFn::ACTIVE, "active", 0, 0},
    {duql::AggFn::COUNT_DISTINCT, "count_distinct", 1, 1},
    {duql::AggFn::COLLECT, "collect", 1, 1},
    {duql::AggFn::ARGMAX, "arg_max", 2, 2},
    {duql::AggFn::ARGMIN, "arg_min", 2, 2},
    {duql::AggFn::SKETCH, "sketch", 1, 1},
    {duql::AggFn::MERGE, "merge", 1, 1},
};

struct WinInfo {
    duql::WinFn fn;
    std::string_view name;
    std::uint8_t min_args;
    std::uint8_t max_args;
    bool window_only;
};

constexpr WinInfo WINDOW_FUNCTIONS[] = {
    {duql::WinFn::ROW_NUMBER, "row_number", 0, 0, true},
    {duql::WinFn::RANK, "rank", 0, 0, true},
    {duql::WinFn::DENSE_RANK, "dense_rank", 0, 0, true},
    {duql::WinFn::LAG, "lag", 1, 2, true},
    {duql::WinFn::LEAD, "lead", 1, 2, true},
    {duql::WinFn::RUNNING_SUM, "running_sum", 1, 1, true},
    {duql::WinFn::RUNNING_COUNT, "running_count", 0, 0, true},
    {duql::WinFn::COUNT, "count", 0, 1, false},
    {duql::WinFn::SUM, "sum", 1, 1, false},
    {duql::WinFn::MIN, "min", 1, 1, false},
    {duql::WinFn::MAX, "max", 1, 1, false},
    {duql::WinFn::MEAN, "mean", 1, 1, false},
    {duql::WinFn::FIRST, "first", 1, 1, false},
    {duql::WinFn::LAST, "last", 1, 1, false},
    {duql::WinFn::VAR, "var", 1, 1, false},
    {duql::WinFn::STD, "std", 1, 1, false},
    {duql::WinFn::QUANTILE, "quantile", 2, 2, false},
    {duql::WinFn::HISTOGRAM, "histogram", 1, 1, false},
};

const WinInfo* window_info(std::string_view name) {
    for (const auto& w : WINDOW_FUNCTIONS)
        if (w.name == name) return &w;
    return nullptr;
}

bool listed(std::string_view name, const auto& names) {
    for (const auto& n : names)
        if (n == name) return true;
    return false;
}

const AggInfo* aggregate_info(std::string_view name) {
    for (const auto& a : AGGREGATES)
        if (a.name == name) return &a;
    return nullptr;
}

bool builtin_name(std::string_view name) {
    for (const auto& f : duql::FUNCTIONS)
        if (f.name == name) return true;
    return aggregate_info(name) || window_info(name) || name == "any" ||
           name == "all" || name == "bin" || name == "as_time" ||
           name == "to_seconds";
}

class Lowering {
    // One step of a flattened pipeline: a stage, or a union with a source.
    struct Step {
        const Stage* stage = nullptr;
        const From* source = nullptr;
    };

   public:
    Lowering(const duql::Params& params, std::string_view source,
             const duql::Roles* roles = nullptr)
        : params_(params), src_(source), roles_(roles) {}

    QueryNodePtr filter(const syntax::Program& q) {
        view_ = false;
        if (!q.decls.empty())
            fail(Span{}, declaration_message(q.decls.front()));
        if (!q.pipeline) fail(Span{}, "Expected a filter");
        if (!q.pipeline->sources.empty())
            fail(Span{}, "'from' runs only on a View pipeline");
        const syntax::Pipeline& p = *q.pipeline;
        if (p.stages.empty()) fail(Span{}, "Expected a filter");
        for (std::size_t i = 0; i < p.stages.size(); ++i) {
            const Stage& s = p.stages[i];
            if (i == 0 && std::holds_alternative<Where>(s.node)) continue;
            if (pipeline_stage(s))
                fail(s.span, "'" + stage_name(s) +
                                 "' is a pipeline stage; this takes a filter");
            fail(s.span, stage_message(s));
        }
        return condition(*std::get<Where>(p.stages.front().node).condition);
    }

    // `rowsets` are the source's, which the query's lets follow.
    duql::Program program(const syntax::Program& q,
                          const std::vector<Let>& rowsets) {
        std::vector<const Let*> all;
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
            if (!let) fail(Span{}, declaration_message(d));
            if (let->name == "all" || let->name == "data")
                fail(Span{}, "'" + let->name +
                                 "' names a built-in row set; give the let "
                                 "another name");
            if (listed(let->name, rowsets_))
                fail(Span{}, "'" + let->name +
                                 "' names a row set of the source; give the "
                                 "let another name");
            if (listed(let->name, names_))
                fail(Span{}, "'" + let->name + "' is declared twice");
            names_.push_back(let->name);
            key_text_ += "let " + let->name + " = " +
                         syntax::to_text(*let->pipeline) + ";\n";
            all.push_back(let);
        }
        std::vector<std::string> bound;
        for (const auto& [name, value] : params_)
            bound.push_back(std::string(name) + "=" +
                            duql::term_text(*make_term(constant(value))) +
                            ";\n");
        std::sort(bound.begin(), bound.end());
        for (const auto& b : bound) key_text_ += b;
        for (const Let* let : all) {
            current_ = let->name;
            // A source's row sets read every record unless they say `from`;
            // a query's lets, like the query, read `data`.
            const bool rowset = listed(let->name, rowsets_);
            const std::size_t side =
                add_side(let->name, *let->pipeline, false,
                         rowset ? duql::InputKind::ALL : duql::InputKind::DATA);
            prog_.sides[side].rowset = rowset;
            lets_.push_back({let, side});
        }
        current_.clear();
        if (!q.pipeline) fail(Span{}, "Expected a pipeline");
        duql::Input in;
        std::vector<Step> steps;
        flatten(*q.pipeline, duql::InputKind::DATA, in, steps);
        prog_.main = lower_steps(std::move(in), steps);
        return std::move(prog_);
    }

    // Whether `n` reads an ISO-8601 time role, which the raw record holds as
    // text.
    bool reads_text_time(const duql::QueryNode& n) const {
        if (!roles_ || !roles_->time_text) return false;
        if (duql::collect_fields(n).contains(roles_->time)) return true;
        return std::visit(
            [&](const auto& x) -> bool {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, duql::AndNode> ||
                              std::is_same_v<T, duql::OrNode>)
                    return reads_text_time(*x.left) ||
                           reads_text_time(*x.right);
                else if constexpr (std::is_same_v<T, duql::NotNode>)
                    return reads_text_time(*x.operand);
                else if constexpr (std::is_same_v<T, duql::ExprLeaf>) {
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
    duql::Pipeline lower_steps(duql::Input in,
                               const std::vector<Step>& steps) const {
        duql::Pipeline out;
        out.input = std::move(in);
        // Rows of a row set are no scan: every stage runs on them.
        const bool scan = out.input.kind != duql::InputKind::SIDE;
        std::size_t i = 0;
        for (; scan && i < steps.size(); ++i) {
            const auto* w = steps[i].stage
                                ? std::get_if<Where>(&steps[i].stage->node)
                                : nullptr;
            if (!w) break;
            auto node = condition(*w->condition);
            if (reads_text_time(*node)) break;
            out.filter = out.filter
                             ? duql::make_node(duql::AndNode{
                                   std::move(out.filter), std::move(node)})
                             : std::move(node);
        }
        if (out.filter) out.filter_text = duql::to_string(*out.filter);
        if (scan && i < steps.size() && steps[i].stage)
            if (const auto* sel = std::get_if<Select>(&steps[i].stage->node);
                sel && plain_paths(*sel)) {
                for (const auto& it : sel->items)
                    out.scan_select.push_back(path_text(
                        std::get<Path>(it.value->node), it.value->span));
                ++i;
            }
        const bool leading =
            i == steps.size() || !steps[i].stage ||
            !std::holds_alternative<Select>(steps[i].stage->node);
        std::optional<Span> bucket;
        bool open_pivot = false;
        for (std::size_t first = i; i < steps.size(); ++i) {
            if (!steps[i].stage) {
                if (open_pivot)
                    fail(Span{},
                         "a stage after 'pivot' needs its columns: list the "
                         "values with 'pivot k in [...]'");
                out.stages.push_back(union_of(*steps[i].source));
                continue;
            }
            const Stage& s = *steps[i].stage;
            if (open_pivot)
                fail(s.span,
                     "a stage after 'pivot' needs its columns: list the "
                     "values with 'pivot k in [...]'");
            if (const auto* pv = std::get_if<Pivot>(&s.node))
                open_pivot = pv->values.empty();
            if (std::holds_alternative<CallTree>(s.node) &&
                (i != first || !leading))
                fail(s.span, "'call_tree' follows only 'where' stages");
            if (std::holds_alternative<Bucket>(s.node)) {
                if (bucket)
                    fail(s.span, "a second 'bucket' before a 'group' or 'agg'");
                bucket = s.span;
            }
            if (std::holds_alternative<Group>(s.node) ||
                std::holds_alternative<Agg>(s.node))
                bucket.reset();
            out.stages.push_back(stage(s));
        }
        if (bucket) fail(*bucket, "'bucket' needs a 'group' or 'agg' after it");
        return out;
    }

    std::optional<LiteralNode> value_of(const Expr& e) const {
        if (std::holds_alternative<Param>(e.node)) return std::nullopt;
        return literal(e);
    }

   private:
    const duql::Params& params_;
    std::string_view src_;
    const duql::Roles* roles_;
    // While a window entry lowers: its calls, and whether a call's argument
    // is lowering.
    mutable std::vector<duql::PipelineWinCall>* win_ = nullptr;
    mutable bool win_arg_ = false;
    // Quantifier conditions being lowered.
    mutable int quant_ = 0;
    // Whether a condition lowers, where an arrow compared with a value holds
    // when any matching row makes it hold.
    mutable bool cond_ = false;
    // Sub-queries being lowered.
    mutable int sub_ = 0;
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
    std::vector<LetSide> lets_;
    std::vector<std::string> names_;
    std::vector<std::string> rowsets_;
    std::string current_;
    std::string key_text_;

    struct Flag {
        bool& flag;
        bool old;
        Flag(bool& f, bool v) : flag(f), old(f) { flag = v; }
        ~Flag() { flag = old; }
    };

    static std::string declaration_message(const Decl& d) {
        if (std::holds_alternative<Let>(d))
            return "'let' runs only on a View pipeline";
        if (std::holds_alternative<Def>(d))
            return "'def' runs only in a query, a source or a macro file";
        return "a 'source' belongs to a record schema; declare it in the "
               "schema's 'source'";
    }

    void need_view(Span span, const std::string& what) const {
        if (!view_)
            fail(span, what +
                           " reads other rows, which only a View pipeline "
                           "has; run it with View::duql");
    }

    // `p` as its input and steps: a `from` naming a let reads that let's
    // input and runs its stages first; each later source is a union step.
    void flatten(const syntax::Pipeline& p, duql::InputKind first,
                 duql::Input& in, std::vector<Step>& steps) const {
        if (p.sources.empty())
            in = {first, {}};
        else
            read(p.sources.front(), in, steps);
        for (std::size_t i = 1; i < p.sources.size(); ++i)
            steps.push_back({nullptr, &p.sources[i]});
        for (const auto& st : p.stages) steps.push_back({&st, nullptr});
    }

    void read(const From& f, duql::Input& in, std::vector<Step>& steps) const {
        if (const auto* prm = std::get_if<Param>(&f.name)) {
            const auto v = bound(*prm, Span{});
            const auto* path = std::get_if<std::string>(&v);
            if (!path)
                fail(Span{}, "'from $" + prm->name +
                                 "' reads a file: bind it to a path string");
            in = {duql::InputKind::FILE, *path};
            return;
        }
        const auto& name = std::get<std::string>(f.name);
        if (f.quoted) {
            in = {duql::InputKind::FILE, name};
        } else if (name == "all") {
            in = {duql::InputKind::ALL, {}};
        } else if (name == "data") {
            in = {duql::InputKind::DATA, {}};
        } else if (listed(name, rowsets_)) {
            in = {duql::InputKind::SIDE, {}, let_of(name, Span{}).side};
        } else {
            flatten(*let_of(name, Span{}).let->pipeline, duql::InputKind::DATA,
                    in, steps);
        }
    }

    // The let `name`, declared before this point.
    const LetSide& let_of(const std::string& name, Span span) const {
        for (const auto& l : lets_)
            if (l.let->name == name) return l;
        if (name == current_) fail(span, "'" + name + "' refers to itself");
        if (listed(name, names_))
            fail(span, "'" + name +
                           "' is declared after its use; declare a let "
                           "before what reads it");
        fail(span, "'" + name + "' is not a let or a row set of the source");
    }

    duql::PipelineUnion union_of(const From& f) const {
        duql::Input in;
        std::vector<Step> steps;
        read(f, in, steps);
        return {std::make_shared<const duql::Pipeline>(
            lower_steps(std::move(in), steps))};
    }

    duql::PipelineUnion union_of(const syntax::Pipeline& p) const {
        duql::Input in;
        std::vector<Step> steps;
        flatten(p, duql::InputKind::DATA, in, steps);
        return {std::make_shared<const duql::Pipeline>(
            lower_steps(std::move(in), steps))};
    }

    // Lowers `p` as the row set `name`, reading `first` unless it says
    // `from`.
    std::size_t add_side(std::string name, const syntax::Pipeline& p, bool sub,
                         duql::InputKind first) const {
        auto* win = win_;
        const bool win_arg = win_arg_;
        const int quant = quant_;
        const bool cond = cond_;
        win_ = nullptr;
        win_arg_ = false;
        quant_ = 0;
        cond_ = false;
        if (sub) ++sub_;
        duql::Input in;
        std::vector<Step> steps;
        flatten(p, first, in, steps);
        duql::Pipeline lowered = lower_steps(std::move(in), steps);
        if (sub) --sub_;
        win_ = win;
        win_arg_ = win_arg;
        quant_ = quant;
        cond_ = cond;
        std::string key = key_text_ + (sub ? "sub " : "let " + name + " ") +
                          syntax::to_text(p);
        prog_.sides.push_back(
            {std::move(name), std::move(lowered), std::move(key)});
        return prog_.sides.size() - 1;
    }

    // The number of columns a side gives, when its last stage fixes them.
    std::optional<std::size_t> width(std::size_t side) const {
        const duql::Pipeline& p = prog_.sides[side].pipeline;
        if (p.stages.empty()) {
            if (p.scan_select.empty()) return std::nullopt;
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
        return std::nullopt;
    }

    std::shared_ptr<duql::LookupSlot> slot() const {
        return std::make_shared<duql::LookupSlot>();
    }

    // The key terms of `e`: the items of a tuple, else `e`.
    std::vector<duql::TermPtr> key_terms(const Expr& e) const {
        std::vector<duql::TermPtr> out;
        if (const auto* tup = std::get_if<Tuple>(&e.node))
            for (const auto& it : tup->items) out.push_back(term(*it));
        else
            out.push_back(term(e));
        for (const auto& k : out)
            duql::for_each_term(*k, [&](const duql::Term& x) {
                if (const auto* f = std::get_if<duql::TField>(&x.node);
                    f && f->root != duql::FieldRoot::RECORD)
                    fail(e.span,
                         "a lookup key inside any() or all() reads the "
                         "record, not '.' or '^.'");
            });
        return out;
    }

    // The side a sub-query reads: a let's own when it is only `from let`.
    void side_of(const syntax::Pipeline& p, duql::TLookup& t) const {
        if (p.stages.empty() && p.sources.size() == 1)
            if (const auto* name = std::get_if<std::string>(&p.sources[0].name);
                name && !p.sources[0].quoted && *name != "all" &&
                *name != "data") {
                t.name = *name;
                t.side = let_of(*name, Span{}).side;
                return;
            }
        t.name = "__sub_" + std::to_string(subs_++);
        t.side = add_side(t.name, p, true, duql::InputKind::DATA);
    }

    duql::TLookup semi_join(const In& n, Span span) const {
        need_view(span, "a sub-query");
        duql::TLookup t;
        t.kind = duql::LookupKind::IN;
        t.negated = n.negated;
        t.keys = key_terms(*n.subject);
        side_of(*n.subquery, t);
        if (const auto w = width(t.side); w && *w != t.keys.size())
            fail(span, "the sub-query gives " + std::to_string(*w) +
                           " columns; 'in' compares " +
                           std::to_string(t.keys.size()));
        t.slot = slot();
        return t;
    }

    duql::TLookup scalar(const Subquery& n, Span span) const {
        need_view(span, "a sub-query");
        duql::TLookup t;
        t.kind = duql::LookupKind::SCALAR;
        side_of(*n.pipeline, t);
        if (const auto w = width(t.side); w && *w != 1)
            fail(span,
                 "a sub-query in an expression gives one column; this "
                 "one gives " +
                     std::to_string(*w));
        t.slot = slot();
        return t;
    }

    // `k -> s(id).path`; with `all`, every matching row's value.
    duql::TLookup arrow(const Arrow& a, Span span, bool all) const {
        need_view(span, "'->'");
        duql::TLookup t;
        t.kind = duql::LookupKind::ARROW;
        t.all = all;
        t.name = a.rowset;
        t.side = let_of(a.rowset, span).side;
        const auto* tup = std::get_if<Tuple>(&a.key->node);
        if (a.target_key) {
            if (tup)
                fail(span,
                     "a tuple key finds the row set's columns by its "
                     "paths; drop '(...)' after '" +
                         a.rowset + "'");
            t.target.push_back(path_text(*a.target_key, span));
        }
        auto name_of = [&](const Expr& k) -> std::string {
            if (const auto* p = std::get_if<Path>(&k.node))
                return path_text(*p, k.span);
            if (const auto* inner = std::get_if<Arrow>(&k.node))
                return path_text(inner->path, k.span);
            fail(k.span, "name the key column of '" + a.rowset +
                             "' for this key: write 'k -> " + a.rowset +
                             "(id).path'");
        };
        t.keys = key_terms(*a.key);
        if (tup)
            for (const auto& it : tup->items) t.target.push_back(name_of(*it));
        else if (!a.target_key)
            t.target.push_back(name_of(*a.key));
        t.column = path_text(a.path, span);
        t.slot = slot();
        return t;
    }

    // In a condition, `subject op ...` with an arrow subject as a
    // quantifier over the matching rows' values; `cond` reads the value as
    // `.`.
    template <class Cond>
    std::optional<duql::TermPtr> any_match(const Expr& subject,
                                           Cond&& cond) const {
        const auto* a = std::get_if<Arrow>(&subject.node);
        if (!cond_ || !a) return std::nullopt;
        return make_term(duql::TQuant{
            false, make_term(arrow(*a, subject.span, true)), cond()});
    }

    static bool pipeline_stage(const Stage& s) {
        return std::visit(
            [](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                return std::is_same_v<T, Where> || std::is_same_v<T, Derive> ||
                       std::is_same_v<T, Select> || std::is_same_v<T, Drop> ||
                       std::is_same_v<T, Rename> ||
                       std::is_same_v<T, Distinct> ||
                       std::is_same_v<T, syntax::Sort> ||
                       std::is_same_v<T, Take> || std::is_same_v<T, Skip> ||
                       std::is_same_v<T, Group> || std::is_same_v<T, Agg> ||
                       std::is_same_v<T, Sample> ||
                       std::is_same_v<T, TimeRange> ||
                       std::is_same_v<T, Bucket> ||
                       std::is_same_v<T, CallTree> ||
                       std::is_same_v<T, Window> || std::is_same_v<T, Expand> ||
                       std::is_same_v<T, Pivot> || std::is_same_v<T, Unpivot> ||
                       std::is_same_v<T, Lookup> ||
                       std::is_same_v<T, AsofLookup> ||
                       std::is_same_v<T, OverlapLookup> ||
                       std::is_same_v<T, Union> || std::is_same_v<T, Session>;
            },
            s.node);
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

    duql::PipelineItem item(std::string name, const Expr& value) const {
        auto t = term(value);
        std::string text = duql::term_text(*t);
        if (name.empty()) {
            const auto* path = std::get_if<Path>(&value.node);
            if (!path)
                fail(value.span,
                     "name this expression: write 'name = expr' or "
                     "'expr as name'");
            name = path_text(*path, value.span);
        }
        return {std::move(name), duql::TermRef(std::move(t)), std::move(text)};
    }

    std::int64_t count(const std::string& text, Span span) const {
        if (text.starts_with('$')) {
            const auto v = bound(Param{text.substr(1)}, span);
            if (const auto* i = std::get_if<std::int64_t>(&v); i && *i >= 0)
                return *i;
            if (const auto* u = std::get_if<std::uint64_t>(&v);
                u && *u <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()))
                return static_cast<std::int64_t>(*u);
            fail(span, "a row count must be a non-negative integer");
        }
        std::int64_t n = 0;
        const auto r =
            std::from_chars(text.data(), text.data() + text.size(), n);
        if (r.ec != std::errc{} || n < 0)
            fail(span, "a row count must be a non-negative integer");
        return n;
    }

    duql::PipelineStage stage(const Stage& s) const {
        return std::visit(
            [&](const auto& n) -> duql::PipelineStage {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, Where>) {
                    const Flag in_condition(cond_, true);
                    auto t = term(*n.condition);
                    std::string text = duql::term_text(*t);
                    return duql::PipelineWhere{duql::TermRef(std::move(t)),
                                               std::move(text)};
                } else if constexpr (std::is_same_v<T, Select>) {
                    duql::PipelineSelect out;
                    for (const auto& it : n.items)
                        out.items.push_back(item(it.name, *it.value));
                    return out;
                } else if constexpr (std::is_same_v<T, Derive>) {
                    duql::PipelineDerive out;
                    for (const auto& a : n.fields)
                        out.items.push_back(item(a.name, *a.value));
                    return out;
                } else if constexpr (std::is_same_v<T, Drop>) {
                    duql::PipelineDrop out;
                    for (const auto& path : n.paths)
                        out.names.push_back(path_text(path, s.span));
                    return out;
                } else if constexpr (std::is_same_v<T, Rename>) {
                    duql::PipelineRename out;
                    for (const auto& [name, path] : n.pairs)
                        out.pairs.emplace_back(name, path_text(path, s.span));
                    return out;
                } else if constexpr (std::is_same_v<T, Distinct>) {
                    duql::PipelineDistinct out;
                    for (const auto& k : n.keys)
                        out.items.push_back(
                            item(std::holds_alternative<Path>(k->node)
                                     ? std::string{}
                                     : duql::term_text(*term(*k)),
                                 *k));
                    return out;
                } else if constexpr (std::is_same_v<T, syntax::Sort>) {
                    duql::PipelineSort out;
                    for (std::size_t k = 0; k < n.keys.size(); ++k) {
                        const auto& key = n.keys[k];
                        out.keys.push_back(
                            {item(std::holds_alternative<Path>(key.value->node)
                                      ? std::string{}
                                      : duql::term_text(*term(*key.value)),
                                  *key.value),
                             key.descending, key.nulls == NullsOrder::FIRST});
                    }
                    return out;
                } else if constexpr (std::is_same_v<T, Take>) {
                    if (n.by.empty())
                        return duql::PipelineTake{count(n.count, s.span)};
                    duql::PipelineTakeBy out;
                    out.count = count(n.count, s.span);
                    for (const auto& k : n.by) out.keys.push_back(key_item(*k));
                    for (const auto& key : n.order)
                        out.order.push_back({key_item(*key.value),
                                             key.descending,
                                             key.nulls == NullsOrder::FIRST});
                    return out;
                } else if constexpr (std::is_same_v<T, Skip>) {
                    return duql::PipelineSkip{count(n.count, s.span)};
                } else if constexpr (std::is_same_v<T, Sample>) {
                    return sample(n, s.span);
                } else if constexpr (std::is_same_v<T, Group>) {
                    duql::PipelineGroup out;
                    for (const auto& k : n.keys)
                        out.keys.push_back(item(k.name, *k.value));
                    out.aggs = aggregates(n.aggregates);
                    return out;
                } else if constexpr (std::is_same_v<T, Agg>) {
                    return duql::PipelineGroup{{}, aggregates(n.aggregates)};
                } else if constexpr (std::is_same_v<T, TimeRange>) {
                    require_role(s.span, "time_range", true, n.overlap);
                    duql::PipelineTimeRange out{time_value(*n.low),
                                                time_value(*n.high),
                                                n.overlap,
                                                nullptr,
                                                {}};
                    if (!(out.low <= out.high))
                        fail(s.span, "'time_range' needs low <= high");
                    const auto ts = [&] { return role_field(roles_->time); };
                    duql::TermPtr lower =
                        n.overlap
                            ? make_term(duql::TBinary{
                                  duql::TermOp::GT,
                                  make_term(duql::TBinary{
                                      duql::TermOp::ADD, ts(),
                                      role_field(roles_->duration)}),
                                  number_term(out.low)})
                            : make_term(duql::TBinary{duql::TermOp::GE, ts(),
                                                      number_term(out.low)});
                    duql::TermPtr upper = make_term(duql::TBinary{
                        duql::TermOp::LT, ts(), number_term(out.high)});
                    out.condition = make_term(duql::TBinary{
                        duql::TermOp::AND, std::move(lower), std::move(upper)});
                    out.text = duql::term_text(*out.condition);
                    return out;
                } else if constexpr (std::is_same_v<T, Bucket>) {
                    require_role(s.span, "bucket", true, false);
                    const double width = time_value(*n.width);
                    if (!(width > 0))
                        fail(n.width->span, "a bucket width must be positive");
                    duql::TermRef key = make_term(duql::TBinary{
                        duql::TermOp::MUL,
                        make_term(duql::TBinary{duql::TermOp::IDIV,
                                                role_field(roles_->time),
                                                number_term(width)}),
                        number_term(width)});
                    std::string text = duql::term_text(*key);
                    return duql::PipelineBucket{width, n.fill, std::move(key),
                                                std::move(text)};
                } else if constexpr (std::is_same_v<T, CallTree>) {
                    require_role(s.span, "call_tree", true, true);
                    return duql::PipelineCallTree{};
                } else if constexpr (std::is_same_v<T, Session>) {
                    return session(n, s.span);
                } else if constexpr (std::is_same_v<T, Window>) {
                    return window(n);
                } else if constexpr (std::is_same_v<T, Expand>) {
                    if (negative_at(n.path) != n.path.steps.size())
                        fail(s.span,
                             "'expand' takes a path without a negative index");
                    duql::PipelineExpand out;
                    out.path = path_text(n.path, s.span);
                    out.name = n.as;
                    for (const auto& step : n.path.steps)
                        if (n.as.empty() && !step.key.empty())
                            out.name = step.key;
                    out.index = n.with_index;
                    out.keep_empty = n.keep_empty;
                    if (out.name == out.index)
                        fail(s.span,
                             "'expand' names the element and its index '" +
                                 out.name + "'");
                    return out;
                } else if constexpr (std::is_same_v<T, Pivot>) {
                    duql::PipelinePivot out;
                    out.key = key_item(*n.key);
                    out.fixed = !n.values.empty();
                    for (const auto& v : n.values) {
                        const auto* lit = std::get_if<Literal>(&v->node);
                        if (lit && std::holds_alternative<Null>(lit->value)) {
                            out.values.push_back({duql::TNull{}});
                            continue;
                        }
                        const auto value = literal(*v);
                        if (!value)
                            fail(v->span,
                                 "a pivot value is a literal or a parameter");
                        out.values.push_back(constant(value->value));
                    }
                    out.aggs = aggregates(n.aggregates, "pivot");
                    return out;
                } else if constexpr (std::is_same_v<T, Unpivot>) {
                    duql::PipelineUnpivot out;
                    for (const auto& path : n.paths)
                        out.fields.push_back(path_text(path, s.span));
                    out.key = n.key_name;
                    out.value = n.value_name;
                    if (out.key == out.value)
                        fail(s.span, "'unpivot' names its key and value '" +
                                         out.key + "'");
                    return out;
                } else if constexpr (std::is_same_v<T, Lookup>) {
                    duql::PipelineLookup out = lookup(n.rowset, n.keys, s.span);
                    if (!n.into.empty()) out.mode = duql::PipelineNest{n.into};
                    return out;
                } else if constexpr (std::is_same_v<T, OverlapLookup>) {
                    require_role(s.span, "lookup ... overlap", true, true);
                    duql::PipelineLookup out = lookup(n.rowset, n.keys, s.span);
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
                    duql::PipelineLookup out = lookup(n.rowset, n.keys, s.span);
                    duql::PipelineAsof asof;
                    const auto& [t, c] = n.time;
                    asof.column = key_column(*t, c.get(), n.rowset, "asof");
                    asof.time = key_item(*t);
                    asof.direction = n.direction;
                    if (!n.within.empty())
                        asof.tolerance = count(n.within, s.span);
                    out.mode = std::move(asof);
                    return out;
                } else if constexpr (std::is_same_v<T, Union>) {
                    return union_of(*n.other);
                } else {
                    fail(s.span, stage_message(s));
                }
            },
            s.node);
    }

    // The side's column a key or time pairs with: `named` when the key gives
    // one, else the row's own path.
    std::string key_column(const Expr& k, const Expr* k2,
                           const std::string& rowset,
                           const char* clause) const {
        const Expr& named = k2 ? *k2 : k;
        const auto* p = std::get_if<Path>(&named.node);
        if (!p)
            fail(named.span,
                 k2 ? std::string("'") + clause +
                          " k == c' names a column of the row set with c"
                    : "name the row set's " + std::string(clause) +
                          " column: 'lookup " + rowset + " ... " + clause +
                          " expr == column'");
        return path_text(*p, named.span);
    }

    duql::PipelineLookup lookup(
        const std::string& rowset,
        const std::vector<std::pair<ExprPtr, ExprPtr>>& keys, Span span) const {
        duql::PipelineLookup out;
        out.side = let_of(rowset, span).side;
        out.name = rowset;
        for (const auto& [k, k2] : keys)
            out.keys.emplace_back(key_item(*k),
                                  key_column(*k, k2.get(), rowset, "on"));
        return out;
    }

    duql::PipelineItem key_item(const Expr& e) const {
        return item(std::holds_alternative<Path>(e.node)
                        ? std::string{}
                        : duql::term_text(*term(e)),
                    e);
    }

    duql::PipelineSample sample(const Sample& n, Span span) const {
        duql::PipelineSample out;
        out.percent = n.percent;
        const auto r = from_chars_double(
            n.amount.data(), n.amount.data() + n.amount.size(), out.amount);
        if (r.ec != std::errc{}) fail(span, "Invalid sample size");
        if (n.percent) {
            if (!(out.amount >= 0 && out.amount <= 100))
                fail(span, "a sample percentage must be in [0, 100]");
        } else if (std::trunc(out.amount) != out.amount) {
            fail(span, "a sample row count must be an integer");
        }
        if (!n.seed.empty() &&
            std::from_chars(n.seed.data(), n.seed.data() + n.seed.size(),
                            out.seed)
                    .ec != std::errc{})
            fail(span, "Invalid seed");
        return out;
    }

    duql::PipelineSession session(const Session& n, Span span) const {
        require_role(span, "session", true, false);
        duql::PipelineSession out;
        for (const auto& k : n.keys) out.keys.push_back(item(k.name, *k.value));
        out.gap = time_value(*n.gap);
        if (!(out.gap >= 0)) fail(n.gap->span, "a session gap is not negative");
        if (n.max) {
            out.span = time_value(*n.max);
            if (!(out.span > 0))
                fail(n.max->span, "a session 'max' must be positive");
        }
        out.time = role_field(roles_->time);
        if (!roles_->duration.empty()) {
            duql::TermPtr dur = role_field(roles_->duration);
            if (const auto d = roles_->fields.find(roles_->duration);
                d != roles_->fields.end() &&
                d->second != roles_->time_ns_per_unit)
                dur = make_term(duql::TBinary{
                    duql::TermOp::MUL, std::move(dur),
                    number_term(
                        static_cast<double>(d->second) /
                        static_cast<double>(roles_->time_ns_per_unit))});
            out.end = make_term(duql::TBinary{
                duql::TermOp::ADD, role_field(roles_->time), std::move(dur)});
        }
        out.name = n.as.empty() ? std::string("session") : n.as;
        out.text = duql::term_text(*out.time) +
                   (out.end ? " end " + duql::term_text(*out.end) : "");
        return out;
    }

    void require_role(Span span, std::string_view stage, bool time,
                      bool duration) const {
        const auto missing = [&](std::string_view role) {
            const std::string schema =
                roles_ && !roles_->schema.empty()
                    ? "record schema '" + roles_->schema + "'"
                    : std::string("this record schema");
            fail(span, "'" + std::string(stage) + "' needs the " +
                           std::string(role) + " role, which " + schema +
                           " does not bind");
        };
        if (time && (!roles_ || roles_->time.empty())) missing("time");
        if (duration && (!roles_ || roles_->duration.empty()))
            missing("duration");
    }

    // A time in the unit of the time role: a number, a parameter, or a
    // duration.
    double time_value(const Expr& e) const {
        if (const auto* d = std::get_if<Duration>(&e.node)) {
            const auto per = unit_ns(d->unit);
            if (!per) fail(e.span, "Unknown duration unit '" + d->unit + "'");
            double amount = 0;
            if (from_chars_double(d->amount.data(),
                                  d->amount.data() + d->amount.size(), amount)
                    .ec != std::errc{})
                fail(e.span, "Invalid duration");
            return amount * static_cast<double>(*per) /
                   static_cast<double>(roles_->time_ns_per_unit);
        }
        if (auto v = literal(e))
            if (auto x = number(v->value)) return *x;
        fail(e.span, "Expected a time: a number, a parameter or a duration");
    }

    duql::TermPtr number_term(double v) const {
        if (std::trunc(v) == v && v >= 0 && v < 1.8e19)
            return make_term(duql::TConst{static_cast<std::uint64_t>(v)});
        if (std::trunc(v) == v && v < 0 && v > -9.2e18)
            return make_term(duql::TConst{static_cast<std::int64_t>(v)});
        return make_term(duql::TConst{v});
    }

    duql::PipelineItem role_item(const std::string& path) const {
        duql::TermPtr t = role_field(path);
        std::string text = duql::term_text(*t);
        return {{}, duql::TermRef(std::move(t)), std::move(text)};
    }

    duql::TermPtr role_field(const std::string& path) const {
        duql::TField f;
        f.base = path;
        std::size_t at = 0;
        while (at <= path.size()) {
            const std::size_t dot = std::min(path.find('.', at), path.size());
            f.steps.push_back({path.substr(at, dot - at), false, std::nullopt});
            at = dot + 1;
        }
        f.neg_at = f.steps.size();
        return make_term(std::move(f));
    }

    static std::optional<double> number(const duql::LiteralValue& v) {
        if (const auto* i = std::get_if<std::int64_t>(&v))
            return static_cast<double>(*i);
        if (const auto* u = std::get_if<std::uint64_t>(&v))
            return static_cast<double>(*u);
        if (const auto* d = std::get_if<double>(&v)) return *d;
        return std::nullopt;
    }

    std::string text_of(Span span) const {
        return std::string(src_.substr(span.offset, span.length));
    }

    duql::PipelineWindow window(const Window& n) const {
        duql::PipelineWindow out;
        for (const auto& k : n.partition)
            out.keys.push_back(item(k.name, *k.value));
        for (const auto& key : n.order)
            out.order.push_back({key_item(*key.value), key.descending,
                                 key.nulls == NullsOrder::FIRST});
        for (const auto& a : n.fields) {
            const std::size_t before = out.calls.size();
            win_ = &out.calls;
            auto t = term(*a.value);
            win_ = nullptr;
            if (out.calls.size() == before)
                fail(a.value->span,
                     "each entry of a 'window' block uses a window "
                     "function, such as 'r = row_number()' or "
                     "'g = ts - lag(ts)'");
            out.items.push_back(
                {a.name, duql::TermRef(std::move(t)), text_of(a.value->span)});
        }
        return out;
    }

    // A call of a window function inside a window entry, as a field of the
    // hidden column it fills; nullopt for any other function.
    std::optional<duql::TermPtr> window_call(const Call& c, Span span) const {
        const WinInfo* w = window_info(c.name);
        if (!win_) {
            if (w && w->window_only)
                fail(span, "'" + c.name +
                               "' is a window function; it stands in a "
                               "'window' block");
            return std::nullopt;
        }
        if (win_arg_ && (w || aggregate_info(c.name)))
            fail(span,
                 "a window function's argument holds no window "
                 "function or aggregate");
        if (win_arg_) return std::nullopt;
        if (!w) {
            if (aggregate_info(c.name))
                fail(span, "'" + c.name + "' does not run in a 'window' block");
            return std::nullopt;
        }
        if ((w->fn == duql::WinFn::MIN || w->fn == duql::WinFn::MAX) &&
            c.args.size() > 1)
            return std::nullopt;
        const std::size_t n = c.args.size();
        if (n < w->min_args || n > w->max_args)
            fail(span, "Wrong number of arguments to '" + c.name + "'");
        for (const auto& arg : c.args)
            if (!arg.name.empty())
                fail(arg.value->span, "'" + c.name +
                                          "' takes no named argument '" +
                                          arg.name + "'");
        duql::PipelineWinCall call;
        call.column = "__duql_w_" + std::to_string(win_->size());
        call.fn = w->fn;
        call.text = text_of(span);
        if (n > 0) {
            win_arg_ = true;
            call.arg = term(*c.args[0].value);
            win_arg_ = false;
        }
        if (n > 1 && w->fn == duql::WinFn::QUANTILE) {
            const Expr& level = *c.args[1].value;
            const auto v = literal(level);
            const auto q = v ? number(v->value) : std::nullopt;
            if (!q || !(*q >= 0 && *q <= 1))
                fail(level.span,
                     "a quantile level is a number in [0, 1], written as a "
                     "literal or a parameter");
            call.q = *q;
        } else if (n > 1) {
            const Expr& at = *c.args[1].value;
            const auto v = literal(at);
            const std::int64_t* i =
                v ? std::get_if<std::int64_t>(&v->value) : nullptr;
            const std::uint64_t* u =
                v ? std::get_if<std::uint64_t>(&v->value) : nullptr;
            if (i && *i >= 0)
                call.offset = *i;
            else if (u && *u <= static_cast<std::uint64_t>(
                                    std::numeric_limits<std::int64_t>::max()))
                call.offset = static_cast<std::int64_t>(*u);
            else
                fail(at.span,
                     "a lag or lead distance is a non-negative integer, "
                     "written as a literal or a parameter");
        }
        duql::TField f;
        f.base = call.column;
        f.steps.push_back({call.column, false, std::nullopt});
        f.neg_at = 1;
        win_->push_back(std::move(call));
        return make_term(std::move(f));
    }

    std::vector<duql::PipelineAgg> aggregates(
        const std::vector<Assign>& block,
        std::string_view stage = "group") const {
        std::vector<duql::PipelineAgg> out;
        for (const auto& a : block) {
            const Expr& e = *a.value;
            const auto* c = std::get_if<Call>(&e.node);
            const AggInfo* info = c ? aggregate_info(c->name) : nullptr;
            if (!info)
                fail(e.span, "each entry of a '" + std::string(stage) +
                                 "' block is one aggregate call, such as "
                                 "'n = count()' or 't = sum(dur)'");
            const std::size_t n = c->args.size();
            if (n < info->min_args || n > info->max_args)
                fail(e.span, "Wrong number of arguments to '" + c->name + "'");
            for (const auto& arg : c->args)
                if (!arg.name.empty())
                    fail(arg.value->span, "'" + c->name +
                                              "' takes no named argument '" +
                                              arg.name + "'");
            if (duql::is_occupancy(info->fn))
                require_role(e.span, c->name, true, true);
            duql::PipelineAgg agg;
            agg.name = a.name;
            agg.fn = info->fn;
            agg.text = c->name + "(";
            if (n > 0) {
                const Expr* first = c->args[0].value.get();
                const auto* inner = std::get_if<Call>(&first->node);
                if (info->fn == duql::AggFn::QUANTILE && inner &&
                    inner->name == "merge") {
                    if (inner->args.size() != 1 || !inner->args[0].name.empty())
                        fail(first->span,
                             "Wrong number of arguments to 'merge'");
                    agg.merged = true;
                    first = inner->args[0].value.get();
                }
                agg.arg = term(*first);
                agg.text += agg.merged
                                ? "merge(" + duql::term_text(*agg.arg) + ")"
                                : duql::term_text(*agg.arg);
            }
            if (info->fn == duql::AggFn::ARGMAX ||
                info->fn == duql::AggFn::ARGMIN) {
                agg.by = term(*c->args[1].value);
                agg.text += ", " + duql::term_text(*agg.by);
            }
            if (info->fn == duql::AggFn::QUANTILE) {
                const Expr& level = *c->args[1].value;
                const auto v = literal(level);
                const auto q = v ? number(v->value) : std::nullopt;
                if (!q || !(*q >= 0 && *q <= 1))
                    fail(level.span,
                         "a quantile level is a number in [0, 1], written "
                         "as a literal or a parameter");
                agg.q = *q;
                agg.text +=
                    ", " + duql::term_text(*make_term(duql::TConst{*q}));
            }
            agg.text += ")";
            out.push_back(std::move(agg));
        }
        return out;
    }

    static std::string stage_name(const Stage& s) {
        static constexpr const char* NAMES[] = {
            "where",    "derive",     "select",    "drop",   "rename",
            "distinct", "group",      "agg",       "window", "pivot",
            "unpivot",  "sort",       "take",      "skip",   "sample",
            "expand",   "lookup",     "lookup",    "lookup", "union",
            "call",     "time_range", "call_tree", "bucket", "session"};
        static_assert(std::size(NAMES) == std::variant_size_v<StageNode>);
        return NAMES[s.node.index()];
    }

    static std::optional<std::int64_t> unit_ns(std::string_view unit) {
        if (unit == "ns") return 1;
        if (unit == "us") return 1000;
        if (unit == "ms") return 1'000'000;
        if (unit == "s") return 1'000'000'000;
        if (unit == "m") return 60'000'000'000;
        if (unit == "h") return 3'600'000'000'000;
        if (unit == "d") return 86'400'000'000'000;
        return std::nullopt;
    }

    // Nanoseconds per unit of an expression over a field with a time or
    // duration role; arithmetic keeps its operand's unit.
    std::optional<std::int64_t> unit_of(const Expr& e) const {
        if (!roles_) return std::nullopt;
        if (const auto* p = std::get_if<Path>(&e.node)) {
            if (p->root != PathRoot::RECORD) return std::nullopt;
            const std::string text = path_text(*p, e.span);
            std::string_view key = text;
            auto it = roles_->fields.find(key);
            if (it == roles_->fields.end() && key.starts_with("args."))
                it = roles_->fields.find(key.substr(5));
            if (it == roles_->fields.end()) return std::nullopt;
            return it->second;
        }
        if (const auto* b = std::get_if<Binary>(&e.node)) {
            if (auto u = unit_of(*b->left)) return u;
            return unit_of(*b->right);
        }
        if (const auto* u = std::get_if<Unary>(&e.node))
            return unit_of(*u->operand);
        if (const auto* c = std::get_if<Call>(&e.node)) {
            if (c->name == "as_time") return roles_->time_ns_per_unit;
            if (c->name == "bin" && !c->args.empty())
                return unit_of(*c->args[0].value);
        }
        return std::nullopt;
    }

    // The duration `dur` in the unit of `other`: an integer when exact.
    duql::TConst duration(const Expr& dur, const Expr& other) const {
        const auto& d = std::get<Duration>(dur.node);
        const auto per = unit_ns(d.unit);
        if (!per) fail(dur.span, "Unknown duration unit '" + d.unit + "'");
        double amount = 0;
        const auto r = from_chars_double(
            d.amount.data(), d.amount.data() + d.amount.size(), amount);
        if (r.ec != std::errc{}) fail(dur.span, "Invalid duration");
        const auto unit = unit_of(other);
        if (!unit)
            fail(other.span,
                 "'" +
                     std::string(
                         src_.substr(other.span.offset, other.span.length)) +
                     "' has no time or duration role to give the duration "
                     "a unit; write as_time(x, \"ns\") to give it one");
        const double v =
            amount * static_cast<double>(*per) / static_cast<double>(*unit);
        if (std::trunc(v) == v && v < 1.8e19)
            return duql::TConst{static_cast<std::uint64_t>(v)};
        return duql::TConst{v};
    }

    duql::TermPtr operand(const Expr& e, const Expr& other) const {
        if (std::holds_alternative<Duration>(e.node))
            return make_term(duration(e, other));
        return term(e);
    }

    // bin(t, d), to_seconds(t) and as_time(x, unit) as arithmetic in the
    // units of the schema's roles.
    std::optional<duql::TermPtr> time_call(const Call& c, Span span) const {
        if (c.name != "bin" && c.name != "to_seconds" && c.name != "as_time")
            return std::nullopt;
        if (!roles_)
            fail(span, "'" + c.name +
                           "' needs the record schema's time roles; run the "
                           "query on a View");
        if (c.args.size() != (c.name == "to_seconds" ? 1u : 2u))
            fail(span, "Wrong number of arguments to '" + c.name + "'");
        const Expr& x = *c.args[0].value;
        if (c.name == "bin") {
            const Expr& w = *c.args[1].value;
            return make_term(
                duql::TBinary{duql::TermOp::MUL,
                              make_term(duql::TBinary{duql::TermOp::IDIV,
                                                      term(x), operand(w, x)}),
                              operand(w, x)});
        }
        if (c.name == "to_seconds") {
            const auto unit = unit_of(x);
            if (!unit)
                fail(x.span,
                     "to_seconds() takes a field with a time or duration "
                     "role");
            return make_term(duql::TBinary{
                duql::TermOp::DIV, term(x),
                make_term(duql::TConst{1e9 / static_cast<double>(*unit)})});
        }
        const auto per = unit_ns(pattern(*c.args[1].value));
        if (!per)
            fail(c.args[1].value->span,
                 "as_time() takes the unit \"ns\", \"us\", \"ms\" or "
                 "\"s\"");
        const std::int64_t time = roles_->time_ns_per_unit;
        if (*per % time == 0)
            return make_term(duql::TBinary{
                duql::TermOp::MUL, term(x),
                make_term(duql::TConst{std::uint64_t(*per / time)})});
        return make_term(
            duql::TBinary{duql::TermOp::DIV, term(x),
                          make_term(duql::TConst{static_cast<double>(time) /
                                                 static_cast<double>(*per)})});
    }

    [[noreturn]] void fail(Span span, std::string msg) const {
        throw Failure{
            make_error(src_, span.offset, span.length, std::move(msg))};
    }

    static std::string stage_message(const Stage& s) {
        return "'" + stage_name(s) +
               "' as a pipeline stage arrives in duql stage 12h";
    }

    QueryNodePtr condition(const Expr& e) const {
        if (const auto* u = std::get_if<Unary>(&e.node)) {
            if (u->op == UnaryOp::NOT)
                return duql::make_node(duql::NotNode{condition(*u->operand)});
        } else if (const auto* b = std::get_if<Binary>(&e.node)) {
            switch (b->op) {
                case BinaryOp::OR:
                    return duql::make_node(duql::OrNode{condition(*b->left),
                                                        condition(*b->right)});
                case BinaryOp::AND:
                    return duql::make_node(duql::AndNode{condition(*b->left),
                                                         condition(*b->right)});
                case BinaryOp::EQ:
                case BinaryOp::NE:
                case BinaryOp::LT:
                case BinaryOp::LE:
                case BinaryOp::GT:
                case BinaryOp::GE:
                    if (auto n = compare(*b)) return n;
                    break;
                case BinaryOp::REGEX:
                case BinaryOp::IREGEX:
                case BinaryOp::NREGEX:
                case BinaryOp::NIREGEX:
                    if (auto f = as_field(*b->left)) {
                        const auto [op, negated] = regex_op(b->op);
                        return match(std::move(*f), op, pattern(*b->right),
                                     negated, *b->right);
                    }
                    break;
                default:
                    break;
            }
        } else if (const auto* in = std::get_if<In>(&e.node)) {
            if (!in->subquery)
                if (auto n = membership(*in)) return n;
        } else if (const auto* l = std::get_if<Like>(&e.node)) {
            if (auto f = as_field(*l->subject))
                return match(
                    std::move(*f), l->icase ? MatchOp::ILIKE : MatchOp::LIKE,
                    l->pattern, l->negated, e, escape_char(l->escape, e.span));
        } else if (const auto* c = std::get_if<Contains>(&e.node)) {
            return match(duql::FieldNode{path_text(c->path, e.span), c->any},
                         MatchOp::ICONTAINS, c->text, c->negated, e);
        }
        const Flag in_condition(cond_, true);
        auto t = term(e);
        std::string text = duql::term_text(*t);
        return duql::make_node(duql::ExprLeaf{
            std::shared_ptr<const duql::Term>(std::move(t)), std::move(text)});
    }

    static std::pair<MatchOp, bool> regex_op(BinaryOp op) {
        const bool icase = op == BinaryOp::IREGEX || op == BinaryOp::NIREGEX;
        const bool negated = op == BinaryOp::NREGEX || op == BinaryOp::NIREGEX;
        return {icase ? MatchOp::IREGEX : MatchOp::REGEX, negated};
    }

    void reject_null(const Expr& e) const {
        if (const auto* lit = std::get_if<Literal>(&e.node))
            if (std::holds_alternative<Null>(lit->value))
                fail(e.span,
                     "a comparison with null is never true; write "
                     "'x is null' or 'x is not null'");
    }

    // `field op value` as a literal leaf, or null when the condition has
    // another shape.
    QueryNodePtr compare(const Binary& b) const {
        reject_null(*b.left);
        reject_null(*b.right);
        auto f = as_field(*b.left);
        if (!f) return nullptr;
        if (std::holds_alternative<Duration>(b.right->node)) {
            const auto c = duration(*b.right, *b.left);
            LiteralNode lit;
            if (const auto* u = std::get_if<std::uint64_t>(&c.value))
                lit.value = *u;
            else
                lit.value = std::get<double>(c.value);
            return duql::make_node(
                duql::CompareNode{std::move(*f), compare_op(b.op), lit});
        }
        auto value = literal(*b.right);
        if (!value) return nullptr;
        return duql::make_node(duql::CompareNode{
            std::move(*f), compare_op(b.op), std::move(*value)});
    }

    static duql::CompareOp compare_op(BinaryOp op) {
        switch (op) {
            case BinaryOp::NE:
                return duql::CompareOp::NE;
            case BinaryOp::LT:
                return duql::CompareOp::LT;
            case BinaryOp::LE:
                return duql::CompareOp::LE;
            case BinaryOp::GT:
                return duql::CompareOp::GT;
            case BinaryOp::GE:
                return duql::CompareOp::GE;
            default:
                return duql::CompareOp::EQ;
        }
    }

    void reject_null_items(const In& in) const {
        for (const auto& item : in.list)
            if (const auto* lit = std::get_if<Literal>(&item->node))
                if (std::holds_alternative<Null>(lit->value))
                    fail(item->span,
                         "null in an 'in' list is never matched and makes "
                         "'not in' never true; test 'x is null' apart");
    }

    // A literal `in` list on a field as a literal leaf, or null.
    QueryNodePtr membership(const In& in) const {
        reject_null_items(in);
        auto f = as_field(*in.subject);
        if (!f) return nullptr;
        duql::ArrayNode arr;
        for (const auto& item : in.list) {
            auto value = literal(*item);
            if (!value) return nullptr;
            arr.elements.push_back(std::move(*value));
        }
        arr.set = duql::make_in_set(arr);
        if (in.negated)
            return duql::make_node(
                duql::NotInNode{std::move(*f), std::move(arr)});
        return duql::make_node(duql::InNode{std::move(*f), std::move(arr)});
    }

    std::string pattern(const Expr& e) const {
        if (const auto* lit = std::get_if<Literal>(&e.node))
            if (const auto* s = std::get_if<std::string>(&lit->value))
                return *s;
        if (const auto* p = std::get_if<Param>(&e.node)) {
            const auto value = bound(*p, e.span);
            if (const auto* s = std::get_if<std::string>(&value)) return *s;
        }
        fail(e.span, "Expected a string pattern");
    }

    duql::PatternPtr compile(MatchOp op, const std::string& pattern,
                             std::optional<char> escape, const Expr& at) const {
        duql::PatternResult r = [&] {
            switch (op) {
                case MatchOp::LIKE:
                    return duql::compile_like(pattern, false, escape);
                case MatchOp::ILIKE:
                    return duql::compile_like(pattern, true, escape);
                case MatchOp::REGEX:
                    return duql::compile_regex(pattern, false);
                case MatchOp::IREGEX:
                    return duql::compile_regex(pattern, true);
                case MatchOp::ICONTAINS:
                    break;
            }
            return duql::compile_contains(pattern, true);
        }();
        if (!r)
            fail(at.span, "Invalid pattern at offset " +
                              std::to_string(r.error().offset) + ": " +
                              r.error().message);
        return std::move(*r);
    }

    std::optional<char> escape_char(const std::optional<std::string>& esc,
                                    Span span) const {
        if (!esc) return std::nullopt;
        if (esc->size() != 1) fail(span, "'escape' takes one character");
        return (*esc)[0];
    }

    QueryNodePtr match(duql::FieldNode f, MatchOp op, std::string pattern,
                       bool negated, const Expr& at,
                       std::optional<char> escape = std::nullopt) const {
        duql::MatchNode node;
        node.compiled = compile(op, pattern, escape, at);
        node.field = std::move(f);
        node.op = op;
        node.pattern = std::move(pattern);
        node.negated = negated;
        node.escape = escape;
        return duql::make_node(std::move(node));
    }

    // The field a literal leaf tests: a record path without a negative index,
    // or any(path).
    std::optional<duql::FieldNode> as_field(const Expr& e) const {
        const Path* p = std::get_if<Path>(&e.node);
        bool any = false;
        if (!p)
            if (const auto* c = std::get_if<Call>(&e.node))
                if (c->name == "any" && c->args.size() == 1 &&
                    c->args[0].name.empty()) {
                    p = std::get_if<Path>(&c->args[0].value->node);
                    any = p != nullptr;
                }
        if (!p || negative_at(*p) != p->steps.size()) return std::nullopt;
        return duql::FieldNode{path_text(*p, e.span), any};
    }

    static std::size_t negative_at(const Path& p) {
        for (std::size_t i = 0; i < p.steps.size(); ++i)
            if (p.steps[i].index && *p.steps[i].index < 0) return i;
        return p.steps.size();
    }

    std::string path_text(const Path& p, Span span) const {
        return steps_text(p, span, p.steps.size());
    }

    [[noreturn]] void reject_root(const Path& p, Span span) const {
        if (p.root == PathRoot::ENCLOSING && sub_ > 0)
            fail(span,
                 "'^.' reads the enclosing row, which a sub-query cannot "
                 "see; to read a row of a let by key write 'k -> set.path'");
        fail(span, "'.' and '^.' paths stand inside any(p, e) or all(p, e)");
    }

    std::string steps_text(const Path& p, Span span, std::size_t end) const {
        if (p.root != PathRoot::RECORD) reject_root(p, span);
        std::string out;
        for (std::size_t i = 0; i < end; ++i) {
            const auto& step = p.steps[i];
            if (!step.key.empty() || !step.index) {
                if (!out.empty()) out += '.';
                out += step.key;
            }
            if (step.index) {
                out += '[';
                out += std::to_string(*step.index);
                out += ']';
            }
        }
        return out;
    }

    static duql::TermPtr make_term(decltype(duql::Term::node) node) {
        return std::make_unique<const duql::Term>(duql::Term{std::move(node)});
    }

    static duql::TConst constant(const duql::LiteralValue& v) {
        return std::visit([](const auto& x) { return duql::TConst{x}; }, v);
    }

    static duql::TermOp term_op(BinaryOp op) {
        switch (op) {
            case BinaryOp::OR:
                return duql::TermOp::OR;
            case BinaryOp::AND:
                return duql::TermOp::AND;
            case BinaryOp::EQ:
                return duql::TermOp::EQ;
            case BinaryOp::NE:
                return duql::TermOp::NE;
            case BinaryOp::LT:
                return duql::TermOp::LT;
            case BinaryOp::LE:
                return duql::TermOp::LE;
            case BinaryOp::GT:
                return duql::TermOp::GT;
            case BinaryOp::GE:
                return duql::TermOp::GE;
            case BinaryOp::ADD:
                return duql::TermOp::ADD;
            case BinaryOp::SUB:
                return duql::TermOp::SUB;
            case BinaryOp::MUL:
                return duql::TermOp::MUL;
            case BinaryOp::DIV:
                return duql::TermOp::DIV;
            case BinaryOp::IDIV:
                return duql::TermOp::IDIV;
            case BinaryOp::MOD:
                return duql::TermOp::MOD;
            case BinaryOp::COALESCE:
                return duql::TermOp::COALESCE;
            case BinaryOp::REGEX:
            case BinaryOp::IREGEX:
            case BinaryOp::NREGEX:
            case BinaryOp::NIREGEX:
                break;
        }
        return duql::TermOp::EQ;
    }

    duql::TermPtr term(const Expr& e) const {
        return std::visit(
            [&](const auto& n) -> duql::TermPtr {
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
                    fail(e.span,
                         "a duration takes its unit from the field it meets "
                         "in a comparison, 'between', arithmetic or bin()");
                } else if constexpr (std::is_same_v<T, Path>) {
                    return field_term(n, e.span);
                } else if constexpr (std::is_same_v<T, Param>) {
                    return make_term(constant(bound(n, e.span)));
                } else if constexpr (std::is_same_v<T, Unary>) {
                    if (n.op == UnaryOp::NOT)
                        return make_term(
                            duql::TUnary{duql::TermOp::NOT, term(*n.operand)});
                    if (auto v = literal(e))
                        return make_term(constant(v->value));
                    return make_term(
                        duql::TUnary{duql::TermOp::NEG, term(*n.operand)});
                } else if constexpr (std::is_same_v<T, Binary>) {
                    return binary_term(n);
                } else if constexpr (std::is_same_v<T, In>) {
                    if (n.subquery) return make_term(semi_join(n, e.span));
                    reject_null_items(n);
                    if (auto t = any_match(*n.subject, [&] {
                            duql::TIn in{element(), {}, n.negated};
                            for (const auto& item : n.list)
                                in.list.push_back(term(*item));
                            return make_term(std::move(in));
                        }))
                        return std::move(*t);
                    duql::TIn t{term(*n.subject), {}, n.negated};
                    for (const auto& item : n.list)
                        t.list.push_back(term(*item));
                    return make_term(std::move(t));
                } else if constexpr (std::is_same_v<T, Contains>) {
                    if (!n.any)
                        return make_term(duql::TMatch{
                            field_term(n.path, e.span), MatchOp::ICONTAINS,
                            n.text, n.negated, std::nullopt,
                            compile(MatchOp::ICONTAINS, n.text, std::nullopt,
                                    e)});
                    if (quant_ > 0) fail(e.span, NESTED_QUANTIFIER);
                    return make_term(
                        duql::TQuant{false, field_term(n.path, e.span),
                                     or_false(make_term(duql::TMatch{
                                         element(), MatchOp::ICONTAINS, n.text,
                                         n.negated, std::nullopt,
                                         compile(MatchOp::ICONTAINS, n.text,
                                                 std::nullopt, e)}))});
                } else if constexpr (std::is_same_v<T, Between>) {
                    if (auto t = any_match(*n.subject, [&] {
                            return make_term(duql::TBetween{
                                element(), operand(*n.low, *n.subject),
                                operand(*n.high, *n.subject), n.negated});
                        }))
                        return std::move(*t);
                    return make_term(duql::TBetween{
                        term(*n.subject), operand(*n.low, *n.subject),
                        operand(*n.high, *n.subject), n.negated});
                } else if constexpr (std::is_same_v<T, Like>) {
                    const MatchOp op = n.icase ? MatchOp::ILIKE : MatchOp::LIKE;
                    const auto esc = escape_char(n.escape, e.span);
                    if (auto t = any_match(*n.subject, [&] {
                            return make_term(duql::TMatch{
                                element(), op, n.pattern, n.negated, esc,
                                compile(op, n.pattern, esc, e)});
                        }))
                        return std::move(*t);
                    return make_term(
                        duql::TMatch{term(*n.subject), op, n.pattern, n.negated,
                                     esc, compile(op, n.pattern, esc, e)});
                } else if constexpr (std::is_same_v<T, Is>) {
                    return make_term(
                        duql::TIs{term(*n.subject), n.missing, n.negated});
                } else if constexpr (std::is_same_v<T, Arrow>) {
                    return make_term(arrow(n, e.span, false));
                } else if constexpr (std::is_same_v<T, Call>) {
                    return call_term(n, e.span);
                } else if constexpr (std::is_same_v<T, List>) {
                    fail(e.span, "a list stands only after 'in'");
                } else if constexpr (std::is_same_v<T, Tuple>) {
                    fail(e.span,
                         "a tuple stands only before 'in (from ...)' or "
                         "'->'");
                } else {
                    return make_term(scalar(n, e.span));
                }
            },
            e.node);
    }

    static constexpr const char* NESTED_QUANTIFIER =
        "a quantifier inside another quantifier is not supported";

    static duql::TermPtr element() {
        duql::TField f;
        f.root = duql::FieldRoot::ELEMENT;
        return std::make_unique<const duql::Term>(duql::Term{std::move(f)});
    }

    // `t ?? false`: the legacy any() leaf counts an unknown element as false.
    duql::TermPtr or_false(duql::TermPtr t) const {
        return make_term(duql::TBinary{duql::TermOp::COALESCE, std::move(t),
                                       make_term(duql::TConst{false})});
    }

    duql::TermPtr field_term(const Path& p, Span span) const {
        if (p.root != PathRoot::RECORD) {
            if (quant_ == 0) reject_root(p, span);
            if (p.root == PathRoot::CURRENT) {
                duql::TField f;
                f.root = duql::FieldRoot::ELEMENT;
                f.steps = p.steps;
                return make_term(std::move(f));
            }
            auto t = field_term(Path{PathRoot::RECORD, p.steps}, span);
            auto f = std::get<duql::TField>(t->node);
            f.root = duql::FieldRoot::OUTER;
            return make_term(std::move(f));
        }
        const std::size_t neg = negative_at(p);
        duql::TField f;
        f.base = steps_text(p, span, neg);
        if (neg < p.steps.size() && !p.steps[neg].key.empty()) {
            if (!f.base.empty()) f.base += '.';
            f.base += p.steps[neg].key;
        }
        f.steps = p.steps;
        f.neg_at = neg;
        return make_term(std::move(f));
    }

    duql::TermPtr binary_term(const Binary& b) const {
        switch (b.op) {
            case BinaryOp::REGEX:
            case BinaryOp::IREGEX:
            case BinaryOp::NREGEX:
            case BinaryOp::NIREGEX: {
                const auto [op, negated] = regex_op(b.op);
                std::string p = pattern(*b.right);
                auto compiled = compile(op, p, std::nullopt, *b.right);
                if (auto t = any_match(*b.left, [&] {
                        return make_term(duql::TMatch{element(), op, p, negated,
                                                      std::nullopt, compiled});
                    }))
                    return std::move(*t);
                return make_term(duql::TMatch{term(*b.left), op, std::move(p),
                                              negated, std::nullopt,
                                              std::move(compiled)});
            }
            case BinaryOp::EQ:
            case BinaryOp::NE:
            case BinaryOp::LT:
            case BinaryOp::LE:
            case BinaryOp::GT:
            case BinaryOp::GE:
                reject_null(*b.left);
                reject_null(*b.right);
                if (auto q = quantified(b)) return std::move(*q);
                if (std::holds_alternative<Arrow>(b.left->node)) {
                    if (auto t = any_match(*b.left, [&] {
                            return make_term(
                                duql::TBinary{term_op(b.op), element(),
                                              operand(*b.right, *b.left)});
                        }))
                        return std::move(*t);
                } else if (auto t = any_match(*b.right, [&] {
                               return make_term(duql::TBinary{
                                   term_op(b.op), operand(*b.left, *b.right),
                                   element()});
                           })) {
                    return std::move(*t);
                }
                break;
            default:
                break;
        }
        return make_term(duql::TBinary{term_op(b.op),
                                       operand(*b.left, *b.right),
                                       operand(*b.right, *b.left)});
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
    std::optional<duql::TermPtr> quantified(const Binary& b) const {
        const Call* l = quantified_side(*b.left);
        const Call* r = quantified_side(*b.right);
        if (!l && !r) return std::nullopt;
        const Call* c = l ? l : r;
        if (l && r)
            fail(c->args[0].value->span,
                 "compare a quantified array with a value");
        if (quant_ > 0) fail(c->args[0].value->span, NESTED_QUANTIFIER);
        const Expr& other = l ? *b.right : *b.left;
        duql::TermPtr value = term(other);
        duql::TermPtr cond =
            l ? make_term(
                    duql::TBinary{term_op(b.op), element(), std::move(value)})
              : make_term(
                    duql::TBinary{term_op(b.op), std::move(value), element()});
        const bool all = c->name == "all";
        if (!all) cond = or_false(std::move(cond));
        return make_term(
            duql::TQuant{all, term(*c->args[0].value), std::move(cond)});
    }

    duql::TermPtr quantifier(const Call& c, Span span) const {
        if (c.args.size() == 1)
            fail(span, "'" + c.name + "(p)' compares with a value, as in '" +
                           c.name + "(p) == v'; with a condition write '" +
                           c.name + "(p, e)'");
        if (c.args.size() != 2)
            fail(span, "Wrong number of arguments to '" + c.name + "'");
        for (const auto& a : c.args)
            if (!a.name.empty())
                fail(a.value->span, "'" + c.name +
                                        "' takes no named argument '" + a.name +
                                        "'");
        if (quant_ > 0) fail(span, NESTED_QUANTIFIER);
        auto subject = term(*c.args[0].value);
        ++quant_;
        auto cond = term(*c.args[1].value);
        --quant_;
        return make_term(
            duql::TQuant{c.name == "all", std::move(subject), std::move(cond)});
    }

    duql::TermPtr call_term(const Call& c, Span span) const {
        if (auto t = time_call(c, span)) return std::move(*t);
        if (c.name == "any" || c.name == "all") return quantifier(c, span);
        if (auto t = window_call(c, span)) return std::move(*t);
        const duql::FnInfo* info = nullptr;
        for (const auto& f : duql::FUNCTIONS)
            if (f.name == c.name) info = &f;
        if (!info && aggregate_info(c.name))
            fail(span, "'" + c.name +
                           "' is an aggregate; it stands in a 'group' or "
                           "'agg' block");
        if (!info) fail(span, "Unknown function '" + c.name + "'");
        const std::size_t n = c.args.size();
        if (n < info->min_args ||
            (info->max_args != duql::VARIADIC && n > info->max_args) ||
            (info->fn == duql::Fn::CASE && n % 2 == 0))
            fail(span, "Wrong number of arguments to '" + c.name + "'");
        duql::TCall t{info->fn, {}, nullptr};
        for (const auto& a : c.args) {
            if (!a.name.empty())
                fail(a.value->span, "'" + c.name +
                                        "' takes no named argument '" + a.name +
                                        "'");
            t.args.push_back(term(*a.value));
        }
        if (info->fn == duql::Fn::EXISTS &&
            !std::holds_alternative<duql::TField>(t.args[0]->node))
            fail(c.args[0].value->span, "exists() takes a path");
        if (info->fn == duql::Fn::EXTRACT) {
            const Expr& at = *c.args[1].value;
            t.pattern = compile(MatchOp::REGEX, pattern(at), std::nullopt, at);
            const std::size_t groups = duql::capture_count(*t.pattern);
            if (t.args.size() > 2)
                if (const auto* g = std::get_if<duql::TConst>(&t.args[2]->node))
                    if (const auto* want =
                            std::get_if<std::uint64_t>(&g->value);
                        want && *want > groups)
                        fail(c.args[2].value->span,
                             "extract() group " + std::to_string(*want) +
                                 " is past the pattern's " +
                                 std::to_string(groups) + " groups");
        }
        return make_term(std::move(t));
    }

    duql::LiteralValue bound(const Param& p, Span span) const {
        const auto it = params_.find(p.name);
        if (it == params_.end())
            fail(span, "No value bound to parameter $" + p.name);
        return it->second;
    }

    // A literal, a parameter, or a negated numeric literal, as the value of a
    // query node.
    std::optional<LiteralNode> literal(const Expr& e) const {
        if (const auto* p = std::get_if<Param>(&e.node))
            return LiteralNode{bound(*p, e.span)};
        if (const auto* u = std::get_if<Unary>(&e.node)) {
            if (u->op != UnaryOp::NEG) return std::nullopt;
            const auto* lit = std::get_if<Literal>(&u->operand->node);
            if (!lit) return std::nullopt;
            if (const auto* v = std::get_if<std::uint64_t>(&lit->value)) {
                if (*v == 0) return LiteralNode{std::uint64_t{0}};
                if (*v > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max()))
                    fail(e.span,
                         "Invalid integer: '-" + lit->number_text + "'");
                return LiteralNode{-static_cast<std::int64_t>(*v)};
            }
            if (const auto* d = std::get_if<double>(&lit->value))
                return LiteralNode{-*d};
            return std::nullopt;
        }
        const auto* lit = std::get_if<Literal>(&e.node);
        if (!lit) return std::nullopt;
        return std::visit(
            [&](const auto& v) -> std::optional<LiteralNode> {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, Null>)
                    return std::nullopt;
                else
                    return LiteralNode{v};
            },
            lit->value);
    }
};

}  // namespace

namespace {

// The macros in scope for a query with `defs`, after the source's `source`.
dftracer::utils::expected<MacroScopes, duql::DuqlError> scopes_of(
    std::vector<Def>& defs, std::string_view text,
    std::vector<duql::Macro> source) {
    auto own = duql::macros_of(defs, text, "the query");
    if (!own) return dftracer::utils::unexpected(own.error());
    MacroScopes scopes{std::move(*own), std::move(source), duql::path_macros()};
    for (const auto& scope : scopes)
        for (const auto& m : scope)
            if (builtin_name(m.name))
                return dftracer::utils::unexpected(make_error(
                    text, 0, 0,
                    "macro '" + m.name + "' in " + m.origin +
                        " has the name of a built-in function; rename it"));
    return scopes;
}

dftracer::utils::expected<void, duql::DuqlError> expand(
    syntax::Pipeline& p, const MacroScopes& scopes, std::string_view text) {
    return duql::expand_macros(p, scopes, text, builtin_name);
}

}  // namespace

dftracer::utils::expected<duql::Program, duql::DuqlError> compile_program(
    std::string_view text, const duql::Params& params, const duql::Roles* roles,
    std::string_view source) {
    auto tree = syntax::parse(text);
    if (!tree) return dftracer::utils::unexpected(tree.error());
    std::vector<Def> defs = duql::take_defs(tree->decls);
    std::vector<Let> rowsets;
    std::vector<duql::Macro> source_macros;
    bool fallback = false;
    const std::string source_text =
        source.empty() ? std::string()
                       : "source s {\n" + std::string(source) + "\n}";
    std::optional<syntax::Program> source_tree;
    if (!source.empty()) {
        auto st = syntax::parse(source_text);
        if (!st) return dftracer::utils::unexpected(st.error());
        source_tree = std::move(*st);
        auto& decl = std::get<SourceDecl>(source_tree->decls.front());
        auto m = duql::macros_of(decl.defs, source_text, "the source");
        if (!m) return dftracer::utils::unexpected(m.error());
        source_macros = std::move(*m);
        fallback = duql::args_fallback(decl.defs);
        for (auto& r : decl.rowsets)
            rowsets.push_back({r.name, std::move(r.pipeline)});
    }
    auto scopes = scopes_of(defs, text, source_macros);
    if (!scopes) return dftracer::utils::unexpected(scopes.error());
    const MacroScopes source_scopes{source_macros, duql::path_macros()};
    for (auto& r : rowsets)
        if (auto ok = expand(*r.pipeline, source_scopes, source_text); !ok)
            return dftracer::utils::unexpected(ok.error());
    for (auto& d : tree->decls)
        if (auto* let = std::get_if<Let>(&d))
            if (auto ok = expand(*let->pipeline, *scopes, text); !ok)
                return dftracer::utils::unexpected(ok.error());
    if (tree->pipeline)
        if (auto ok = expand(*tree->pipeline, *scopes, text); !ok)
            return dftracer::utils::unexpected(ok.error());
    try {
        duql::Program out =
            Lowering(params, text, roles).program(*tree, rowsets);
        out.args_fallback = fallback;
        return out;
    } catch (const Failure& f) {
        return dftracer::utils::unexpected(f.error);
    }
}

void for_each_pipeline_term(const duql::Pipeline& p,
                            const std::function<void(const duql::Term&)>& fn) {
    auto on = [&](const duql::TermRef& t) {
        if (t) duql::for_each_term(*t, fn);
    };
    auto items = [&](const std::vector<duql::PipelineItem>& v) {
        for (const auto& it : v) on(it.term);
    };
    auto keys = [&](const std::vector<duql::PipelineSortKey>& v) {
        for (const auto& k : v) on(k.key.term);
    };
    std::function<void(const duql::QueryNode&)> node =
        [&](const duql::QueryNode& n) {
            std::visit(
                [&](const auto& x) {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, duql::AndNode> ||
                                  std::is_same_v<T, duql::OrNode>) {
                        node(*x.left);
                        node(*x.right);
                    } else if constexpr (std::is_same_v<T, duql::NotNode>) {
                        node(*x.operand);
                    } else if constexpr (std::is_same_v<T, duql::ExprLeaf>) {
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
                    for (const auto& c : s.calls) on(c.arg);
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

dftracer::utils::expected<duql::LiteralValue, duql::DuqlError> parse_literal(
    std::string_view text) {
    auto tree = syntax::parse(text);
    if (!tree) return dftracer::utils::unexpected(tree.error());
    if (tree->decls.empty() && tree->pipeline &&
        tree->pipeline->sources.empty() && tree->pipeline->stages.size() == 1)
        if (const auto* w =
                std::get_if<Where>(&tree->pipeline->stages.front().node))
            if (const duql::Params none;
                auto lit = Lowering(none, text).value_of(*w->condition))
                return std::move(lit->value);
    return dftracer::utils::unexpected(make_error(
        text, 0, text.size(),
        "a parameter value must be a number, a string, true or false"));
}

dftracer::utils::expected<duql::QueryNodePtr, duql::DuqlError> lower_filter(
    syntax::Program program, const duql::Params& params,
    std::string_view source) {
    std::vector<Def> defs = duql::take_defs(program.decls);
    auto scopes = scopes_of(defs, source, {});
    if (!scopes) return dftracer::utils::unexpected(scopes.error());
    if (program.pipeline)
        if (auto ok = expand(*program.pipeline, *scopes, source); !ok)
            return dftracer::utils::unexpected(ok.error());
    try {
        return Lowering(params, source).filter(program);
    } catch (const Failure& f) {
        return dftracer::utils::unexpected(f.error);
    }
}

}  // namespace dftracer::utils::duql
