// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/macros.h>
#include <dftracer/utils/duql/syntax/lexer.h>
#include <dftracer/utils/duql/syntax/parser.h>
#include <dftracer/utils/duql/syntax/walk.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <utility>

namespace dftracer::utils::duql {

namespace {

using namespace syntax;

using Status = dstd::Expected<void, DuqlError>;
using Fail = dstd::Unexpected<DuqlError>;

void set_span(ExprPtr& e, Span span) {
    if (!e) return;
    e->span = span;
    children(*e, [&](ExprPtr& c) { set_span(c, span); });
}

// A copy of `e`: its canonical text parsed again.
ExprPtr clone(const Expr& e) {
    auto tree = syntax::parse("where " + syntax::to_text(e));
    if (!tree || !tree->pipeline || tree->pipeline->stages.empty())
        dstd::fatal("duql: a macro body does not print back",
                    syntax::to_text(e).c_str());
    return std::move(
        std::get<Where>(tree->pipeline->stages.front().node).condition);
}

class Expander {
   public:
    Expander(const MacroScopes& scopes, dstd::StringView text,
             const dstd::Function<bool(dstd::StringView)>& builtin)
        : scopes_(scopes), text_(text), builtin_(builtin) {}

    Status expand(ExprPtr& slot) {
        if (!slot) return {};
        Status status;
        children(*slot, [&](ExprPtr& c) {
            if (!status) return;
            status = expand(c);
        });
        if (!status) return status;
        auto* call = std::get_if<Call>(&slot->node);
        if (!call) return {};
        const Macro* m = find(call->name);
        if (!m) return {};
        const Span span = slot->span;
        auto checked = check(*call, *m, span);
        if (!checked) return checked;
        const auto* expr_body =
            std::get_if<dstd::SharedPtr<const Expr>>(&m->body);
        if (!expr_body)
            return fail(span, "macro '" + m->name +
                                  "' is a pipeline; call it as a stage");
        ExprPtr body = clone(**expr_body);
        bind(body, *m, call->args);
        stack_.push_back(m->name);
        auto expanded = expand(body);
        stack_.pop_back();
        if (!expanded) return expanded;
        set_span(body, span);
        slot = std::move(body);
        return {};
    }

    Status splice(Pipeline& p, bool leading) {
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
                m ? std::get_if<dstd::SharedPtr<const Pipeline>>(&m->body)
                  : nullptr;
            if (!body) {
                if (!first) {
                    if (m)
                        return fail(span,
                                    "macro '" + m->name +
                                        "' is an expression; write 'where " +
                                        m->name + "(...)'");
                    return fail(span, "Unknown stage or pipeline macro '" +
                                          use->call.name + "'");
                }
                auto e = dstd::make_unique<Expr>();
                e->node = std::move(use->call);
                e->span = span;
                p.stages[i].node = Where{std::move(e)};
                ++i;
                continue;
            }
            auto checked = check(use->call, *m, span);
            if (!checked) return checked;
            auto tree = syntax::parse(syntax::to_text(**body));
            if (!tree || !tree->pipeline)
                dstd::fatal("duql: a macro body does not print back",
                            syntax::to_text(**body).c_str());
            PipelinePtr copy = std::move(tree->pipeline);
            const dstd::Vector<Arg> args = std::move(use->call.args);
            each_slot(*copy, [&](ExprPtr& e) { bind(e, *m, args); });
            stack_.push_back(m->name);
            auto spliced = splice(*copy, first);
            stack_.pop_back();
            if (!spliced) return spliced;
            each_slot(*copy, [&](ExprPtr& e) { set_span(e, span); });
            for (auto& s : copy->stages) s.span = span;
            const std::size_t n = copy->stages.size();
            p.stages.erase(p.stages.begin() + static_cast<std::ptrdiff_t>(i));
            p.stages.insert(p.stages.begin() + static_cast<std::ptrdiff_t>(i),
                            std::make_move_iterator(copy->stages.begin()),
                            std::make_move_iterator(copy->stages.end()));
            i += n;
        }
        for (auto& s : p.stages) {
            Status done = std::visit(
                [&](auto& n) -> Status {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, Lookup> ||
                                  std::is_same_v<T, AsofLookup> ||
                                  std::is_same_v<T, OverlapLookup>) {
                        if (n.side) return splice(*n.side, true);
                    } else if constexpr (std::is_same_v<T, Union>) {
                        return splice(*n.other, true);
                    }
                    return {};
                },
                s.node);
            if (!done) return done;
        }
        Status status;
        each_slot(p, [&](ExprPtr& e) {
            if (!status) return;
            status = splice_in(e);
        });
        return status;
    }

