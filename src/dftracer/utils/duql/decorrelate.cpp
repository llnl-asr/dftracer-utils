#include <dftracer/utils/duql/decorrelate.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/syntax/walk.h>

#include <algorithm>
#include <array>
#include <utility>

namespace dftracer::utils::duql {

namespace {

using namespace syntax;

constexpr std::array<const char*, std::variant_size_v<StageNode>> STAGE_NAMES =
    {"where",   "derive", "select", "drop",       "rename",    "distinct",
     "group",   "agg",    "window", "pivot",      "unpivot",   "sort",
     "take",    "skip",   "sample", "expand",     "lookup",    "lookup",
     "lookup",  "union",  "call",   "time_range", "call_tree", "bucket",
     "session", "parse",  "use"};

ExprPtr expr_of(ExprNode node) {
    return std::make_unique<Expr>(Expr{std::move(node), Span{}});
}

ExprPtr field_of(const std::string& name) {
    Path p;
    p.steps.push_back(PathStep{name, false, std::nullopt});
    return expr_of(std::move(p));
}

ExprPtr bool_literal(bool v) {
    Literal l;
    l.value = v;
    return expr_of(std::move(l));
}

ExprPtr and_of(std::vector<ExprPtr> terms) {
    ExprPtr out = std::move(terms.front());
    for (std::size_t i = 1; i < terms.size(); ++i)
        out =
            expr_of(Binary{BinaryOp::AND, std::move(out), std::move(terms[i])});
    return out;
}

// Calls `fn(expr, path)` for each `^.` path of `e` that reads the row around
// the sub-query: not inside a nested sub-query, and not inside the condition
// of any() or all(), where `^.` is the quantified record.
template <class F>
void enclosing(Expr& e, int quant, F& fn) {
    if (auto* p = std::get_if<Path>(&e.node)) {
        if (p->root == PathRoot::ENCLOSING && quant == 0) fn(e, *p);
        return;
    }
    if (std::holds_alternative<Subquery>(e.node)) return;
    if (auto* in = std::get_if<In>(&e.node)) {
        enclosing(*in->subject, quant, fn);
        for (auto& x : in->list) enclosing(*x, quant, fn);
        return;
    }
    if (auto* c = std::get_if<Call>(&e.node);
        c && (c->name == "any" || c->name == "all") && c->args.size() == 2) {
        enclosing(*c->args[0].value, quant, fn);
        enclosing(*c->args[1].value, quant + 1, fn);
        return;
    }
    children(e, [&](ExprPtr& child) { enclosing(*child, quant, fn); });
}

// The expression slots of stage `s`, sub-query stages excluded.
template <class F>
void slots(Stage& s, F&& fn) {
    Pipeline one;
    one.stages.push_back(std::move(s));
    each_slot(one, fn);
    s = std::move(one.stages.front());
}

struct Copy {
    std::string text;
    PipelinePtr pipeline;
};

Copy copy_of(const Pipeline& p) {
    Copy c;
    c.text = to_text(p);
    auto tree = syntax::parse(c.text);
    c.pipeline = std::move(tree->pipeline);
    return c;
}

[[noreturn]] void refuse(const std::string& text, Span span,
                         std::string message) {
    throw Refusal{text, span, std::move(message)};
}

void flatten(ExprPtr& e, std::vector<ExprPtr*>& out) {
    if (auto* b = std::get_if<Binary>(&e->node); b && b->op == BinaryOp::AND) {
        flatten(b->left, out);
        flatten(b->right, out);
        return;
    }
    out.push_back(&e);
}

struct Rewrite {
    std::vector<Stage> stages;
    std::vector<ExprPtr> outer;
    std::optional<CorrelatedBound> low;
    std::optional<CorrelatedBound> high;
};

std::size_t count_enclosing(Expr& e) {
    std::size_t n = 0;
    auto count = [&](Expr&, Path&) { ++n; };
    enclosing(e, 0, count);
    return n;
}

// Whether `e` reads the sub-query's own row outside nested sub-queries and
// the conditions of any() and all().
bool reads_own(Expr& e, int quant = 0) {
    if (auto* p = std::get_if<Path>(&e.node))
        return p->root != PathRoot::ENCLOSING && quant == 0;
    if (std::holds_alternative<Subquery>(e.node)) return false;
    if (auto* c = std::get_if<Call>(&e.node);
        c && (c->name == "any" || c->name == "all") && c->args.size() == 2)
        return reads_own(*c->args[0].value, quant) ||
               reads_own(*c->args[1].value, quant + 1);
    bool out = false;
    children(e, [&](ExprPtr& child) { out = out || reads_own(*child, quant); });
    return out;
}

void set_span(Expr& e, Span span) {
    e.span = span;
    children(e, [&](ExprPtr& c) { set_span(*c, span); });
}

// A copy of `e` with `span`: its canonical text parsed again.
ExprPtr clone(const Expr& e, Span span) {
    auto tree = syntax::parse("where " + to_text(e));
    ExprPtr out = std::move(
        std::get<Where>(tree->pipeline->stages.front().node).condition);
    set_span(*out, span);
    return out;
}

BinaryOp flipped(BinaryOp op) {
    switch (op) {
        case BinaryOp::LT:
            return BinaryOp::GT;
        case BinaryOp::LE:
            return BinaryOp::GE;
        case BinaryOp::GT:
            return BinaryOp::LT;
        case BinaryOp::GE:
            return BinaryOp::LE;
        default:
            return op;
    }
}

bool comparison(BinaryOp op) {
    return op == BinaryOp::EQ || op == BinaryOp::LT || op == BinaryOp::LE ||
           op == BinaryOp::GT || op == BinaryOp::GE;
}

// Sorts the correlated terms of one `where` into its keys, the bounds of
// `out` and the terms it keeps.
class Terms {
   public:
    Terms(const std::string& text, Rewrite& out, std::string& range)
        : text_(text), out_(out), range_(range) {}

