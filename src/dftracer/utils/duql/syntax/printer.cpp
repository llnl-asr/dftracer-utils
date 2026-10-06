#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/duql/string_literal.h>
#include <dftracer/utils/duql/syntax/tree.h>
#include <dftracer/utils/duql/term.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

namespace dftracer::utils::duql::syntax {

static bool eq(const Expr& a, const Expr& b);
static bool eq(const Pipeline& a, const Pipeline& b);

static bool eq(const ExprPtr& a, const ExprPtr& b) {
    if (!a || !b) return !a && !b;
    return eq(*a, *b);
}

static bool eq(const PipelinePtr& a, const PipelinePtr& b) {
    if (!a || !b) return !a && !b;
    return eq(*a, *b);
}

template <class T>
static bool eq(const std::vector<T>& a, const std::vector<T>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!eq(a[i], b[i])) return false;
    }
    return true;
}

template <class... Ts>
static bool eq(const std::variant<Ts...>& a, const std::variant<Ts...>& b) {
    if (a.index() != b.index()) return false;
    return std::visit(
        [&](const auto& x) {
            return eq(x, std::get<std::decay_t<decltype(x)>>(b));
        },
        a);
}

static bool eq(const Literal& a, const Literal& b) { return a == b; }
static bool eq(const Duration& a, const Duration& b) { return a == b; }
static bool eq(const Path& a, const Path& b) { return a == b; }
static bool eq(const Param& a, const Param& b) { return a == b; }

static bool eq(const Unary& a, const Unary& b) {
    return a.op == b.op && eq(a.operand, b.operand);
}
static bool eq(const Binary& a, const Binary& b) {
    return a.op == b.op && eq(a.left, b.left) && eq(a.right, b.right);
}
static bool eq(const In& a, const In& b) {
    return a.negated == b.negated && a.list_param == b.list_param &&
           eq(a.subject, b.subject) && eq(a.list, b.list) &&
           eq(a.subquery, b.subquery);
}
static bool eq(const Contains& a, const Contains& b) {
    return a.text == b.text && a.negated == b.negated && a.path == b.path &&
           a.any == b.any;
}
static bool eq(const Between& a, const Between& b) {
    return a.negated == b.negated && eq(a.subject, b.subject) &&
           eq(a.low, b.low) && eq(a.high, b.high);
}
static bool eq(const Like& a, const Like& b) {
    return a.negated == b.negated && a.icase == b.icase &&
           a.pattern == b.pattern && a.escape == b.escape &&
           a.pattern_param == b.pattern_param && eq(a.subject, b.subject);
}
static bool eq(const Is& a, const Is& b) {
    return a.negated == b.negated && a.missing == b.missing &&
           eq(a.subject, b.subject);
}
static bool eq(const Arrow& a, const Arrow& b) {
    return a.rowset == b.rowset && a.target_key == b.target_key &&
           a.path == b.path && eq(a.key, b.key);
}
static bool eq(const Arg& a, const Arg& b) {
    return a.name == b.name && eq(a.value, b.value);
}
static bool eq(const Call& a, const Call& b) {
    return a.name == b.name && eq(a.args, b.args);
}
static bool eq(const Over& a, const Over& b) {
    return a.rows == b.rows && eq(a.call, b.call) && eq(a.width, b.width);
}
static bool eq(const List& a, const List& b) { return eq(a.items, b.items); }
static bool eq(const Tuple& a, const Tuple& b) { return eq(a.items, b.items); }
static bool eq(const Subquery& a, const Subquery& b) {
    return eq(a.pipeline, b.pipeline);
}

static bool eq(const Index& a, const Index& b) {
    return eq(a.base, b.base) && eq(a.index, b.index);
}

static bool eq(const Expr& a, const Expr& b) { return eq(a.node, b.node); }

static bool eq(const Assign& a, const Assign& b) {
    return a.name == b.name && eq(a.value, b.value);
}
static bool eq(const Item& a, const Item& b) {
    return a.name == b.name && eq(a.value, b.value);
}
static bool eq(const SortKey& a, const SortKey& b) {
    return a.descending == b.descending && a.nulls == b.nulls &&
           eq(a.value, b.value);
}
static bool eq(const std::pair<ExprPtr, ExprPtr>& a,
               const std::pair<ExprPtr, ExprPtr>& b) {
    return eq(a.first, b.first) && eq(a.second, b.second);
}

