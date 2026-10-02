#include <dftracer/utils/duql/syntax/lexer.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/syntax/walk.h>
#include <dftracer/utils/duql/wildcard.h>

#include <algorithm>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>

namespace dftracer::utils::duql {

namespace {

using namespace syntax;

struct Failure {
    DuqlError error;
};

using Segments = std::vector<std::string>;

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

Segments split(std::string_view s) {
    Segments out;
    std::size_t at = 0;
    while (true) {
        const auto dot = s.find('.', at);
        out.emplace_back(s.substr(at, dot - at));
        if (dot == std::string_view::npos) return out;
        at = dot + 1;
    }
}

std::string join(const Segments& s) {
    std::string out;
    for (const auto& x : s) out += (out.empty() ? "" : ".") + x;
    return out;
}

Segments segments_of(const Path& p) {
    Segments out;
    for (const auto& st : p.steps) {
        if (!st.key.empty()) out.push_back(st.key);
        if (st.index) out.push_back(std::to_string(*st.index));
    }
    return out;
}

bool has_wildcard(const Path& p) {
    return std::any_of(p.steps.begin(), p.steps.end(),
                       [](const PathStep& s) { return s.wildcard(); });
}

bool is_ident(std::string_view s) {
    if (s.empty() || all_digits(s.substr(0, 1))) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
               (c >= 'A' && c <= 'Z');
    });
}

// Numeric segments by value and before the others, which go by bytes.
bool segment_less(const std::string& a, const std::string& b) {
    const bool na = all_digits(a);
    const bool nb = all_digits(b);
    if (na != nb) return na;
    if (!na) return a < b;
    const auto skip = [](const std::string& s) {
        const auto k = s.find_first_not_of('0');
        return k == std::string::npos ? std::string_view()
                                      : std::string_view(s).substr(k);
    };
    const std::string_view x = skip(a);
    const std::string_view y = skip(b);
    return x.size() != y.size() ? x.size() < y.size() : x < y;
}

bool path_less(const Segments& a, const Segments& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        segment_less);
}

Path path_of(const Segments& segs) {
    Path p;
    for (const auto& s : segs)
        p.steps.push_back({s, !is_ident(s) && !all_digits(s), {}});
    return p;
}

ExprPtr make(ExprNode node, Span span) {
    return std::make_unique<Expr>(Expr{std::move(node), span});
}

ExprPtr clone(const Expr& e) {
    auto tree = syntax::parse("where " + syntax::to_text(e));
    return std::move(
        std::get<Where>(tree->pipeline->stages.front().node).condition);
}

const Path* quant_pattern(const Expr& e) {
    const auto* c = std::get_if<Call>(&e.node);
    if (!c || (c->name != "any" && c->name != "all") || c->args.size() != 1)
        return nullptr;
    const auto* p = std::get_if<Path>(&c->args[0].value->node);
    return p && has_wildcard(*p) ? p : nullptr;
}

bool comparison(const Expr& e) {
    if (const auto* b = std::get_if<Binary>(&e.node)) switch (b->op) {
            case BinaryOp::EQ:
            case BinaryOp::NE:
            case BinaryOp::LT:
            case BinaryOp::LE:
            case BinaryOp::GT:
            case BinaryOp::GE:
            case BinaryOp::REGEX:
            case BinaryOp::IREGEX:
            case BinaryOp::NREGEX:
            case BinaryOp::NIREGEX:
                return true;
            default:
                return false;
        }
    return std::holds_alternative<In>(e.node) ||
           std::holds_alternative<Between>(e.node) ||
           std::holds_alternative<Like>(e.node) ||
           std::holds_alternative<Is>(e.node);
}

class Expander {
   public:
    Expander(const LeafPaths& leaves, bool fallback, std::string_view text)
        : leaves_(leaves), fallback_(fallback), text_(text) {}

    void run(Pipeline& p) {
        stage_items(p);
        each_slot(p, [&](ExprPtr& e) { expr(e, false); });
    }

   private:
    const LeafPaths& leaves_;
    bool fallback_;
    std::string_view text_;
    std::optional<std::vector<Segments>> cache_;

    [[noreturn]] void fail(Span span, std::string msg) const {
        throw Failure{
            make_error(text_, span.offset, span.length, std::move(msg))};
    }

