#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/build_host.h>
#include <dftracer/utils/plugins/config.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/plugins/plugins_internal.h>
#include <dftracer/utils/plugins/reserved_names.h>
#include <dftracer/utils/plugins/state_registry.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dlfcn.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dftracer::utils::plugins {

namespace {
using View = trace::views::View;
using ExportStats = trace::views::ExportStats;
using Fold = trace::views::detail::Fold;
using Query = query::Query;
}  // namespace

struct Plugins::Impl {
    struct Loaded {
        void* handle = nullptr;
        dftu_plugin* plugin = nullptr;
        bool owned = true; /* injected test plugins are not owned */
        std::string name;  /* load-path stem, used to tag the plugin's logs */
        std::string path;  /* as given to Builder::add; empty for injected test
                              plugins */
        /* State types the factory registered; one instance per fold slice. */
        StateRegistry states;
        /* Op names the factory registered via dftu_op_register; the op
           registry outlives the plugin, so these must be unregistered before
           dlclose or a later lookup strcmps a name in unmapped memory. */
        std::vector<std::string> registered_ops;
        /* Provider names the factory registered via
           dftu_svc_providers::register_provider; the provider registry
           outlives the plugin, so these must be unregistered before dlclose or
           a later LazyFrame::scan calls into unmapped memory. */
        std::vector<std::string> registered_providers;
    };

    ~Impl() {
        // Unregister every op/provider this plugin added and run destroy(),
        // both before dlclose unmaps the plugin's code: a later dftu_op_find,
        // dftu_lazyframe_from_provider or destroy() may otherwise dereference
        // memory that dlclose just unmapped.
        for (auto& p : plugins) {
            for (const std::string& name : p.registered_ops)
                ::dftu_op_unregister(name.c_str());
            for (const std::string& name : p.registered_providers)
                ::dftu_provider_unregister(name.c_str());
            if (p.owned && p.plugin && p.plugin->destroy)
                p.plugin->destroy(p.plugin->self);
            if (p.handle) dlclose(p.handle);
        }
    }

    std::vector<Loaded> plugins;
    // deque pins each ConfigTree so factory-held roots stay valid until
    // teardown.
    std::deque<ConfigTree> configs;
    // Providers before consumers; a full permutation of plugin indices.
    std::vector<std::size_t> order;
    // The union prune, settled once at build. run() applies it directly;
    // attach() only offers it to the session (see Plugins::attach()).
    std::optional<Query> prune;
};

struct Plugins::Builder::State {
    struct Pending {
        std::string path;
        ConfigTree config;
        bool has_config = false;
        // Parsed at build() rather than add(), so a malformed literal fails
        // through the Result build() already returns instead of throwing out
        // of add().
        std::optional<std::string> config_json;
    };
    std::vector<Pending> pending;
};

/// Private-member seam for plugins_internal.h; keeps the C ABI out of
/// plugins.h.
struct PluginsInternalAccess {
    static Plugins make(std::unique_ptr<Plugins::Impl> impl) {
        return Plugins(std::move(impl));
    }
    static const Plugins::Impl& impl(const Plugins& set) { return *set.impl_; }
};

