#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/duql/macros.h>
#include <dftracer/utils/duql/syntax/lexer.h>
#include <dftracer/utils/duql/syntax/parser.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

namespace dftracer::utils::duql {

namespace {

using namespace syntax;

struct Failure {
    DuqlError error;
};

// Calls `fn` on every expression slot directly under `e`.
template <class F>
void children(Expr& e, F&& fn);

template <class F>
void each_slot(Pipeline& p, F&& fn) {
    auto items = [&](std::vector<Item>& v) {
        for (auto& it : v) fn(it.value);
    };
    auto assigns = [&](std::vector<Assign>& v) {
        for (auto& a : v) fn(a.value);
    };
    auto sort_keys = [&](std::vector<SortKey>& v) {
        for (auto& k : v) fn(k.value);
    };
    auto exprs = [&](std::vector<ExprPtr>& v) {
        for (auto& x : v) fn(x);
    };
    for (auto& s : p.stages)
        std::visit(
            [&](auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, Where>) {
                    fn(n.condition);
                } else if constexpr (std::is_same_v<T, Derive>) {
                    assigns(n.fields);
                } else if constexpr (std::is_same_v<T, Select>) {
                    items(n.items);
                } else if constexpr (std::is_same_v<T, Distinct>) {
                    exprs(n.keys);
                } else if constexpr (std::is_same_v<T, Group>) {
                    items(n.keys);
                    assigns(n.aggregates);
                } else if constexpr (std::is_same_v<T, Agg>) {
                    assigns(n.aggregates);
                } else if constexpr (std::is_same_v<T, Window>) {
                    items(n.partition);
                    sort_keys(n.order);
                    assigns(n.fields);
                } else if constexpr (std::is_same_v<T, Pivot>) {
                    fn(n.key);
                    exprs(n.values);
                    assigns(n.aggregates);
                } else if constexpr (std::is_same_v<T, Sort>) {
                    sort_keys(n.keys);
                } else if constexpr (std::is_same_v<T, Take>) {
                    exprs(n.by);
                    sort_keys(n.order);
                } else if constexpr (std::is_same_v<T, Lookup>) {
                    for (auto& [k, c] : n.keys) {
                        fn(k);
                        fn(c);
                    }
                } else if constexpr (std::is_same_v<T, OverlapLookup>) {
                    for (auto& [k, c] : n.keys) {
                        fn(k);
                        fn(c);
                    }
                } else if constexpr (std::is_same_v<T, AsofLookup>) {
                    for (auto& [k, c] : n.keys) {
                        fn(k);
                        fn(c);
                    }
                    fn(n.time.first);
                    fn(n.time.second);
                } else if constexpr (std::is_same_v<T, Union>) {
                    each_slot(*n.other, fn);
                } else if constexpr (std::is_same_v<T, CallStage>) {
                    for (auto& a : n.call.args) fn(a.value);
                } else if constexpr (std::is_same_v<T, TimeRange>) {
                    fn(n.low);
                    fn(n.high);
                } else if constexpr (std::is_same_v<T, Bucket>) {
                    fn(n.width);
                }
            },
            s.node);
}

template <class F>
void children(Expr& e, F&& fn) {
    std::visit(
        [&](auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Unary>) {
                fn(n.operand);
            } else if constexpr (std::is_same_v<T, Binary>) {
                fn(n.left);
                fn(n.right);
            } else if constexpr (std::is_same_v<T, In>) {
                fn(n.subject);
                for (auto& x : n.list) fn(x);
                if (n.subquery) each_slot(*n.subquery, fn);
            } else if constexpr (std::is_same_v<T, Between>) {
                fn(n.subject);
                fn(n.low);
                fn(n.high);
            } else if constexpr (std::is_same_v<T, Like> ||
                                 std::is_same_v<T, Is>) {
                fn(n.subject);
            } else if constexpr (std::is_same_v<T, Arrow>) {
                fn(n.key);
            } else if constexpr (std::is_same_v<T, Call>) {
                for (auto& a : n.args) fn(a.value);
            } else if constexpr (std::is_same_v<T, List> ||
                                 std::is_same_v<T, Tuple>) {
                for (auto& x : n.items) fn(x);
            } else if constexpr (std::is_same_v<T, Subquery>) {
                each_slot(*n.pipeline, fn);
            }
        },
        e.node);
}