static bool eq(const Where& a, const Where& b) {
    return eq(a.condition, b.condition);
}
static bool eq(const Derive& a, const Derive& b) {
    return eq(a.fields, b.fields);
}
static bool eq(const Parse& a, const Parse& b) {
    return a.column == b.column && eq(a.pattern, b.pattern);
}
static bool eq(const Select& a, const Select& b) {
    return eq(a.items, b.items);
}
static bool eq(const Drop& a, const Drop& b) { return a.paths == b.paths; }
static bool eq(const Rename& a, const Rename& b) { return a.pairs == b.pairs; }
static bool eq(const Distinct& a, const Distinct& b) {
    return eq(a.keys, b.keys);
}
static bool eq(const Group& a, const Group& b) {
    return eq(a.keys, b.keys) && eq(a.aggregates, b.aggregates);
}
static bool eq(const Agg& a, const Agg& b) {
    return eq(a.aggregates, b.aggregates);
}
static bool eq(const Window& a, const Window& b) {
    return eq(a.partition, b.partition) && eq(a.order, b.order) &&
           eq(a.fields, b.fields);
}
static bool eq(const Pivot& a, const Pivot& b) {
    return eq(a.key, b.key) && eq(a.values, b.values) && a.labels == b.labels &&
           eq(a.aggregates, b.aggregates);
}
static bool eq(const Unpivot& a, const Unpivot& b) {
    return a.paths == b.paths && a.key_name == b.key_name &&
           a.value_name == b.value_name;
}
static bool eq(const Sort& a, const Sort& b) { return eq(a.keys, b.keys); }
static bool eq(const Take& a, const Take& b) {
    return a.count == b.count && a.last == b.last && eq(a.by, b.by) &&
           eq(a.order, b.order);
}
static bool eq(const Skip& a, const Skip& b) { return a.count == b.count; }
static bool eq(const Sample& a, const Sample& b) {
    return a.amount == b.amount && a.percent == b.percent && a.seed == b.seed;
}
static bool eq(const Expand& a, const Expand& b) {
    return a.path == b.path && a.as == b.as && a.with_index == b.with_index &&
           a.keep_empty == b.keep_empty;
}
static bool eq(const Lookup& a, const Lookup& b) {
    return a.rowset == b.rowset && a.kind == b.kind && a.into == b.into &&
           eq(a.side, b.side) && eq(a.keys, b.keys);
}
static bool eq(const AsofLookup& a, const AsofLookup& b) {
    return a.rowset == b.rowset && eq(a.side, b.side) &&
           a.direction == b.direction && eq(a.within, b.within) &&
           eq(a.keys, b.keys) && eq(a.time, b.time);
}
static bool eq(const OverlapLookup& a, const OverlapLookup& b) {
    return a.rowset == b.rowset && a.into == b.into && eq(a.side, b.side) &&
           eq(a.keys, b.keys);
}
static bool eq(const Union& a, const Union& b) { return eq(a.other, b.other); }
static bool eq(const CallStage& a, const CallStage& b) {
    return eq(a.call, b.call);
}
static bool eq(const TimeRange& a, const TimeRange& b) {
    return a.overlap == b.overlap && eq(a.low, b.low) && eq(a.high, b.high);
}
static bool eq(const CallTree&, const CallTree&) { return true; }
static bool eq(const Use& a, const Use& b) { return eq(a.call, b.call); }
static bool eq(const Bucket& a, const Bucket& b) {
    return a.fill == b.fill && a.fill_mode == b.fill_mode && a.as == b.as &&
           eq(a.width, b.width) && eq(a.every, b.every) && eq(a.at, b.at) &&
           eq(a.low, b.low) && eq(a.high, b.high);
}

static bool eq(const Session& a, const Session& b) {
    return eq(a.keys, b.keys) && eq(a.gap, b.gap) && eq(a.max, b.max) &&
           a.as == b.as;
}

static bool eq(const Stage& a, const Stage& b) { return eq(a.node, b.node); }

static bool eq(const From& a, const From& b) {
    return a.quoted == b.quoted && a.name == b.name;
}

static bool eq(const Pipeline& a, const Pipeline& b) {
    return eq(a.sources, b.sources) && eq(a.stages, b.stages);
}

static bool eq(const Let& a, const Let& b) {
    return a.name == b.name && eq(a.pipeline, b.pipeline);
}
static bool eq(const Def& a, const Def& b) {
    if (a.name != b.name || a.params != b.params ||
        a.body.index() != b.body.index())
        return false;
    if (a.body.index() == 0)
        return eq(std::get<0>(a.body), std::get<0>(b.body));
    return eq(std::get<1>(a.body), std::get<1>(b.body));
}
static bool eq(const RowSet& a, const RowSet& b) {
    return a.name == b.name && eq(a.pipeline, b.pipeline);
}
static bool eq(const SourceDecl& a, const SourceDecl& b) {
    return a.name == b.name && eq(a.rowsets, b.rowsets) && eq(a.defs, b.defs);
}