namespace {

// The path's file stem (drop directory and the final extension), used to tag
// the plugin's log lines. "/x/y/name_edges.so" -> "name_edges".
std::string plugin_name_from_path(std::string_view path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string_view base =
        slash == std::string_view::npos ? path : path.substr(slash + 1);
    std::size_t dot = base.find_last_of('.');
    if (dot != std::string_view::npos && dot != 0) base = base.substr(0, dot);
    return std::string(base);
}

const char* value_kind_name(std::int32_t kind) {
    switch (static_cast<dftu_value_kind>(kind)) {
        case DFTU_VAL_NULL:
            return "null";
        case DFTU_VAL_BOOL:
            return "bool";
        case DFTU_VAL_I64:
            return "int";
        case DFTU_VAL_F64:
            return "float";
        case DFTU_VAL_STR:
            return "string";
        case DFTU_VAL_ARRAY:
            return "array";
        case DFTU_VAL_OBJECT:
            return "object";
    }
    return "?";
}

const char* op_kind_name(dftu_op_sig sig) {
    switch (dftu_op_kind_of(sig)) {
        case DFTU_OP_KIND_SERIES:
            return "series";
        case DFTU_OP_KIND_AGGREGATE:
            return "aggregate";
        case DFTU_OP_KIND_FRAME:
            return "frame";
        case DFTU_OP_KIND_LAZY:
            return "lazy";
    }
    return "?";
}

// `name(arg, ...) -> ret [kind]` for a registered op, or the bare name when it
// is not in the registry (it cannot be, while its plugin is loaded, but a
// describe() that silently dropped an op would be worse than one that says
// less about it).
std::string op_summary(const std::string& name) {
    const dftu_op_desc* desc = ::dftu_op_find(name.c_str());
    if (!desc) return name;
    // dftu_op_signature returns a thread_local buffer reused by the next call.
    std::string out = name + ::dftu_op_signature(desc->sig);
    out += " [";
    out += op_kind_name(desc->sig);
    out += "]";
    return out;
}

bool is_numeric(std::int32_t kind) {
    return kind == DFTU_VAL_BOOL || kind == DFTU_VAL_I64 ||
           kind == DFTU_VAL_F64;
}

// The declared kind is a contract about what the plugin will READ, and it
// reads through dftu_as_i64/f64/bool, which coerce freely between the three
// scalar kinds. So a numeric declaration accepts any numeric value; every
// other kind must match exactly.
bool kind_accepts(std::int32_t declared, std::int32_t given) {
    if (declared == DFTU_CONFIG_ANY) return true;
    if (is_numeric(declared)) return is_numeric(given);
    return declared == given;
}

// Empty when `config` satisfies what `plugin` declares, else the one reason to
// reject the load. A plugin that declares no keys is not validated: an
// undeclared config is the plugin's own business.
std::string config_violation(const dftu_plugin* plugin,
                             const dftu_value* config) {
    if (!plugin->config_keys) return {};
    const dftu_config_key* keys = plugin->config_keys(plugin->self);
    if (!keys) return {};

    if (config && config->kind != DFTU_VAL_OBJECT)
        return "expected a JSON object";

    for (const dftu_config_key* k = keys; k->name; ++k) {
        const dftu_value* v = config ? dftu_obj_get(config, k->name) : nullptr;
        if (!v) {
            if (k->required)
                return std::string("required key '") + k->name + "' is missing";
            continue;
        }
        if (!kind_accepts(k->kind, v->kind))
            return std::string("key '") + k->name + "' must be " +
                   value_kind_name(k->kind) + ", got " +
                   value_kind_name(v->kind);
    }

    if (!config) return {};
    for (std::uint32_t i = 0; i < config->count; ++i) {
        const std::string_view given{config->as.members[i].key,
                                     config->as.members[i].key_len};
        bool declared = false;
        for (const dftu_config_key* k = keys; k->name && !declared; ++k)
            declared = given == k->name;
        if (!declared)
            return "unknown key '" + std::string(given) +
                   "'; the plugin declares its keys, so this is a typo or a "
                   "key it does not read";
    }
    return {};
}

// ConfigTree::from_json_string throws; convert that into the plugin set's
// Result so a malformed literal fails at build() alongside every other
// plugin load error, not by throwing out of Builder::add.
Result<ConfigTree> parse_config_json(const std::string& path,
                                     const std::string& json) {
    try {
        return ConfigTree::from_json_string(json);
    } catch (const std::exception& e) {
        return make_error(
            ErrorCode::PARSE,
            "plugin '" + path + "' config does not parse: " + e.what());
    }
}

// A plugin built against the dataframe C ABI (a jit plugin builds columns
// with dftu_series_new_flat and frames with dftu_dataframe_new) resolves
// those symbols at load. On macOS a two-level-namespace lookup finds them in
// the loaded dylib; on Linux a library a Python extension pulled in sits in
// its local scope, so the plugin's RTLD_NOW dlopen would fail on the first
// dftu_* symbol. Promote the library that holds the ABI to the global scope
// once (RTLD_NOLOAD | RTLD_GLOBAL re-opens an already loaded library without
// loading it again); no plugin links the host, as section 17 requires.
void expose_dataframe_abi() {
    static bool done = false;
    if (done) return;
    done = true;
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&dftu_dataframe_new), &info) ==
            0 ||
        !info.dli_fname)
        return;
    if (void* h = dlopen(info.dli_fname, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD))
        (void)h;  // the handle stays open: the library was loaded already
}