    std::vector<Path> matches(const Path& pattern, Span span) {
        if (!cache_) {
            cache_.emplace();
            for (const auto& l : leaves_()) cache_->push_back(split(l));
        }
        const Segments want = segments_of(pattern);
        const bool bare = fallback_ && want.front() != "args";
        std::set<Segments, decltype(&path_less)> found(&path_less);
        const auto fits = [&](const Segments& s, std::size_t from) {
            if (s.size() - from != want.size()) return false;
            for (std::size_t i = 0; i < want.size(); ++i)
                if (want[i] != "*" && want[i] != s[from + i]) return false;
            return true;
        };
        for (const auto& s : *cache_) {
            if (fits(s, 0)) {
                found.insert(s);
            } else if (bare && s.front() == "args" && fits(s, 1)) {
                found.insert(Segments(s.begin() + 1, s.end()));
            }
        }
        if (found.empty())
            fail(span, "no field matches the pattern '" + join(want) + "'");
        std::vector<Path> out;
        for (const auto& s : found) out.push_back(path_of(s));
        return out;
    }

    void paths(std::vector<Path>& list, Span span) {
        std::vector<Path> next;
        for (auto& p : list) {
            if (!has_wildcard(p)) {
                next.push_back(std::move(p));
                continue;
            }
            for (auto& m : matches(p, span)) next.push_back(std::move(m));
        }
        list = std::move(next);
    }

    void stage_items(Pipeline& p) {
        for (auto& s : p.stages)
            std::visit(
                [&](auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, Select>) {
                        std::vector<Item> next;
                        for (auto& it : n.items) {
                            const auto* path =
                                std::get_if<Path>(&it.value->node);
                            if (!path || !has_wildcard(*path)) {
                                next.push_back(std::move(it));
                                continue;
                            }
                            for (auto& m : matches(*path, it.value->span))
                                next.push_back(
                                    {make(std::move(m), it.value->span), {}});
                        }
                        n.items = std::move(next);
                    } else if constexpr (std::is_same_v<T, Drop> ||
                                         std::is_same_v<T, Unpivot>) {
                        paths(n.paths, s.span);
                    } else if constexpr (std::is_same_v<T, Lookup> ||
                                         std::is_same_v<T, AsofLookup> ||
                                         std::is_same_v<T, OverlapLookup>) {
                        if (n.side) stage_items(*n.side);
                    } else if constexpr (std::is_same_v<T, Union>) {
                        stage_items(*n.other);
                    }
                },
                s.node);
    }

    void expr(ExprPtr& slot, bool compared) {
        if (!slot) return;
        if (quant_pattern(*slot)) {
            if (!compared)
                fail(slot->span,
                     "any(p) and all(p) with a pattern compare with a value, "
                     "as in 'any(p) > v'");
            return;
        }
        if (auto* sub = std::get_if<Subquery>(&slot->node))
            stage_items(*sub->pipeline);
        if (auto* in = std::get_if<In>(&slot->node); in && in->subquery)
            stage_items(*in->subquery);
        const bool cmp = comparison(*slot);
        children(*slot, [&](ExprPtr& c) { expr(c, cmp); });
        if (cmp) rewrite(slot);
    }

    static ExprPtr* quant_child(Expr& e, int& count) {
        ExprPtr* found = nullptr;
        count = 0;
        children(e, [&](ExprPtr& c) {
            if (c && quant_pattern(*c)) {
                found = &c;
                ++count;
            }
        });
        return found;
    }

    void rewrite(ExprPtr& slot) {
        int count = 0;
        ExprPtr* q = quant_child(*slot, count);
        if (!q) return;
        if (count > 1)
            fail(slot->span, "compare a quantified array with a value");
        const Span span = slot->span;
        const Call& call = std::get<Call>((*q)->node);
        const Path& pattern = *quant_pattern(**q);
        const bool all = call.name == "all";
        std::vector<Path> ms = matches(pattern, (*q)->span);
        if (ms.size() == 1) {
            *q = make(std::move(ms.front()), span);
            return;
        }
        const auto part_for = [&](Path m) {
            ExprPtr part = clone(*slot);
            int n = 0;
            *quant_child(*part, n) = make(std::move(m), span);
            return part;
        };
        ExprPtr acc = part_for(std::move(ms.front()));
        for (std::size_t i = 1; i < ms.size(); ++i)
            acc = make(Binary{all ? BinaryOp::AND : BinaryOp::OR,
                              std::move(acc), part_for(std::move(ms[i]))},
                       span);
        acc->span = span;
        slot = std::move(acc);
    }
};

}  // namespace

dftracer::utils::expected<void, DuqlError> expand_wildcards(
    syntax::Pipeline& p, const LeafPaths& leaves, bool args_fallback,
    std::string_view text) {
    try {
        Expander(leaves, args_fallback, text).run(p);
        return {};
    } catch (const Failure& f) {
        return dftracer::utils::unexpected(f.error);
    }
}

}  // namespace dftracer::utils::duql