bool equal(const Program& a, const Program& b) {
    return eq(a.decls, b.decls) && eq(a.pipeline, b.pipeline);
}

namespace {

enum Prec : int {
    OR = 1,
    AND,
    NOT,
    CMP,
    ADD,
    MUL,
    NEG,
    COALESCE,
    POSTFIX,
    PRIMARY,
};

constexpr std::array<std::string_view, 12> RESERVED = {
    "and",     "or", "not",    "in",   "like", "ilike",
    "between", "is", "escape", "null", "true", "false"};

bool is_ident(std::string_view s) {
    if (s.empty()) return false;
    auto head = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    if (!head(s[0])) return false;
    for (char c : s) {
        if (!head(c) && !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

bool is_reserved(std::string_view s) {
    for (std::string_view r : RESERVED) {
        if (r.size() != s.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < s.size() && same; ++i) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            same = c == r[i];
        }
        if (same) return true;
    }
    return false;
}

void put_string(std::string& out, const std::string& raw) {
    out += quote_string(raw);
}

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

void put_key(std::string& out, const PathStep& step, bool first) {
    if (!step.quoted && ((is_ident(step.key) && !is_reserved(step.key)) ||
                         (!first && all_digits(step.key)))) {
        out += step.key;
    } else {
        out += '`';
        out += step.key;
        out += '`';
    }
}

void put_steps(std::string& out, const std::vector<PathStep>& steps) {
    bool first = true;
    for (const PathStep& step : steps) {
        if (step.wildcard()) {
            out += ".*";
        } else if (!step.key.empty() || !step.index) {
            if (!first) out += '.';
            put_key(out, step, first);
        }
        if (step.index) {
            out += '[';
            out += std::to_string(*step.index);
            out += ']';
        }
        first = false;
    }
}

void put_path(std::string& out, const Path& path) {
    if (path.root == PathRoot::CURRENT) out += '.';
    if (path.root == PathRoot::ENCLOSING) out += "^.";
    put_steps(out, path.steps);
}

std::string number(const Literal& lit) {
    if (!lit.number_text.empty()) return lit.number_text;
    if (const auto* i = std::get_if<std::int64_t>(&lit.value)) {
        return std::to_string(*i);
    }
    if (const auto* u = std::get_if<std::uint64_t>(&lit.value)) {
        return std::to_string(*u);
    }
    std::string s = double_text(std::get<double>(lit.value));
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

class Printer {
   public:
    std::string out;

    void expr(const Expr& e, int min_prec) {
        std::string text;
        std::swap(text, out);
        int prec = node(e.node);
        std::swap(text, out);
        if (prec < min_prec) {
            out += '(';
            out += text;
            out += ')';
        } else {
            out += text;
        }
    }

    void expr(const ExprPtr& e, int min_prec) {
        assert(e);
        if (e) expr(*e, min_prec);
    }

    void exprs(const std::vector<ExprPtr>& items) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i) out += ", ";
            expr(items[i], OR);
        }
    }

    void inline_pipeline(const Pipeline& p) { pipeline(p, " | "); }

    void pipeline(const Pipeline& p, std::string_view sep) {
        bool first = true;
        if (!p.sources.empty()) {
            out += "from ";
            for (std::size_t i = 0; i < p.sources.size(); ++i) {
                if (i) out += ", ";
                source(p.sources[i]);
            }
            first = false;
        }
        for (const Stage& s : p.stages) {
            if (!first) out += sep;
            stage(s.node);
            first = false;
        }
    }

    void subquery(const PipelinePtr& p) {
        assert(p && !p->sources.empty());
        out += '(';
        if (p) inline_pipeline(*p);
        out += ')';
    }

    void def(const Def& d) {
        out += "def ";
        out += d.name;
        if (!d.params.empty()) {
            out += '(';
            for (std::size_t i = 0; i < d.params.size(); ++i) {
                if (i) out += ", ";
                out += d.params[i];
            }
            out += ')';
        }
        out += " = ";
        if (const auto* p = std::get_if<PipelinePtr>(&d.body))
            inline_pipeline(**p);
        else
            expr(std::get<ExprPtr>(d.body), OR);
    }

   private:
    void source(const From& f) {
        if (const auto* p = std::get_if<Param>(&f.name)) {
            out += '$';
            out += p->name;
        } else if (f.quoted) {
            put_string(out, std::get<std::string>(f.name));
        } else {
            out += std::get<std::string>(f.name);
        }
    }

    int node(const ExprNode& n) {
        return std::visit([&](const auto& x) { return print(x); }, n);
    }

    int print(const Literal& lit) {
        const auto& v = lit.value;
        if (std::holds_alternative<Null>(v)) {
            out += "null";
        } else if (const auto* b = std::get_if<bool>(&v)) {
            out += *b ? "true" : "false";
        } else if (const auto* s = std::get_if<std::string>(&v)) {
            put_string(out, *s);
        } else {
            std::string text = number(lit);
            out += text;
            if (!text.empty() && text[0] == '-') return NEG;
        }
        return PRIMARY;
    }

    int print(const Duration& d) {
        out += d.amount;
        out += d.unit;
        return PRIMARY;
    }

    int print(const Path& p) {
        put_path(out, p);
        return PRIMARY;
    }

    int print(const Param& p) {
        out += '$';
        out += p.name;
        return PRIMARY;
    }

    int print(const Unary& u) {
        if (u.op == UnaryOp::NOT) {
            out += "not ";
            expr(u.operand, CMP);
            return NOT;
        }
        out += '-';
        expr(u.operand, COALESCE);
        return NEG;
    }

    int print(const Binary& b) {
        struct Info {
            std::string_view text;
            int prec;
            int left;
            int right;
        };
        auto info = [](BinaryOp op) -> Info {
            switch (op) {
                case BinaryOp::OR:
                    return {"or", OR, OR, AND};
                case BinaryOp::AND:
                    return {"and", AND, AND, NOT};
                case BinaryOp::EQ:
                    return {"==", CMP, ADD, ADD};
                case BinaryOp::NE:
                    return {"!=", CMP, ADD, ADD};
                case BinaryOp::LT:
                    return {"<", CMP, ADD, ADD};
                case BinaryOp::LE:
                    return {"<=", CMP, ADD, ADD};
                case BinaryOp::GT:
                    return {">", CMP, ADD, ADD};
                case BinaryOp::GE:
                    return {">=", CMP, ADD, ADD};
                case BinaryOp::REGEX:
                    return {"~", CMP, ADD, ADD};
                case BinaryOp::IREGEX:
                    return {"~*", CMP, ADD, ADD};
                case BinaryOp::NREGEX:
                    return {"!~", CMP, ADD, ADD};
                case BinaryOp::NIREGEX:
                    return {"!~*", CMP, ADD, ADD};
                case BinaryOp::ADD:
                    return {"+", ADD, ADD, MUL};
                case BinaryOp::SUB:
                    return {"-", ADD, ADD, MUL};
                case BinaryOp::MUL:
                    return {"*", MUL, MUL, NEG};
                case BinaryOp::DIV:
                    return {"/", MUL, MUL, NEG};
                case BinaryOp::IDIV:
                    return {"//", MUL, MUL, NEG};
                case BinaryOp::MOD:
                    return {"%", MUL, MUL, NEG};
                case BinaryOp::COALESCE:
                    return {"??", COALESCE, COALESCE, POSTFIX};
            }
            return {"?", PRIMARY, PRIMARY, PRIMARY};
        };
        Info i = info(b.op);
        expr(b.left, i.left);
        out += ' ';
        out += i.text;
        out += ' ';
        expr(b.right, i.right);
        return i.prec;
    }

    int print(const In& in) {
        expr(in.subject, ADD);
        out += in.negated ? " not in " : " in ";
        if (!in.list_param.empty()) {
            out += '$';
            out += in.list_param;
        } else if (in.subquery) {
            subquery(in.subquery);
        } else {
            out += '[';
            exprs(in.list);
            out += ']';
        }
        return CMP;
    }

    int print(const Contains& c) {
        put_string(out, c.text);
        out += c.negated ? " not in " : " in ";
        if (c.any) out += "any(";
        put_path(out, c.path);
        if (c.any) out += ')';
        return CMP;
    }

    int print(const Between& b) {
        expr(b.subject, ADD);
        out += b.negated ? " not between " : " between ";
        expr(b.low, ADD);
        out += " and ";
        expr(b.high, ADD);
        return CMP;
    }

    int print(const Like& l) {
        expr(l.subject, ADD);
        out += l.negated ? " not " : " ";
        out += l.icase ? "ilike " : "like ";
        if (l.pattern_param.empty()) {
            put_string(out, l.pattern);
        } else {
            out += '$';
            out += l.pattern_param;
        }
        if (l.escape) {
            out += " escape ";
            put_string(out, *l.escape);
        }
        return CMP;
    }

    int print(const Is& is) {
        expr(is.subject, ADD);
        out += is.negated ? " is not " : " is ";
        out += is.missing ? "missing" : "null";
        return CMP;
    }

    int print(const Arrow& a) {
        expr(a.key, POSTFIX);
        out += " -> ";
        out += a.rowset;
        if (a.target_key) {
            out += '(';
            put_path(out, *a.target_key);
            out += ')';
        }
        out += '.';
        put_steps(out, a.path.steps);
        return POSTFIX;
    }

    int print(const Call& c) {
        out += c.name;
        out += '(';
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            if (i) out += ", ";
            if (!c.args[i].name.empty()) {
                out += c.args[i].name;
                out += " = ";
            }
            expr(c.args[i].value, OR);
        }
        out += ')';
        return PRIMARY;
    }