Result<Plugins::Impl::Loaded> load_plugin(const std::string& path,
                                          const dftu_value* config) {
    expose_dataframe_abi();
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* why = dlerror();
        return make_error(ErrorCode::IO,
                          "plugin '" + path + "' failed to load: " +
                              (why ? why : "unknown dlopen error"));
    }

    auto factory = reinterpret_cast<dftu_plugin_factory>(
        dlsym(handle, DFTRACER_PLUGIN_FACTORY_SYMBOL));
    if (!factory) {
        dlclose(handle);
        return make_error(ErrorCode::NOT_FOUND,
                          "plugin '" + path + "' exports no '" +
                              DFTRACER_PLUGIN_FACTORY_SYMBOL + "' symbol");
    }

    BuildHost build_host(plugin_name_from_path(path));
    dftu_plugin* plugin = factory(build_host.host(), config);

    // A rejected load never reaches Loaded, so nothing will later walk
    // registered_ops()/registered_providers() and unregister them: a factory
    // can register either before hitting any of the failures below, so undo
    // that registration here too or it dangles the instant dlclose unmaps the
    // plugin's fn/name.
    auto unregister_all = [&build_host]() {
        for (const std::string& name : build_host.registered_ops())
            ::dftu_op_unregister(name.c_str());
        for (const std::string& name : build_host.registered_providers())
            ::dftu_provider_unregister(name.c_str());
    };

    // A factory that reached outside the registration surface built itself on
    // a host that was not there; whether it noticed the NULL or not, the load
    // fails here rather than at the first batch.
    if (!build_host.denied().empty()) {
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
        unregister_all();
        dlclose(handle);
        return make_error(
            ErrorCode::INVALID_ARGUMENT,
            "plugin '" + path + "' factory asked for '" + build_host.denied() +
                "', which the build-phase host does not provide. A factory may "
                "only register ops, state types and ports; the full host "
                "arrives with the first fold callback");
    }

    if (!plugin) {
        unregister_all();
        dlclose(handle);
        return make_error(ErrorCode::INVALID_ARGUMENT,
                          "plugin '" + path + "' factory returned null");
    }

    if (plugin->abi_version != DFTRACER_PLUGIN_ABI_VERSION) {
        const std::uint32_t got = plugin->abi_version;
        if (plugin->destroy) plugin->destroy(plugin->self);
        unregister_all();
        dlclose(handle);
        return make_error(ErrorCode::INVALID_ARGUMENT,
                          "plugin '" + path + "' ABI version " +
                              std::to_string(got) + " does not match host " +
                              std::to_string(DFTRACER_PLUGIN_ABI_VERSION));
    }

    if (std::string bad = config_violation(plugin, config); !bad.empty()) {
        if (plugin->destroy) plugin->destroy(plugin->self);
        unregister_all();
        dlclose(handle);
        return make_error(ErrorCode::INVALID_ARGUMENT,
                          "plugin '" + path + "' config: " + bad);
    }

    // A plan_query that does not parse used to be logged and dropped, which
    // silently cost two things: plugin_union_prune_query gave up, so the whole
    // SET lost its index prune, and PluginFold ran with no filter, folding the
    // plugin over events its own predicate excluded. The second is a wrong
    // answer, not a lost optimisation, so a broken predicate fails the load.
    if (const char* pq =
            plugin->plan_query ? plugin->plan_query(plugin->self) : nullptr;
        pq != nullptr && *pq != '\0') {
        // Copied before the teardown below: the string belongs to the plugin
        // (the C++ SDK backs it with the holder's `plan` member), so destroy()
        // frees it and dlclose() unmaps the code it came from. Reading it
        // afterwards to build the message is a use-after-free.
        const std::string plan(pq);
        auto parsed = Query::from_string(plan);
        if (!parsed) {
            if (plugin->destroy) plugin->destroy(plugin->self);
            unregister_all();
            dlclose(handle);
            return make_error(
                ErrorCode::INVALID_ARGUMENT,
                "plugin '" + path + "' plan_query '" + plan +
                    "' does not parse: " + parsed.error().message);
        }
    }

    return Plugins::Impl::Loaded{handle,
                                 plugin,
                                 true,
                                 plugin_name_from_path(path),
                                 path,
                                 build_host.take_states(),
                                 build_host.take_registered_ops(),
                                 build_host.take_registered_providers()};
}