void set_span(ExprPtr& e, Span span) {
    if (!e) return;
    e->span = span;
    children(*e, [&](ExprPtr& c) { set_span(c, span); });
}

// A copy of `e`: its canonical text parsed again.
ExprPtr clone(const Expr& e) {
    auto tree = syntax::parse(syntax::to_text(e));
    if (!tree || !tree->pipeline || tree->pipeline->stages.empty())
        throw DFTUtilsException(
            ErrorCode::INVALID_ARGUMENT,
            "duql: a macro body does not print back: " + syntax::to_text(e));
    return std::move(
        std::get<Where>(tree->pipeline->stages.front().node).condition);
}

class Expander {
   public:
    Expander(const MacroScopes& scopes, std::string_view text,
             const std::function<bool(std::string_view)>& builtin)
        : scopes_(scopes), text_(text), builtin_(builtin) {}

    void expand(ExprPtr& slot) {
        if (!slot) return;
        children(*slot, [&](ExprPtr& c) { expand(c); });
        auto* call = std::get_if<Call>(&slot->node);
        if (!call) return;
        const Macro* m = find(call->name);
        if (!m) return;
        const Span span = slot->span;
        if (builtin_(m->name))
            fail(span, "macro '" + m->name +
                           "' has the name of a built-in function; rename it");
        if (call->args.size() != m->params.size())
            fail(span, "macro '" + m->name + "' takes " +
                           std::to_string(m->params.size()) +
                           " argument(s), got " +
                           std::to_string(call->args.size()));
        for (const auto& a : call->args)
            if (!a.name.empty())
                fail(span, "macro '" + m->name + "' takes no named argument '" +
                               a.name + "'");
        if (std::find(stack_.begin(), stack_.end(), m->name) != stack_.end()) {
            std::string cycle;
            for (const auto& n : stack_) cycle += n + " -> ";
            fail(span, "macros call each other: " + cycle + m->name);
        }
        ExprPtr body = clone(*m->body);
        bind(body, *m, call->args);
        stack_.push_back(m->name);
        expand(body);
        stack_.pop_back();
        set_span(body, span);
        slot = std::move(body);
    }

   private:
    const MacroScopes& scopes_;
    std::string_view text_;
    const std::function<bool(std::string_view)>& builtin_;
    std::vector<std::string> stack_;

    [[noreturn]] void fail(Span span, std::string msg) const {
        throw Failure{syntax::make_error(text_, span.offset, span.length,
                                         std::move(msg))};
    }

    const Macro* find(std::string_view name) const {
        for (const auto& scope : scopes_)
            for (const auto& m : scope)
                if (m.name == name) return &m;
        return nullptr;
    }

    // Replaces each bare parameter name in `e` with a copy of its argument.
    static void bind(ExprPtr& e, const Macro& m, const std::vector<Arg>& args) {
        if (!e) return;
        if (const auto* p = std::get_if<Path>(&e->node);
            p && p->root == PathRoot::RECORD && p->steps.size() == 1 &&
            !p->steps.front().index) {
            for (std::size_t i = 0; i < m.params.size(); ++i)
                if (m.params[i] == p->steps.front().key) {
                    e = clone(*args[i].value);
                    return;
                }
        }
        children(*e, [&](ExprPtr& c) { bind(c, m, args); });
    }
};

struct PathRegistry {
    std::mutex mu;
    bool env_loaded = false;
    std::set<std::string> loaded;
    std::vector<Macro> macros;
};

PathRegistry& registry() {
    static PathRegistry r;
    return r;
}