    int print(const Over& o) {
        expr(o.call, POSTFIX);
        out += " over ";
        expr(o.width, PRIMARY);
        if (o.rows) out += " rows";
        return POSTFIX;
    }

    int print(const List& l) {
        out += '[';
        exprs(l.items);
        out += ']';
        return PRIMARY;
    }

    int print(const Tuple& t) {
        out += '(';
        exprs(t.items);
        out += ')';
        return PRIMARY;
    }

    int print(const Index& i) {
        expr(i.base, POSTFIX);
        out += '[';
        expr(i.index, OR);
        out += ']';
        return POSTFIX;
    }

    int print(const Subquery& s) {
        subquery(s.pipeline);
        return PRIMARY;
    }

    void assigns(const std::vector<Assign>& fields) {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i) out += ", ";
            out += fields[i].name;
            out += " = ";
            expr(fields[i].value, OR);
        }
    }

    void block(const std::vector<Assign>& fields) {
        out += "{ ";
        assigns(fields);
        out += fields.empty() ? "}" : " }";
    }

    void items(const std::vector<Item>& list) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (i) out += ", ";
            if (!list[i].name.empty()) {
                out += list[i].name;
                out += " = ";
            }
            expr(list[i].value, OR);
        }
    }

    void keys(const std::vector<SortKey>& list) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (i) out += ", ";
            const SortKey& k = list[i];
            if (k.descending) {
                out += '-';
                expr(k.value, COALESCE);
            } else {
                std::size_t at = out.size();
                expr(k.value, OR);
                if (out.size() > at && out[at] == '-') {
                    out.insert(at, 1, '(');
                    out += ')';
                }
            }
            if (k.nulls == NullsOrder::FIRST) out += " nulls first";
            if (k.nulls == NullsOrder::LAST) out += " nulls last";
        }
    }

    void paths(const std::vector<Path>& list) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (i) out += ", ";
            put_path(out, list[i]);
        }
    }

    void stage(const StageNode& n) {
        std::visit([&](const auto& x) { print_stage(x); }, n);
    }

    void print_stage(const Where& s) {
        out += "where ";
        expr(s.condition, OR);
    }

    void print_stage(const Derive& s) {
        out += "derive ";
        assigns(s.fields);
    }

    void print_stage(const Parse& s) {
        out += "parse ";
        put_path(out, s.column);
        out += " ~ ";
        expr(s.pattern, OR);
    }

    void print_stage(const Select& s) {
        out += "select ";
        items(s.items);
    }

    void print_stage(const Drop& s) {
        out += "drop ";
        paths(s.paths);
    }

    void print_stage(const Rename& s) {
        out += "rename ";
        for (std::size_t i = 0; i < s.pairs.size(); ++i) {
            if (i) out += ", ";
            out += s.pairs[i].first;
            out += " = ";
            put_path(out, s.pairs[i].second);
        }
    }

    void print_stage(const Distinct& s) {
        out += "distinct";
        if (!s.keys.empty()) {
            out += ' ';
            items(s.keys);
        }
    }

    void print_stage(const Group& s) {
        out += "group ";
        if (!s.keys.empty()) {
            items(s.keys);
            out += ' ';
        }
        block(s.aggregates);
    }

    void print_stage(const Agg& s) {
        out += "agg ";
        block(s.aggregates);
    }

    void print_stage(const Window& s) {
        out += "window ";
        if (!s.partition.empty()) {
            items(s.partition);
            out += ' ';
        }
        if (!s.order.empty()) {
            out += "sort ";
            keys(s.order);
            out += ' ';
        }
        block(s.fields);
    }

    void print_stage(const Pivot& s) {
        out += "pivot ";
        expr(s.key, ADD);
        if (!s.values.empty()) {
            out += " in [";
            for (std::size_t i = 0; i < s.values.size(); ++i) {
                if (i) out += ", ";
                expr(s.values[i], OR);
                if (i < s.labels.size() && !s.labels[i].empty()) {
                    out += " as ";
                    out += s.labels[i];
                }
            }
            out += ']';
        }
        out += ' ';
        block(s.aggregates);
    }

    void print_stage(const Unpivot& s) {
        out += "unpivot ";
        paths(s.paths);
        out += " as ";
        out += s.key_name;
        out += ", ";
        out += s.value_name;
    }

    void print_stage(const Sort& s) {
        out += "sort ";
        keys(s.keys);
    }

    void print_stage(const Take& s) {
        out += "take ";
        out += s.count;
        if (!s.last.empty()) {
            out += "..";
            out += s.last;
        }
        if (!s.by.empty()) {
            out += " by ";
            exprs(s.by);
        }
        if (!s.order.empty()) {
            out += " sort ";
            keys(s.order);
        }
    }

    void print_stage(const Skip& s) {
        out += "skip ";
        out += s.count;
    }

    void print_stage(const Sample& s) {
        out += "sample ";
        out += s.amount;
        if (s.percent) out += '%';
        if (!s.seed.empty()) {
            out += " seed ";
            out += s.seed;
        }
    }

    void print_stage(const Expand& s) {
        out += "expand ";
        put_path(out, s.path);
        if (!s.as.empty()) {
            out += " as ";
            out += s.as;
        }
        if (!s.with_index.empty()) {
            out += " with_index ";
            out += s.with_index;
        }
        if (s.keep_empty) out += " keep_empty";
    }

    void join_key(const std::pair<ExprPtr, ExprPtr>& k) {
        expr(k.first, ADD);
        if (k.second) {
            out += " == ";
            expr(k.second, ADD);
        }
    }

    void join_keys(const std::string& rowset, const PipelinePtr& side,
                   const std::vector<std::pair<ExprPtr, ExprPtr>>& keys) {
        out += "lookup ";
        if (side)
            subquery(side);
        else
            out += rowset;
        out += " on ";
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i) out += ", ";
            join_key(keys[i]);
        }
    }

    void print_stage(const Lookup& s) {
        join_keys(s.rowset, s.side, s.keys);
        if (s.kind == LookupKind::INNER) out += " inner";
        if (s.kind == LookupKind::ANTI) out += " anti";
        if (!s.into.empty()) {
            out += " into ";
            out += s.into;
        }
    }

    void print_stage(const OverlapLookup& s) {
        join_keys(s.rowset, s.side, s.keys);
        out += " overlap";
        if (!s.into.empty()) {
            out += " into ";
            out += s.into;
        }
    }

    void print_stage(const AsofLookup& s) {
        join_keys(s.rowset, s.side, s.keys);
        out += " asof ";
        join_key(s.time);
        switch (s.direction) {
            case AsofDirection::BACKWARD:
                break;
            case AsofDirection::FORWARD:
                out += " forward";
                break;
            case AsofDirection::NEAREST:
                out += " nearest";
                break;
        }
        if (s.within) {
            out += " within ";
            expr(s.within, ADD);
        }
    }

    void print_stage(const Union& s) {
        out += "union ";
        subquery(s.other);
    }

    void print_stage(const Use& s) { print(s.call); }

    void print_stage(const CallStage& s) {
        out += "call ";
        print(s.call);
    }

    void print_stage(const TimeRange& s) {
        out += "time_range ";
        if (s.low) {
            expr(s.low, OR);
            out += ' ';
        }
        out += "..";
        if (s.high) {
            out += ' ';
            expr(s.high, OR);
        }
        if (s.overlap) out += " overlap";
    }

    void print_stage(const CallTree&) { out += "call_tree"; }

    void print_stage(const Bucket& s) {
        out += "bucket ";
        expr(s.width, OR);
        if (s.every) {
            out += " every ";
            expr(s.every, OR);
        }
        if (s.at) {
            out += " at ";
            expr(s.at, OR);
        }
        if (s.fill) out += " fill";
        if (s.fill_mode == FillMode::FORWARD) out += " forward";
        if (s.fill_mode == FillMode::LINEAR) out += " linear";
        if (s.low) {
            out += " from ";
            expr(s.low, OR);
            out += " to ";
            expr(s.high, OR);
        }
        if (!s.as.empty()) {
            out += " as ";
            out += s.as;
        }
    }

    void print_stage(const Session& s) {
        out += "session ";
        if (!s.keys.empty()) {
            items(s.keys);
            out += ' ';
        }
        out += "gap ";
        expr(s.gap, OR);
        if (s.max) {
            out += " max ";
            expr(s.max, OR);
        }
        if (!s.as.empty()) {
            out += " as ";
            out += s.as;
        }
    }
};

}  // namespace