    std::vector<ExprPtr> kept;
    std::vector<Assign> keys;

    void add(ExprPtr& t) {
        if (count_enclosing(*t) == 0) {
            kept.push_back(std::move(t));
            return;
        }
        if (auto* b = std::get_if<Between>(&t->node);
            b && !b->negated && count_enclosing(*b->subject) == 0) {
            const Span at = t->span;
            ExprPtr low = expr_of(Binary{BinaryOp::GE, clone(*b->subject, at),
                                         std::move(b->low)});
            ExprPtr high = expr_of(Binary{BinaryOp::LE, std::move(b->subject),
                                          std::move(b->high)});
            low->span = at;
            high->span = at;
            add(low);
            add(high);
            return;
        }
        const std::string term = to_text(*t);
        auto* b = std::get_if<Binary>(&t->node);
        const bool ok = b && comparison(b->op);
        const std::size_t l = ok ? count_enclosing(*b->left) : 0;
        const std::size_t r = ok ? count_enclosing(*b->right) : 0;
        if (!ok || (l > 0) == (r > 0))
            refuse(text_, t->span,
                   "'" + term +
                       "' reads the enclosing row only as one side of '==', "
                       "'<', '<=', '>', '>=' or 'between' against the "
                       "sub-query's own fields, in a 'where' term joined by "
                       "'and'; for intervals write 'lookup ... overlap'");
        ExprPtr inner = std::move(l > 0 ? b->right : b->left);
        ExprPtr outer = std::move(l > 0 ? b->left : b->right);
        const BinaryOp op = l > 0 ? flipped(b->op) : b->op;
        if (reads_own(*outer))
            refuse(text_, t->span,
                   "'" + term +
                       "' mixes the enclosing row with the sub-query's own "
                       "fields on one side; compare an expression of each");
        auto unroot = [](Expr&, Path& path) { path.root = PathRoot::RECORD; };
        enclosing(*outer, 0, unroot);
        if (op == BinaryOp::EQ) {
            keys.push_back(Assign{std::string(CORRELATED_KEY_PREFIX) +
                                      std::to_string(out_.outer.size()),
                                  std::move(inner)});
            out_.outer.push_back(std::move(outer));
            return;
        }
        const std::string bounded = to_text(*inner);
        if (range_.empty()) {
            range_ = bounded;
            keys.push_back(Assign{CORRELATED_RANGE, std::move(inner)});
        } else if (bounded != range_) {
            refuse(text_, t->span,
                   "'" + term + "' bounds '" + bounded +
                       "', another term bounds '" + range_ +
                       "'; a correlated range bounds one expression; for "
                       "intervals write 'lookup ... overlap'");
        }
        const bool lower = op == BinaryOp::GT || op == BinaryOp::GE;
        auto& slot = lower ? out_.low : out_.high;
        if (slot)
            refuse(text_, t->span,
                   "'" + term + "' is a second " + (lower ? "lower" : "upper") +
                       " bound of '" + range_ + "'");
        slot = CorrelatedBound{std::move(outer),
                               op == BinaryOp::GT || op == BinaryOp::LT};
    }