// Kahn topological sort of `n` nodes over `from -> to` edges (from must precede
// to). Ready nodes are drained lowest-index first so an independent set keeps
// its registration order. A short return signals a cycle.
std::vector<std::size_t> topo_sort(
    std::size_t n,
    const std::vector<std::pair<std::size_t, std::size_t>>& edges) {
    std::vector<std::vector<std::size_t>> succ(n);
    std::vector<std::size_t> indeg(n, 0);
    for (const auto& [a, b] : edges) {
        succ[a].push_back(b);
        ++indeg[b];
    }
    std::priority_queue<std::size_t, std::vector<std::size_t>,
                        std::greater<std::size_t>>
        ready;
    for (std::size_t i = 0; i < n; ++i)
        if (indeg[i] == 0) ready.push(i);
    std::vector<std::size_t> order;
    order.reserve(n);
    while (!ready.empty()) {
        std::size_t u = ready.top();
        ready.pop();
        order.push_back(u);
        for (std::size_t v : succ[u])
            if (--indeg[v] == 0) ready.push(v);
    }
    return order;
}

// The plugin name a diagnostic should use: the load-path stem, or the index for
// an injected plugin that has none.
std::string plugin_label(const Plugins::Impl& impl, std::size_t i) {
    const std::string& name = impl.plugins[i].name;
    return name.empty() ? ("plugin " + std::to_string(i)) : ("'" + name + "'");
}

std::vector<std::string> name_list(const char* const* (*slot)(void*),
                                   void* self) {
    std::vector<std::string> out;
    if (!slot) return out;
    const char* const* names = slot(self);
    if (!names) return out;
    for (; *names; ++names)
        if (**names) out.emplace_back(*names);
    return out;
}

/// Order the fold so every provider of a name runs before the plugins that
/// consume it, from the plugins' declared provides/consumes.
Result<void> settle_order(Plugins::Impl& impl) {
    for (std::size_t i = 0; i < impl.plugins.size(); ++i) {
        const dftu_plugin* pl = impl.plugins[i].plugin;
        for (const auto* slot : {&pl->provides, &pl->consumes})
            for (const std::string& name : name_list(*slot, pl->self))
                if (is_host_namespace(name))
                    return make_error(
                        ErrorCode::INVALID_ARGUMENT,
                        "plugin " + plugin_label(impl, i) + " declares '" +
                            name +
                            "'; the 'dftu.' namespace belongs to the host and "
                            "is refused to plugins");
    }

    std::unordered_map<std::string, std::size_t> producer;
    for (std::size_t i = 0; i < impl.plugins.size(); ++i) {
        const dftu_plugin* pl = impl.plugins[i].plugin;
        for (const std::string& name : name_list(pl->provides, pl->self)) {
            auto [it, fresh] = producer.emplace(name, i);
            if (!fresh)
                return make_error(
                    ErrorCode::INVALID_ARGUMENT,
                    "plugins " + plugin_label(impl, it->second) + " and " +
                        plugin_label(impl, i) + " both provide '" + name +
                        "'. Names are lowercased, so two identifiers that "
                        "differ only in case collide here");
        }
    }

    std::vector<std::pair<std::size_t, std::size_t>> edges;
    for (std::size_t i = 0; i < impl.plugins.size(); ++i) {
        const dftu_plugin* pl = impl.plugins[i].plugin;
        for (const std::string& name : name_list(pl->consumes, pl->self)) {
            auto it = producer.find(name);
            if (it == producer.end())
                return make_error(ErrorCode::NOT_FOUND,
                                  "plugin " + plugin_label(impl, i) +
                                      " consumes '" + name +
                                      "' which no loaded plugin provides");
            if (it->second != i) edges.emplace_back(it->second, i);
        }
    }

    impl.order = topo_sort(impl.plugins.size(), edges);
    if (impl.order.size() != impl.plugins.size()) {
        std::vector<bool> sorted(impl.plugins.size(), false);
        for (std::size_t i : impl.order) sorted[i] = true;
        std::size_t stuck = 0;
        while (stuck < sorted.size() && sorted[stuck]) ++stuck;
        impl.order.clear();
        return make_error(
            ErrorCode::INVALID_ARGUMENT,
            "plugin provides/consumes graph has a cycle through " +
                plugin_label(impl, stuck));
    }
    return {};
}