std::string to_text(const Expr& expr) {
    Printer p;
    p.expr(expr, OR);
    return std::move(p.out);
}

std::string to_text(const Pipeline& pipeline) {
    Printer p;
    p.inline_pipeline(pipeline);
    return std::move(p.out);
}

std::string to_text(const Program& query) {
    Printer p;
    p.out += "duql " DFTRACER_UTILS_DUQL_VERSION_STRING "\n";
    for (const Decl& decl : query.decls) {
        if (const auto* let = std::get_if<Let>(&decl)) {
            p.out += "let ";
            p.out += let->name;
            p.out += " = ";
            if (let->pipeline) p.inline_pipeline(*let->pipeline);
            p.out += ";\n";
        } else if (const auto* def = std::get_if<Def>(&decl)) {
            p.def(*def);
            p.out += ";\n";
        } else {
            const auto& src = std::get<SourceDecl>(decl);
            p.out += "source ";
            p.out += src.name;
            p.out += " {\n";
            for (const RowSet& rs : src.rowsets) {
                p.out += "  ";
                p.out += rs.name;
                p.out += " = ";
                if (rs.pipeline) p.inline_pipeline(*rs.pipeline);
                p.out += ";\n";
            }
            for (const Def& d : src.defs) {
                p.out += "  ";
                p.def(d);
                p.out += ";\n";
            }
            p.out += "}\n";
        }
    }
    if (query.pipeline) {
        p.pipeline(*query.pipeline, "\n| ");
        if (!query.pipeline->sources.empty() ||
            !query.pipeline->stages.empty()) {
            p.out += '\n';
        }
    }
    return std::move(p.out);
}

}  // namespace dftracer::utils::duql::syntax