   private:
    const std::string& text_;
    Rewrite& out_;
    std::string& range_;
};

Rewrite split(const std::string& text, Pipeline& p, bool keys_derived) {
    Rewrite out;
    std::string range;
    std::optional<std::size_t> start;
    for (auto& s : p.stages) {
        if (auto* w = std::get_if<Where>(&s.node)) {
            std::vector<ExprPtr*> terms;
            flatten(w->condition, terms);
            Terms sorted(text, out, range);
            for (auto* t : terms) sorted.add(*t);
            if (!sorted.kept.empty())
                out.stages.push_back(
                    Stage{Where{and_of(std::move(sorted.kept))}, s.span});
            if (!sorted.keys.empty()) {
                if (!start) start = out.stages.size();
                if (keys_derived)
                    out.stages.push_back(
                        Stage{Derive{std::move(sorted.keys)}, s.span});
            }
            continue;
        }
        auto note = [&](Expr& e, Path&) {
            refuse(text, e.span,
                   "'" + to_text(e) +
                       "' reads the enclosing row in a stage other than "
                       "'where'");
        };
        slots(s, [&](ExprPtr& e) {
            if (e) enclosing(*e, 0, note);
        });
        out.stages.push_back(std::move(s));
    }
    if (!start) return out;
    for (std::size_t i = *start + 1; i < out.stages.size(); ++i) {
        const auto& node = out.stages[i].node;
        const bool terminal = i + 1 == out.stages.size();
        if (std::holds_alternative<Where>(node) ||
            std::holds_alternative<Derive>(node))
            continue;
        if (terminal && (std::holds_alternative<Select>(node) ||
                         std::holds_alternative<Group>(node) ||
                         std::holds_alternative<Agg>(node)))
            continue;
        const bool named = std::holds_alternative<Select>(node) ||
                           std::holds_alternative<Group>(node) ||
                           std::holds_alternative<Agg>(node);
        refuse(text, out.stages[i].span,
               std::string("'") + STAGE_NAMES[node.index()] +
                   (named ? "' must be the last stage of a correlated "
                            "sub-query"
                          : "' after a correlated 'where' depends on the "
                            "rows of other keys; a correlated sub-query "
                            "holds only 'where', 'derive' and a last "
                            "'select', 'group' or 'agg'"));
    }
    return out;
}

bool is_key_derive(const Stage& s) {
    const auto* d = std::get_if<Derive>(&s.node);
    return d && (d->fields.front().name.rfind(CORRELATED_KEY_PREFIX, 0) == 0 ||
                 d->fields.front().name == CORRELATED_RANGE);
}

// The inner key expressions when no later `derive` can redefine what they
// read, taken out of their `derive` stages so the last stage computes them:
// a group key of a field then keeps the column's JSON type. Empty otherwise.
std::vector<Assign> inline_keys(Rewrite& r) {
    std::size_t first = r.stages.size();
    for (std::size_t i = 0; i < r.stages.size(); ++i)
        if (is_key_derive(r.stages[i]))
            first = std::min(first, i);
        else if (i > first && std::holds_alternative<Derive>(r.stages[i].node))
            return {};
    std::vector<Assign> out;
    std::vector<Stage> kept;
    for (auto& st : r.stages) {
        if (!is_key_derive(st)) {
            kept.push_back(std::move(st));
            continue;
        }
        for (auto& a : std::get<Derive>(st.node).fields)
            out.push_back(std::move(a));
    }
    r.stages = std::move(kept);
    return out;
}

void name_terminal(const Rewrite& r, const std::string& text) {
    if (r.stages.empty() ||
        !(std::holds_alternative<Select>(r.stages.back().node) ||
          std::holds_alternative<Group>(r.stages.back().node) ||
          std::holds_alternative<Agg>(r.stages.back().node)))
        refuse(text, Span{},
               "end a correlated sub-query with 'select', 'group' or 'agg' "
               "so that its columns are named");
}

// What a range sub-query's aggregate `e` gives for the rows in range.
RangeRead range_read(const std::string& text, const Expr& e) {
    const auto* c = std::get_if<Call>(&e.node);
    const std::string name = c ? c->name : to_text(e);
    if (c && name == "count") {
        return c->args.empty() ? RangeRead::COUNT : RangeRead::COUNT_VALUES;
    }
    if (c && name == "count_if") return RangeRead::COUNT_IF;
    if (c && name == "sum") return RangeRead::SUM;
    if (c && name == "min") return RangeRead::MIN;
    if (c && name == "max") return RangeRead::MAX;
    if (c && name == "mean") return RangeRead::MEAN;
    refuse(text, e.span,
           "'" + name +
               "' over a correlated range; use count, count_if, sum, min, "
               "max or mean");
}

}  // namespace

std::optional<Decorrelated> decorrelate(const Pipeline& p, bool scalar) {
    Copy main = copy_of(p);
    bool reads = false;
    for (auto& s : main.pipeline->stages)
        slots(s, [&](ExprPtr& e) {
            auto any = [&](Expr&, Path&) { reads = true; };
            if (e) enclosing(*e, 0, any);
        });
    if (!reads) return std::nullopt;

    Copy empty = copy_of(p);
    Decorrelated d;
    d.text = main.text;
    Rewrite r = split(main.text, *main.pipeline, true);
    name_terminal(r, main.text);
    for (std::size_t i = 0; i < r.outer.size(); ++i)
        d.names.push_back(std::string(CORRELATED_KEY_PREFIX) +
                          std::to_string(i));

    std::vector<Assign> inner = inline_keys(r);
    auto key = [&](const std::string& name) {
        for (auto& a : inner)
            if (a.name == name && a.value) return std::move(a.value);
        return field_of(name);
    };
    auto keys = [&] {
        std::vector<Item> items;
        for (const auto& name : d.names) items.push_back(Item{key(name), name});
        return items;
    };
    bool has_empty = false;
    Stage& tail = r.stages.back();
    if (scalar) {
        if (std::holds_alternative<Group>(tail.node))
            refuse(main.text, tail.span,
                   "a correlated sub-query in an expression ends in "
                   "'select' or 'agg', not 'group'");
        const std::size_t width =
            std::holds_alternative<Select>(tail.node)
                ? std::get<Select>(tail.node).items.size()
                : std::get<Agg>(tail.node).aggregates.size();
        if (width != 1)
            refuse(main.text, tail.span,
                   "a sub-query in an expression gives one column; this one "
                   "gives " +
                       std::to_string(width));
        if (auto* s = std::get_if<Select>(&tail.node))
            s->items.front().name = CORRELATED_VALUE;
        else
            std::get<Agg>(tail.node).aggregates.front().name = CORRELATED_VALUE;
    }
    if (r.low || r.high) {
        std::vector<Item> items;
        d.read = RangeRead::ROWS;
        if (auto* s = std::get_if<Select>(&tail.node)) {
            items = std::move(s->items);
        } else if (auto* g = std::get_if<Group>(&tail.node);
                   g && g->aggregates.empty()) {
            items = std::move(g->keys);
        } else if (!scalar) {
            refuse(main.text, tail.span,
                   "an 'in' over a correlated range ends in 'select' or in "
                   "'group' without aggregates");
        } else {
            Assign& a = std::get<Agg>(tail.node).aggregates.front();
            d.read = range_read(main.text, *a.value);
            auto& args = std::get<Call>(a.value->node).args;
            if (!args.empty())
                items.push_back(
                    Item{std::move(args.front().value), CORRELATED_VALUE});
        }
        for (auto& k : keys()) items.push_back(std::move(k));
        items.push_back(Item{key(CORRELATED_RANGE), CORRELATED_RANGE});
        tail.node = Select{std::move(items)};
        d.low = std::move(r.low);
        d.high = std::move(r.high);
    } else if (auto* s = std::get_if<Select>(&tail.node)) {
        for (auto& k : keys()) s->items.push_back(std::move(k));
    } else if (auto* g = std::get_if<Group>(&tail.node)) {
        for (auto& k : keys()) g->keys.push_back(std::move(k));
    } else {
        Group grouped{keys(), std::move(std::get<Agg>(tail.node).aggregates)};
        tail.node = std::move(grouped);
        has_empty = scalar;
    }

    main.pipeline->stages = std::move(r.stages);
    if (has_empty) {
        Rewrite e = split(empty.text, *empty.pipeline, false);
        name_terminal(e, empty.text);
        std::get<Agg>(e.stages.back().node).aggregates.front().name =
            CORRELATED_VALUE;
        std::vector<Stage> stages;
        stages.push_back(Stage{Where{bool_literal(false)}, Span{}});
        for (auto& st : e.stages) stages.push_back(std::move(st));
        empty.pipeline->stages = std::move(stages);
        d.empty = std::move(empty.pipeline);
    }
    d.side = std::move(main.pipeline);
    d.outer = std::move(r.outer);
    return d;
}

}  // namespace dftracer::utils::duql