   private:
    Status splice_in(ExprPtr& e) {
        if (!e) return {};
        if (auto* sq = std::get_if<Subquery>(&e->node))
            return splice(*sq->pipeline, true);
        if (auto* in = std::get_if<In>(&e->node); in && in->subquery) {
            auto subject = splice_in(in->subject);
            if (!subject) return subject;
            for (auto& x : in->list) {
                auto item = splice_in(x);
                if (!item) return item;
            }
            return splice(*in->subquery, true);
        }
        Status status;
        children(*e, [&](ExprPtr& c) {
            if (!status) return;
            status = splice_in(c);
        });
        return status;
    }

    Status check(const Call& call, const Macro& m, Span span) const {
        if (builtin_(m.name))
            return fail(span,
                        "macro '" + m.name +
                            "' has the name of a built-in function; rename it");
        if (call.args.size() != m.params.size())
            return fail(span, "macro '" + m.name + "' takes " +
                                  dstd::int_text(m.params.size()) +
                                  " argument(s), got " +
                                  dstd::int_text(call.args.size()));
        for (const auto& a : call.args)
            if (!a.name.empty())
                return fail(span, "macro '" + m.name +
                                      "' takes no named argument '" + a.name +
                                      "'");
        if (std::find(stack_.begin(), stack_.end(), m.name) != stack_.end()) {
            dstd::String cycle;
            for (const auto& n : stack_) cycle += n + " -> ";
            return fail(span, "macros call each other: " + cycle + m.name);
        }
        return {};
    }

    const MacroScopes& scopes_;
    dstd::StringView text_;
    const dstd::Function<bool(dstd::StringView)>& builtin_;
    dstd::Vector<dstd::String> stack_;

    Fail fail(Span span, dstd::String msg) const {
        return Fail(syntax::make_error(text_, span.offset, span.length,
                                       std::move(msg)));
    }

    const Macro* find(dstd::StringView name) const {
        for (const auto& scope : scopes_)
            for (const auto& m : scope)
                if (m.name == name) return &m;
        return nullptr;
    }

    // Replaces each bare parameter name in `e` with a copy of its argument.
    static void bind(ExprPtr& e, const Macro& m,
                     const dstd::Vector<Arg>& args) {
        if (!e) return;
        if (const auto* p = std::get_if<Path>(&e->node);
            p && p->root == PathRoot::Record && p->steps.size() == 1 &&
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
    dstd::Set<dstd::String> loaded;
    dstd::Vector<Macro> macros;
};

PathRegistry& registry() {
    PathRegistry& r = dstd::Singleton<PathRegistry>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, []() noexcept {
            PathRegistry& held = dstd::Singleton<PathRegistry>::get();
            dstd::Set<dstd::String> dropped_loaded;
            dstd::Vector<Macro> dropped_macros;
            std::lock_guard<std::mutex> lock(held.mu);
            dropped_loaded.swap(held.loaded);
            dropped_macros.swap(held.macros);
            held.env_loaded = false;
        });
        return true;
    }();
    return r;
}

dstd::Result<void> load_file(PathRegistry& r, dstd::PathView file) {
    const dstd::String origin(file.str());
    auto loaded = dstd::fs::read_file(file);
    if (!loaded)
        return common::make_error(common::ErrorCode::Io,
                                  "duql: cannot read macro file " + origin);
    const dstd::String text = std::move(*loaded);
    auto tree = syntax::parse(text);
    if (!tree)
        return common::make_error(
            common::ErrorCode::InvalidArgument,
            "duql macro file " + origin + ": " + tree.error().format());
    if (tree->pipeline)
        return common::make_error(
            common::ErrorCode::InvalidArgument,
            "duql macro file " + origin +
                " holds a pipeline; it may hold only 'def' declarations");
    for (const auto& d : tree->decls)
        if (!std::holds_alternative<Def>(d))
            return common::make_error(common::ErrorCode::InvalidArgument,
                                      "duql macro file " + origin +
                                          " may hold only 'def' declarations");
    dstd::Vector<Def> defs = take_defs(tree->decls);
    auto macros = macros_of(defs, text, origin);
    if (!macros)
        return common::make_error(
            common::ErrorCode::InvalidArgument,
            "duql macro file " + origin + ": " + macros.error().format());
    for (auto& m : *macros) {
        for (const auto& old : r.macros)
            if (old.name == m.name)
                return common::make_error(common::ErrorCode::InvalidArgument,
                                          "duql macro '" + m.name +
                                              "' is defined in " + old.origin +
                                              " and " + origin);
        r.macros.push_back(std::move(m));
    }
    return {};
}