void settle_prune(Plugins::Impl& impl) {
    std::vector<const char*> plan_queries;
    plan_queries.reserve(impl.plugins.size());
    for (const auto& p : impl.plugins)
        plan_queries.push_back(p.plugin->plan_query
                                   ? p.plugin->plan_query(p.plugin->self)
                                   : nullptr);
    impl.prune = detail::plugin_union_prune_query(plan_queries);
}

}  // namespace

namespace detail {

std::optional<Query> plugin_union_prune_query(
    const std::vector<const char*>& plan_queries) {
    namespace q = query;
    if (plan_queries.empty()) return std::nullopt;
    std::vector<std::string> distinct;
    for (const char* s : plan_queries) {
        if (!s || !*s) return std::nullopt;
        std::string_view sv{s};
        if (!q::try_parse(sv)) {
            DFTRACER_UTILS_LOG_DEBUG(
                "Plugin plan_query '%s' failed to parse; scanning all", s);
            return std::nullopt;
        }
        if (std::find(distinct.begin(), distinct.end(), sv) == distinct.end())
            distinct.emplace_back(sv);
    }
    if (distinct.size() == 1) return q::try_parse(distinct.front());
    std::string uni;
    for (const auto& d : distinct) {
        if (!uni.empty()) uni += " or ";
        uni += '(';
        uni += d;
        uni += ')';
    }
    return q::try_parse(uni);
}

}  // namespace detail

Plugins::Builder::Builder() : state_(std::make_unique<State>()) {}
Plugins::Builder::~Builder() = default;
Plugins::Builder::Builder(Builder&&) noexcept = default;
Plugins::Builder& Plugins::Builder::operator=(Builder&&) noexcept = default;

Plugins::Builder& Plugins::Builder::add(std::string path, ConfigTree config) {
    state_->pending.push_back(
        {std::move(path), std::move(config), true, std::nullopt});
    return *this;
}

Plugins::Builder& Plugins::Builder::add(std::string path,
                                        std::string config_json) {
    State::Pending pending;
    pending.path = std::move(path);
    pending.config_json = std::move(config_json);
    state_->pending.push_back(std::move(pending));
    return *this;
}

Plugins::Builder& Plugins::Builder::add(std::string path) {
    state_->pending.push_back(
        {std::move(path), ConfigTree{}, false, std::nullopt});
    return *this;
}

Result<Plugins> Plugins::Builder::build() {
    auto impl = std::make_unique<Impl>();
    impl->plugins.reserve(state_->pending.size());
    for (auto& pending : state_->pending) {
        const dftu_value* root = nullptr;
        if (pending.config_json) {
            auto parsed = parse_config_json(pending.path, *pending.config_json);
            if (!parsed) return unexpected(std::move(parsed).error());
            impl->configs.push_back(std::move(*parsed));
            root = impl->configs.back().root();
        } else if (pending.has_config) {
            impl->configs.push_back(std::move(pending.config));
            root = impl->configs.back().root();
        }
        auto loaded = load_plugin(pending.path, root);
        if (!loaded) return unexpected(std::move(loaded).error());
        impl->plugins.push_back(std::move(*loaded));
    }
    auto ordered = settle_order(*impl);
    if (!ordered) return unexpected(std::move(ordered).error());
    settle_prune(*impl);
    return PluginsInternalAccess::make(std::move(impl));
}

Plugins::Builder Plugins::builder() { return Builder{}; }