namespace dftracer::utils::duql {

namespace {

bool atomic(const Term& t) {
    if (const auto* c = std::get_if<TConst>(&t.node)) {
        if (const auto* i = std::get_if<std::int64_t>(&c->value))
            return *i >= 0;
        if (const auto* d = std::get_if<double>(&c->value)) return *d >= 0;
        return true;
    }
    return std::holds_alternative<TField>(t.node) ||
           std::holds_alternative<TCall>(t.node) ||
           std::holds_alternative<TQuant>(t.node) ||
           std::holds_alternative<TIndex>(t.node) ||
           std::holds_alternative<TList>(t.node) ||
           std::holds_alternative<TLookup>(t.node);
}

const char* op_text(TermOp op) {
    switch (op) {
        case TermOp::NEG:
            return "-";
        case TermOp::NOT:
            return "not ";
        case TermOp::ADD:
            return " + ";
        case TermOp::SUB:
            return " - ";
        case TermOp::MUL:
            return " * ";
        case TermOp::DIV:
            return " / ";
        case TermOp::IDIV:
            return " // ";
        case TermOp::MOD:
            return " % ";
        case TermOp::EQ:
            return " == ";
        case TermOp::NE:
            return " != ";
        case TermOp::LT:
            return " < ";
        case TermOp::LE:
            return " <= ";
        case TermOp::GT:
            return " > ";
        case TermOp::GE:
            return " >= ";
        case TermOp::AND:
            return " and ";
        case TermOp::OR:
            return " or ";
        case TermOp::COALESCE:
            return " ?? ";
    }
    return " ";
}

const char* match_text(MatchOp op, bool negated) {
    switch (op) {
        case MatchOp::LIKE:
            return negated ? " not like " : " like ";
        case MatchOp::ILIKE:
            return negated ? " not ilike " : " ilike ";
        case MatchOp::REGEX:
            return negated ? " !~ " : " ~ ";
        case MatchOp::IREGEX:
            return negated ? " !~* " : " ~* ";
        case MatchOp::ICONTAINS:
            break;
    }
    return negated ? " not in " : " in ";
}

void put_term(std::string& out, const Term& t);

void put_keys(std::string& out, const std::vector<TermPtr>& keys,
              std::size_t first = 0, std::size_t last = SIZE_MAX);

void put_child(std::string& out, const Term& t) {
    if (atomic(t)) {
        put_term(out, t);
        return;
    }
    out += '(';
    put_term(out, t);
    out += ')';
}

// ` on keys, range op bound`: the enclosing row's keys from `first` on.
void put_correlated(std::string& out, const TLookup& n, std::size_t first) {
    const std::size_t bounds = (n.low ? 1 : 0) + (n.high ? 1 : 0);
    const std::size_t keys = n.keys.size() - bounds;
    if (first == n.keys.size()) return;
    out += " on ";
    if (keys > first) put_keys(out, n.keys, first, keys);
    std::size_t at = keys;
    for (const auto& op : {n.low, n.high}) {
        if (!op) continue;
        if (at > first) out += ", ";
        out += "range";
        out += op_text(*op);
        put_child(out, *n.keys[at++]);
    }
}

void put_term(std::string& out, const Term& t) {
    std::visit(
        [&out](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, TConst>) {
                std::visit(
                    [&out](const auto& v) {
                        using V = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<V, TNull>) {
                            out += "null";
                        } else if constexpr (std::is_same_v<V, bool>) {
                            out += v ? "true" : "false";
                        } else if constexpr (std::is_same_v<V, std::string>) {
                            syntax::put_string(out, v);
                        } else if constexpr (std::is_same_v<V, double>) {
                            syntax::Literal lit{v, {}};
                            out += syntax::number(lit);
                        } else {
                            out += std::to_string(v);
                        }
                    },
                    n.value);
            } else if constexpr (std::is_same_v<T, TField>) {
                if (n.root == FieldRoot::ELEMENT) out += '.';
                if (n.root == FieldRoot::OUTER) out += "^.";
                syntax::put_steps(out, n.steps);
            } else if constexpr (std::is_same_v<T, TUnary>) {
                out += op_text(n.op);
                put_child(out, *n.operand);
            } else if constexpr (std::is_same_v<T, TBinary>) {
                put_child(out, *n.left);
                out += op_text(n.op);
                put_child(out, *n.right);
            } else if constexpr (std::is_same_v<T, TBetween>) {
                put_child(out, *n.subject);
                out += n.negated ? " not between " : " between ";
                put_child(out, *n.low);
                out += " and ";
                put_child(out, *n.high);
            } else if constexpr (std::is_same_v<T, TIs>) {
                put_child(out, *n.subject);
                out += n.negated ? " is not " : " is ";
                out += n.missing ? "missing" : "null";
            } else if constexpr (std::is_same_v<T, TIn>) {
                put_child(out, *n.subject);
                out += n.negated ? " not in [" : " in [";
                for (std::size_t i = 0; i < n.list.size(); ++i) {
                    if (i > 0) out += ", ";
                    put_term(out, *n.list[i]);
                }
                out += ']';
            } else if constexpr (std::is_same_v<T, TCall>) {
                out += fn_info(n.fn).name;
                out += '(';
                for (std::size_t i = 0; i < n.args.size(); ++i) {
                    if (i > 0) out += ", ";
                    put_term(out, *n.args[i]);
                }
                out += ')';
            } else if constexpr (std::is_same_v<T, TIndex>) {
                put_child(out, *n.array);
                out += '[';
                put_term(out, *n.index);
                out += ']';
            } else if constexpr (std::is_same_v<T, TList>) {
                out += '[';
                for (std::size_t i = 0; i < n.items.size(); ++i) {
                    if (i > 0) out += ", ";
                    put_term(out, *n.items[i]);
                }
                out += ']';
            } else if constexpr (std::is_same_v<T, TQuant>) {
                out += n.all ? "all(" : "any(";
                put_term(out, *n.subject);
                out += ", ";
                put_term(out, *n.cond);
                out += ')';
            } else if constexpr (std::is_same_v<T, TLookup>) {
                switch (n.kind) {
                    case LookupKind::IN:
                        put_keys(out, n.keys, 0, n.keys.size() - n.correlated);
                        out += n.negated ? " not in (" : " in (";
                        out += n.name;
                        put_correlated(out, n, n.keys.size() - n.correlated);
                        out += ')';
                        break;
                    case LookupKind::ARROW:
                        put_keys(out, n.keys);
                        out += " -> ";
                        out += n.name;
                        if (n.keys.size() == 1)
                            if (const auto* f =
                                    std::get_if<TField>(&n.keys[0]->node);
                                !f || n.target.size() != 1 ||
                                f->base != n.target[0]) {
                                out += '(';
                                out += n.target.empty() ? "" : n.target[0];
                                out += ')';
                            }
                        out += '.';
                        out += n.column;
                        break;
                    case LookupKind::SCALAR:
                        out += '(';
                        out += n.name;
                        put_correlated(out, n, 0);
                        out += ')';
                        break;
                }
            } else if constexpr (std::is_same_v<T, TMatch>) {
                if (n.op == MatchOp::ICONTAINS) {
                    syntax::put_string(out, n.pattern);
                    out += match_text(n.op, n.negated);
                    put_child(out, *n.subject);
                } else {
                    put_child(out, *n.subject);
                    out += match_text(n.op, n.negated);
                    syntax::put_string(out, n.pattern);
                    if (n.escape) {
                        out += " escape ";
                        syntax::put_string(out, std::string(1, *n.escape));
                    }
                }
            }
        },
        t.node);
}

void put_keys(std::string& out, const std::vector<TermPtr>& keys,
              std::size_t first, std::size_t last) {
    last = std::min(last, keys.size());
    if (last - first == 1) {
        put_child(out, *keys[first]);
        return;
    }
    out += '(';
    for (std::size_t i = first; i < last; ++i) {
        if (i > first) out += ", ";
        put_term(out, *keys[i]);
    }
    out += ')';
}

}  // namespace

std::string term_text(const Term& t) {
    std::string out;
    put_term(out, t);
    return out;
}

}  // namespace dftracer::utils::duql