dstd::Result<void> load_locked(PathRegistry& r, dstd::StringView path_text) {
    const dstd::PathView path(path_text);
    auto canonical = dstd::fs::weakly_canonical(path);
    if (!canonical) return dstd::Unexpected(canonical.error());
    if (!r.loaded.insert(dstd::String(canonical->str())).second) return {};
    auto is_dir = dstd::fs::is_directory(path);
    if (!is_dir.has_value()) return dstd::Unexpected(is_dir.error());
    if (!*is_dir) return load_file(r, path);
    dstd::Vector<dstd::Path> files;
    auto dir = dstd::fs::Dir::open(path);
    if (!dir) return dstd::Unexpected(dir.error());
    auto listing = dir->entries();
    if (!listing) return dstd::Unexpected(listing.error());
    for (const dstd::fs::DirEntry& e : *listing) {
        if (dstd::PathView(e.name).extension() != ".duql") continue;
        const auto st = dir->stat(e.name);
        if (st && st->kind == dstd::fs::FileKind::File) {
            files.push_back(path / e.name);
        }
    }
    std::sort(files.begin(), files.end(),
              [](const dstd::Path& a, const dstd::Path& b) {
                  return a.str() < b.str();
              });
    for (const auto& f : files) {
        auto loaded = load_file(r, f.view());
        if (!loaded) return loaded;
    }
    return {};
}

dstd::Result<void> load_env(PathRegistry& r) {
    if (r.env_loaded) return {};
    r.env_loaded = true;
    const auto env = common::Env::get("DFTRACER_UTILS_DUQL_PATH");
    if (!env) return {};
    dstd::StringView rest = *env;
    while (!rest.empty()) {
        const std::size_t colon = rest.find(':');
        const dstd::StringView part = rest.substr(0, colon);
        if (!part.empty()) {
            auto loaded = load_locked(r, part);
            if (!loaded) return loaded;
        }
        if (colon == dstd::StringView::npos) break;
        rest.remove_prefix(colon + 1);
    }
    return {};
}

}  // namespace

dftracer::utils::dstd::Expected<dstd::Vector<Macro>, DuqlError> macros_of(
    dstd::Vector<syntax::Def>& defs, dstd::StringView text,
    const dstd::String& origin) {
    dstd::Vector<Macro> out;
    for (auto& def : defs) {
        if (def.name == "args_fallback") continue;
        auto* expr = std::get_if<ExprPtr>(&def.body);
        if ((expr && !*expr) || (!expr && !std::get<PipelinePtr>(def.body)))
            continue;
        for (const auto& m : out)
            if (m.name == def.name)
                return dftracer::utils::dstd::Unexpected(syntax::make_error(
                    text, expr ? (*expr)->span.offset : 0, 0,
                    "macro '" + def.name + "' is defined twice"));
        if (expr)
            out.push_back({def.name, def.params,
                           dstd::to_shared_ptr(std::move(*expr)), origin});
        else
            out.push_back({def.name, def.params,
                           dstd::to_shared_ptr(
                               std::move(std::get<PipelinePtr>(def.body))),
                           origin});
    }
    return out;
}

bool args_fallback(const dstd::Vector<syntax::Def>& defs) {
    for (const auto& def : defs)
        if (const auto* e = std::get_if<ExprPtr>(&def.body);
            def.name == "args_fallback" && def.params.empty() && e && *e)
            if (const auto* lit = std::get_if<Literal>(&(*e)->node))
                if (const auto* b = std::get_if<bool>(&lit->value)) return *b;
    return false;
}

dstd::Vector<syntax::Def> take_defs(dstd::Vector<syntax::Decl>& decls) {
    dstd::Vector<Def> out;
    dstd::Vector<Decl> rest;
    for (auto& d : decls) {
        if (auto* def = std::get_if<Def>(&d))
            out.push_back(std::move(*def));
        else
            rest.push_back(std::move(d));
    }
    decls = std::move(rest);
    return out;
}

dftracer::utils::dstd::Expected<void, DuqlError> expand_macros(
    syntax::Pipeline& pipeline, const MacroScopes& scopes,
    dstd::StringView text,
    const dstd::Function<bool(dstd::StringView)>& builtin) {
    bool any = false;
    for (const auto& s : scopes) any = any || !s.empty();
    Expander x(scopes, text, builtin);
    auto spliced = x.splice(pipeline, true);
    if (!spliced) return spliced;
    if (!any) return {};
    Status status;
    each_slot(pipeline, [&](ExprPtr& e) {
        if (!status) return;
        status = x.expand(e);
    });
    return status;
}

dstd::Result<dstd::Vector<Macro>> path_macros() {
    auto& r = registry();
    std::lock_guard lock(r.mu);
    auto loaded = load_env(r);
    if (!loaded) return dstd::Unexpected(loaded.error());
    return r.macros;
}

dstd::Result<void> load_macros(dstd::StringView path) {
    auto& r = registry();
    std::lock_guard lock(r.mu);
    auto loaded = load_env(r);
    if (!loaded) return loaded;
    return load_locked(r, path);
}

}  // namespace dftracer::utils::duql