Plugins::Plugins(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Plugins::Plugins(Plugins&&) noexcept = default;
Plugins& Plugins::operator=(Plugins&&) noexcept = default;
Plugins::~Plugins() = default;

std::size_t Plugins::size() const { return impl_->plugins.size(); }

std::vector<Plugins::PluginInfo> Plugins::describe() const {
    std::vector<PluginInfo> out;
    out.reserve(impl_->plugins.size());
    for (const auto& p : impl_->plugins) {
        PluginInfo info;
        info.path = p.path;
        info.abi_version = p.plugin->abi_version;
        info.has_plan_query = p.plugin->plan_query != nullptr;
        info.provides = name_list(p.plugin->provides, p.plugin->self);
        info.consumes = name_list(p.plugin->consumes, p.plugin->self);
        info.reads = name_list(p.plugin->reads, p.plugin->self);
        for (const std::string& op : p.registered_ops)
            info.ops.push_back(op_summary(op));
        if (p.plugin->config_keys) {
            const dftu_config_key* keys = p.plugin->config_keys(p.plugin->self);
            for (; keys && keys->name; ++keys) {
                std::string line = keys->name;
                line += " (";
                line += keys->kind == DFTU_CONFIG_ANY
                            ? "any"
                            : value_kind_name(keys->kind);
                if (keys->required) line += ", required";
                line += ")";
                if (keys->doc && *keys->doc)
                    line += std::string(" - ") + keys->doc;
                info.config_keys.push_back(std::move(line));
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

View Plugins::prune(const View& view) const {
    return impl_->prune ? view.filter(*impl_->prune) : view;
}

coro::CoroTask<Result<PluginRun>> Plugins::run(const View& view) const {
    dftracer::utils::StringIntern intern;
    PluginRun out;
    // One registry for the whole scan; every plugin's finalize publishes its
    // merged accumulators here and later plugins read them by name.
    SharedResultRegistry shared;
    std::vector<std::unique_ptr<PluginFold>> owned;
    owned.reserve(impl_->plugins.size());
    std::vector<Fold*> folds;
    folds.reserve(impl_->plugins.size());
    for (std::size_t i : impl_->order) {
        owned.push_back(std::make_unique<PluginFold>(
            impl_->plugins[i].plugin, intern, &shared, &out.results,
            impl_->plugins[i].name, view.plan().memory_budget,
            &impl_->plugins[i].states));
        folds.push_back(owned.back().get());
    }

    const View pruned = prune(view);
    out.stats = co_await pruned.run_folds(folds, intern);
    co_return out;
}

trace::views::Deferred<PluginRun> Plugins::attach(
    trace::views::ViewSession& session) const {
    namespace views = trace::views;
    // Captured by the factory closures (owned by the session through execute)
    // so it outlives the scan.
    auto shared = std::make_shared<SharedResultRegistry>();
    auto out = std::make_shared<PluginRun>();
    auto executed = std::make_shared<bool>(false);
    NamedResultRegistry* named = &out->results;
    const std::size_t n = impl_->order.size();
    if (n == 0) {
        *executed = true;
        return {out, executed};
    }
    // Offer the same union prune run() applies. The session drops it if any
    // other branch joins, since the scan they share must stay wide enough for
    // all of them; only execute() knows the final branch set.
    if (impl_->prune) session.propose_base_prune(*impl_->prune);
    for (std::size_t idx = 0; idx < n; ++idx) {
        std::size_t i = impl_->order[idx];
        const dftu_plugin* plugin = impl_->plugins[i].plugin;
        std::string name = impl_->plugins[i].name;
        const StateRegistry* states = &impl_->plugins[i].states;
        // A session does not expose its plan's budget here, so an attached
        // plugin's accumulators spill on the auto policy rather than on an
        // explicit session budget.
        session.attach_fold_factory(
            [plugin, shared, named, name,
             states](dftracer::utils::StringIntern& intern)
                -> std::unique_ptr<views::detail::Fold> {
                return std::make_unique<PluginFold>(
                    plugin, intern, shared.get(), named, name, 0, states);
            },
            // The fused scan finalizes every attached fold together, so the
            // last one to finalize marks the handle resolved.
            idx + 1 == n
                ? std::function<void()>([executed]() { *executed = true; })
                : std::function<void()>([]() {}));
    }
    return {out, executed};
}

Result<Plugins> build_injected_plugins(std::vector<dftu_plugin*> plugins) {
    auto impl = std::make_unique<Plugins::Impl>();
    impl->plugins.reserve(plugins.size());
    for (dftu_plugin* pl : plugins)
        impl->plugins.push_back({nullptr, pl, false, {}, {}, {}, {}, {}});
    auto ordered = settle_order(*impl);
    if (!ordered) return unexpected(std::move(ordered).error());
    settle_prune(*impl);
    return PluginsInternalAccess::make(std::move(impl));
}

std::vector<std::size_t> fold_order(const Plugins& set) {
    return PluginsInternalAccess::impl(set).order;
}

}  // namespace dftracer::utils::plugins