void load_file(PathRegistry& r, const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in)
        throw DFTUtilsException(
            ErrorCode::IO, "duql: cannot read macro file " + file.string());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    auto tree = syntax::parse(text);
    if (!tree)
        throw DFTUtilsException(
            ErrorCode::INVALID_ARGUMENT,
            "duql macro file " + file.string() + ": " + tree.error().format());
    if (tree->pipeline)
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "duql macro file " + file.string() +
                                    " holds a pipeline; it may hold only "
                                    "'def' declarations");
    for (const auto& d : tree->decls)
        if (!std::holds_alternative<Def>(d))
            throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                    "duql macro file " + file.string() +
                                        " may hold only 'def' declarations");
    std::vector<Def> defs = take_defs(tree->decls);
    auto macros = macros_of(defs, text, file.string());
    if (!macros)
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "duql macro file " + file.string() + ": " +
                                    macros.error().format());
    for (auto& m : *macros) {
        for (const auto& old : r.macros)
            if (old.name == m.name)
                throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                        "duql macro '" + m.name +
                                            "' is defined in " + old.origin +
                                            " and " + file.string());
        r.macros.push_back(std::move(m));
    }
}

void load_locked(PathRegistry& r, const std::string& path) {
    namespace fs = std::filesystem;
    const std::string key = fs::weakly_canonical(path).string();
    if (!r.loaded.insert(key).second) return;
    if (!fs::is_directory(path)) {
        load_file(r, path);
        return;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(path))
        if (e.is_regular_file() && e.path().extension() == ".duql")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) load_file(r, f);
}

void load_env(PathRegistry& r) {
    if (r.env_loaded) return;
    r.env_loaded = true;
    const char* env = std::getenv("DFTRACER_DUQL_PATH");
    if (!env) return;
    std::string_view rest = env;
    while (!rest.empty()) {
        const std::size_t colon = rest.find(':');
        const std::string part(rest.substr(0, colon));
        if (!part.empty()) load_locked(r, part);
        if (colon == std::string_view::npos) break;
        rest.remove_prefix(colon + 1);
    }
}

}  // namespace

dftracer::utils::expected<std::vector<Macro>, DuqlError> macros_of(
    std::vector<syntax::Def>& defs, std::string_view text,
    const std::string& origin) {
    std::vector<Macro> out;
    for (auto& def : defs) {
        if (def.name == "args_fallback" || !def.body) continue;
        for (const auto& m : out)
            if (m.name == def.name)
                return dftracer::utils::unexpected(syntax::make_error(
                    text, def.body->span.offset, 0,
                    "macro '" + def.name + "' is defined twice"));
        out.push_back({def.name, def.params,
                       std::shared_ptr<const Expr>(std::move(def.body)),
                       origin});
    }
    return out;
}

bool args_fallback(const std::vector<syntax::Def>& defs) {
    for (const auto& def : defs)
        if (def.name == "args_fallback" && def.params.empty() && def.body)
            if (const auto* lit = std::get_if<Literal>(&def.body->node))
                if (const auto* b = std::get_if<bool>(&lit->value)) return *b;
    return false;
}

std::vector<syntax::Def> take_defs(std::vector<syntax::Decl>& decls) {
    std::vector<Def> out;
    std::vector<Decl> rest;
    for (auto& d : decls) {
        if (auto* def = std::get_if<Def>(&d))
            out.push_back(std::move(*def));
        else
            rest.push_back(std::move(d));
    }
    decls = std::move(rest);
    return out;
}

dftracer::utils::expected<void, DuqlError> expand_macros(
    syntax::Pipeline& pipeline, const MacroScopes& scopes,
    std::string_view text,
    const std::function<bool(std::string_view)>& builtin) {
    bool any = false;
    for (const auto& s : scopes) any = any || !s.empty();
    if (!any) return {};
    try {
        Expander x(scopes, text, builtin);
        each_slot(pipeline, [&](ExprPtr& e) { x.expand(e); });
    } catch (const Failure& f) {
        return dftracer::utils::unexpected(f.error);
    }
    return {};
}

std::vector<Macro> path_macros() {
    auto& r = registry();
    std::lock_guard lock(r.mu);
    load_env(r);
    return r.macros;
}

void load_macros(const std::string& path) {
    auto& r = registry();
    std::lock_guard lock(r.mu);
    load_env(r);
    load_locked(r, path);
}

}  // namespace dftracer::utils::duql
