#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/env.h>
#include <dftracer/utils/duql/macros.h>
#include <dftracer/utils/duql/syntax/lexer.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/syntax/walk.h>

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

void set_span(ExprPtr& e, Span span) {
    if (!e) return;
    e->span = span;
    children(*e, [&](ExprPtr& c) { set_span(c, span); });
}

// A copy of `e`: its canonical text parsed again.
ExprPtr clone(const Expr& e) {
    auto tree = syntax::parse("where " + syntax::to_text(e));
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
        check(*call, *m, span);
        const auto* expr_body =
            std::get_if<std::shared_ptr<const Expr>>(&m->body);
        if (!expr_body)
            fail(span,
                 "macro '" + m->name + "' is a pipeline; call it as a stage");
        ExprPtr body = clone(**expr_body);
        bind(body, *m, call->args);
        stack_.push_back(m->name);
        expand(body);
        stack_.pop_back();
        set_span(body, span);
        slot = std::move(body);
    }

    void splice(Pipeline& p, bool leading) {
        for (std::size_t i = 0; i < p.stages.size();) {
            auto* use = std::get_if<Use>(&p.stages[i].node);
            if (!use) {
                ++i;
                continue;
            }
            const Span span = p.stages[i].span;
            const bool first = leading && p.sources.empty() && i == 0;
            const Macro* m = find(use->call.name);
            const auto* body =
                m ? std::get_if<std::shared_ptr<const Pipeline>>(&m->body)
                  : nullptr;
            if (!body) {
                if (!first) {
                    if (m)
                        fail(span, "macro '" + m->name +
                                       "' is an expression; write 'where " +
                                       m->name + "(...)'");
                    fail(span, "Unknown stage or pipeline macro '" +
                                   use->call.name + "'");
                }
                auto e = std::make_unique<Expr>();
                e->node = std::move(use->call);
                e->span = span;
                p.stages[i].node = Where{std::move(e)};
                ++i;
                continue;
            }
            check(use->call, *m, span);
            auto tree = syntax::parse(syntax::to_text(**body));
            if (!tree || !tree->pipeline)
                throw DFTUtilsException(
                    ErrorCode::INVALID_ARGUMENT,
                    "duql: a macro body does not print back: " +
                        syntax::to_text(**body));
            PipelinePtr copy = std::move(tree->pipeline);
            const std::vector<Arg> args = std::move(use->call.args);
            each_slot(*copy, [&](ExprPtr& e) { bind(e, *m, args); });
            stack_.push_back(m->name);
            splice(*copy, first);
            stack_.pop_back();
            each_slot(*copy, [&](ExprPtr& e) { set_span(e, span); });
            for (auto& s : copy->stages) s.span = span;
            const std::size_t n = copy->stages.size();
            p.stages.erase(p.stages.begin() + static_cast<std::ptrdiff_t>(i));
            p.stages.insert(p.stages.begin() + static_cast<std::ptrdiff_t>(i),
                            std::make_move_iterator(copy->stages.begin()),
                            std::make_move_iterator(copy->stages.end()));
            i += n;
        }
        for (auto& s : p.stages)
            std::visit(
                [&](auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, Lookup> ||
                                  std::is_same_v<T, AsofLookup> ||
                                  std::is_same_v<T, OverlapLookup>) {
                        if (n.side) splice(*n.side, true);
                    } else if constexpr (std::is_same_v<T, Union>) {
                        splice(*n.other, true);
                    }
                },
                s.node);
        each_slot(p, [&](ExprPtr& e) { splice_in(e); });
    }

   private:
    void splice_in(ExprPtr& e) {
        if (!e) return;
        if (auto* sq = std::get_if<Subquery>(&e->node)) {
            splice(*sq->pipeline, true);
            return;
        }
        if (auto* in = std::get_if<In>(&e->node); in && in->subquery) {
            splice_in(in->subject);
            for (auto& x : in->list) splice_in(x);
            splice(*in->subquery, true);
            return;
        }
        children(*e, [&](ExprPtr& c) { splice_in(c); });
    }

    void check(const Call& call, const Macro& m, Span span) const {
        if (builtin_(m.name))
            fail(span, "macro '" + m.name +
                           "' has the name of a built-in function; rename it");
        if (call.args.size() != m.params.size())
            fail(span, "macro '" + m.name + "' takes " +
                           std::to_string(m.params.size()) +
                           " argument(s), got " +
                           std::to_string(call.args.size()));
        for (const auto& a : call.args)
            if (!a.name.empty())
                fail(span, "macro '" + m.name + "' takes no named argument '" +
                               a.name + "'");
        if (std::find(stack_.begin(), stack_.end(), m.name) != stack_.end()) {
            std::string cycle;
            for (const auto& n : stack_) cycle += n + " -> ";
            fail(span, "macros call each other: " + cycle + m.name);
        }
    }

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
    const auto env = Env::get("DFTRACER_DUQL_PATH");
    if (!env) return;
    std::string_view rest = *env;
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
        if (def.name == "args_fallback") continue;
        auto* expr = std::get_if<ExprPtr>(&def.body);
        if ((expr && !*expr) || (!expr && !std::get<PipelinePtr>(def.body)))
            continue;
        for (const auto& m : out)
            if (m.name == def.name)
                return dftracer::utils::unexpected(syntax::make_error(
                    text, expr ? (*expr)->span.offset : 0, 0,
                    "macro '" + def.name + "' is defined twice"));
        if (expr)
            out.push_back({def.name, def.params,
                           std::shared_ptr<const Expr>(std::move(*expr)),
                           origin});
        else
            out.push_back({def.name, def.params,
                           std::shared_ptr<const Pipeline>(
                               std::move(std::get<PipelinePtr>(def.body))),
                           origin});
    }
    return out;
}

bool args_fallback(const std::vector<syntax::Def>& defs) {
    for (const auto& def : defs)
        if (const auto* e = std::get_if<ExprPtr>(&def.body);
            def.name == "args_fallback" && def.params.empty() && e && *e)
            if (const auto* lit = std::get_if<Literal>(&(*e)->node))
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
    try {
        Expander x(scopes, text, builtin);
        x.splice(pipeline, true);
        if (!any) return {};
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
