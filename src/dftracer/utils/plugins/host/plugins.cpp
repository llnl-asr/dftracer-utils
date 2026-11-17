// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/file_format_registry.h>
#include <dftracer/utils/dataframe/udaf_registry.h>
#include <dftracer/utils/dataframe/udf_registry.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/queue.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/borrow_release_pin.h>
#include <dftracer/utils/plugins/host/build_host.h>
#include <dftracer/utils/plugins/host/config.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/plugin_abi.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/plugins/host/plugins_internal.h>
#include <dftracer/utils/plugins/host/port_hub.h>
#include <dftracer/utils/plugins/host/reserved_names.h>
#include <dftracer/utils/plugins/host/state_registry.h>
#include <dftracer/utils/trace/index/extensions/extension_registry.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dlfcn.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

namespace dftracer::utils::plugins::host {

namespace {
using View = trace::views::View;
using ExportStats = trace::views::ExportStats;
using Fold = trace::views::detail::Fold;
using Query = duql::Query;
}  // namespace

struct Plugins::Impl {
    struct Loaded {
        /* The plugin library. Every node, provider, index extension and
           cursor of it holds a reference too, so dlclose waits for the last
           of them; null for an injected test plugin. */
        dstd::SharedPtr<PluginLibrary> library;
        dftu_plugin* plugin = nullptr;
        bool owned = true; /* injected test plugins are not owned */
        dstd::String name; /* load-path stem, used to tag the plugin's logs */
        dstd::String path; /* as given to Builder::add; empty for injected test
                              plugins */
        /* State types the factory registered; one instance per fold slice. */
        StateRegistry states;
        /* Op names the factory registered via dftu_op_register; the op
           registry outlives the plugin, so these must be unregistered before
           dlclose or a later lookup strcmps a name in unmapped memory. */
        dstd::Vector<dstd::String> registered_ops;
        /* UDF names registered via dftu_svc_ops::register_udf; same reason as
           the ops. */
        dstd::Vector<dstd::String> registered_udfs;
        /* UDAF names registered via dftu_svc_ops::register_udaf; same reason
           as the ops. */
        dstd::Vector<dstd::String> registered_udafs;
        /* Provider names the factory registered via
           dftu_svc_providers::register_provider; the provider registry
           outlives the plugin, so these must be unregistered before dlclose or
           a later LazyFrame::scan calls into unmapped memory. */
        dstd::Vector<dstd::String> registered_providers;
        /* Node names registered via dftu_svc_nodes::register_node; same
           reason as the providers. */
        dstd::Vector<dstd::String> registered_nodes;
        /* File format names registered via
           dftu_svc_file_formats::register_format; same reason as the
           providers. */
        dstd::Vector<dstd::String> registered_file_formats;
        /* Sink names registered via dftu_svc_file_formats::register_sink; same
           reason as the providers. */
        dstd::Vector<dstd::String> registered_sinks;
        /* Index extension names registered via
           dftu_svc_index::register_extension; same reason as the providers. */
        dstd::Vector<dstd::String> registered_index_extensions;
        /* The pool the plugin's blocks and reservations live in, named after
           the plugin. Closed after the plugin's destroy ran, which releases
           and reports any reservation the plugin left. */
        dstd::SharedPtr<PluginMemory> memory;
        /* The name and version the plugin declares in its dftu_plugin_info;
           name falls back to the load-path stem when it declares none. */
        dstd::String declared_name;
        dstd::String version;
        /* The channel ends and phase results the factory declared. */
        PortDecls ports;
    };

    ~Impl() {
        // Unregister every op/provider this plugin added, then drop the
        // library reference, which runs the plugin's destroy() and closes the
        // library once nothing it made is alive: a later dftu_op_find or
        // dftu_lazyframe_from_provider finds nothing, and what a plan or
        // cursor still holds keeps the library mapped until it is gone.
        for (auto& p : plugins) {
            for (const dstd::String& name : p.registered_ops)
                ::dftu_op_unregister(name.c_str());
            for (const dstd::String& name : p.registered_udfs)
                dataframe::unregister_udf(name);
            for (const dstd::String& name : p.registered_udafs)
                dataframe::unregister_udaf(name);
            for (const dstd::String& name : p.registered_providers)
                ::dftu_provider_unregister(name.c_str());
            for (const dstd::String& name : p.registered_nodes)
                ::dftu_node_unregister(name.c_str());
            for (const dstd::String& name : p.registered_file_formats)
                ::dftu_format_unregister(name.c_str());
            for (const dstd::String& name : p.registered_sinks)
                dataframe::unregister_sink(name);
            for (const dstd::String& name : p.registered_index_extensions)
                trace::index::extensions::unregister_extension(name);
            p.library.reset();
            p.memory.reset();
        }
    }

    dstd::Vector<Loaded> plugins;
    // deque pins each ConfigTree so factory-held roots stay valid until
    // teardown.
    dstd::Deque<ConfigTree> configs;
    // Providers before consumers; a full permutation of plugin indices.
    dstd::Vector<std::size_t> order;
    // The union prune, settled once at build. run() applies it directly;
    // attach() only offers it to the session (see Plugins::attach()).
    dstd::Optional<Query> prune;
    // The channels and phase results of the settled plan: every owner's
    // declarations, which the hub of each scan is built from.
    dstd::Vector<ChannelDecl> channels;
    dstd::Vector<PhaseDecl> phases;
};

struct Plugins::Builder::State {
    struct Pending {
        dstd::String path;
        ConfigTree config;
        bool has_config = false;
        // Parsed at build() rather than add(), so a malformed literal fails
        // through the Result build() already returns instead of throwing out
        // of add().
        dstd::Optional<dstd::String> config_json;
    };
    dstd::Vector<Pending> pending;
};

/// Private-member seam for plugins_internal.h; keeps the C ABI out of
/// plugins.h.
struct PluginsInternalAccess {
    static Plugins make(dstd::UniquePtr<Plugins::Impl> impl) {
        return Plugins(std::move(impl));
    }
    static const Plugins::Impl& impl(const Plugins& set) { return *set.impl_; }
};

namespace {

// The path's file stem (drop directory and the final extension), used to tag
// the plugin's log lines. "/x/y/name_edges.so" -> "name_edges".
dstd::String plugin_name_from_path(dstd::StringView path) {
    std::size_t slash = path.find_last_of("/\\");
    dstd::StringView base =
        slash == dstd::StringView::npos ? path : path.substr(slash + 1);
    std::size_t dot = base.find_last_of('.');
    if (dot != dstd::StringView::npos && dot != 0) base = base.substr(0, dot);
    return dstd::String(base);
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
dstd::String op_summary(const dstd::String& name) {
    const dftu_op_desc* desc = ::dftu_op_find(name.c_str());
    if (!desc) return name;
    // dftu_op_signature returns a thread_local buffer reused by the next call.
    return dstd::fmt<"{}{} [{}]">(name, ::dftu_op_signature(desc->sig),
                                  op_kind_name(desc->sig));
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
dstd::String config_violation(const dftu_plugin* plugin,
                              const dftu_value* config) {
    if (!plugin->config_keys) return {};
    const dftu_config_key* keys = plugin->config_keys(plugin->self);
    if (!keys) return {};

    if (config && config->kind != DFTU_VAL_OBJECT)
        return "expected a JSON object";

    for (const dftu_config_key* k = keys; k->name; ++k) {
        if (k->name[0] == '\0')
            return "the plugin declares a config key with an empty name";
        const dftu_value* v = config ? dftu_obj_get(config, k->name) : nullptr;
        if (!v) {
            if (k->required)
                return dstd::fmt<"required key '{}' is missing">(k->name);
            continue;
        }
        if (!kind_accepts(k->kind, v->kind))
            return dstd::fmt<"key '{}' must be {}, got {}">(
                k->name, value_kind_name(k->kind), value_kind_name(v->kind));
    }

    if (!config) return {};
    for (std::uint32_t i = 0; i < config->count; ++i) {
        const dstd::StringView given{config->as.members[i].key,
                                     config->as.members[i].key_len};
        bool declared = false;
        for (const dftu_config_key* k = keys; k->name && !declared; ++k)
            declared = given == k->name;
        if (!declared)
            return dstd::fmt<
                "unknown key '{}'; the plugin declares its keys, so this is a "
                "typo or a key it does not read">(given);
    }
    return {};
}

// A malformed literal fails at build() alongside every other plugin load
// error, not in Builder::add.
dstd::Result<ConfigTree> parse_config_json(const dstd::String& path,
                                           const dstd::String& json) {
    auto parsed = ConfigTree::from_json_string(json);
    if (!parsed)
        return common::make_error<"plugin '{}' config does not parse: {}">(
            common::ErrorCode::Parse, path, parsed.error().message());
    return parsed;
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
    // Promotes the loaded library to RTLD_GLOBAL; the handle is never closed.
    dlopen(info.dli_fname, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
}

dstd::Result<Plugins::Impl::Loaded> load_plugin(const dstd::String& path,
                                                const dftu_value* config) {
    expose_dataframe_abi();
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* why = dlerror();
        return common::make_error<"plugin '{}' failed to load: {}">(
            common::ErrorCode::Io, path, why ? why : "unknown dlopen error");
    }
    auto library = dstd::make_shared<PluginLibrary>(handle);

    using InfoFn = const dftu_plugin_info* (*)();
    auto info_fn = reinterpret_cast<InfoFn>(
        dlsym(handle, DFTRACER_UTILS_PLUGIN_INFO_SYMBOL));
    if (!info_fn) {
        return common::make_error<
            "plugin '{}' exports no '{}' symbol; define it with "
            "DFTU_PLUGIN_INFO">(common::ErrorCode::NotFound, path,
                                DFTRACER_UTILS_PLUGIN_INFO_SYMBOL);
    }
    const dftu_plugin_info* info = info_fn();
    if (!info || !plugin_struct_size_ok(
                     PluginStruct::PluginInfo, info->struct_size,
                     static_cast<std::uint32_t>(sizeof(dftu_plugin_info)))) {
        return common::make_error<
            "plugin '{}' has no valid dftu_plugin_info (NULL or struct_size "
            "outside [{}, {}])">(
            common::ErrorCode::InvalidArgument, path,
            plugin_struct_min_size(PluginStruct::PluginInfo),
            static_cast<std::uint32_t>(sizeof(dftu_plugin_info)));
    }
    dftu_plugin_info info_copy{};
    common::copy_struct(&info_copy, sizeof(info_copy), info, info->struct_size,
                        plugin_struct_min_size(PluginStruct::PluginInfo));
    if (!abi_compatible(DFTRACER_UTILS_PLUGIN_ABI_VERSION,
                        info_copy.abi_version)) {
        return common::make_error<
            "plugin '{}' was built against plugin ABI version {}; this host "
            "has version {} (major must match and the plugin minor must be no "
            "greater than the host minor); rebuild it">(
            common::ErrorCode::InvalidArgument, path,
            abi_version_string(info_copy.abi_version),
            abi_version_string(DFTRACER_UTILS_PLUGIN_ABI_VERSION));
    }

    // A column that borrows a buffer of this plugin keeps its release callback
    // mapped until the column dies, which is often after this set is gone.
    Dl_info image{};
    if (dladdr(reinterpret_cast<const void*>(info_fn), &image) != 0)
        track_plugin_library(image.dli_fbase, library);

    auto factory = reinterpret_cast<dftu_plugin_factory>(
        dlsym(handle, DFTRACER_UTILS_PLUGIN_FACTORY_SYMBOL));
    if (!factory) {
        return common::make_error<"plugin '{}' exports no '{}' symbol">(
            common::ErrorCode::NotFound, path,
            DFTRACER_UTILS_PLUGIN_FACTORY_SYMBOL);
    }

    BuildHost build_host(plugin_name_from_path(path), library);
    build_host.set_declared_name(
        dstd::String(info_copy.name ? info_copy.name : ""));
    plugins::AbiContext factory_ctx;
    dftu_plugin* plugin = nullptr;
    const dftu_status built =
        factory(factory_ctx.get(), build_host.host(), config, &plugin);

    // A rejected load never reaches Loaded, so nothing will later walk
    // registered_ops()/registered_providers() and unregister them: a factory
    // can register either before hitting any of the failures below, so undo
    // that registration here too or it dangles the instant dlclose unmaps the
    // plugin's fn/name.
    auto unregister_all = [&build_host]() {
        for (const dstd::String& name : build_host.registered_ops())
            ::dftu_op_unregister(name.c_str());
        for (const dstd::String& name : build_host.registered_udfs())
            dataframe::unregister_udf(name);
        for (const dstd::String& name : build_host.registered_udafs())
            dataframe::unregister_udaf(name);
        for (const dstd::String& name : build_host.registered_providers())
            ::dftu_provider_unregister(name.c_str());
        for (const dstd::String& name : build_host.registered_nodes())
            ::dftu_node_unregister(name.c_str());
        for (const dstd::String& name : build_host.registered_file_formats())
            ::dftu_format_unregister(name.c_str());
        for (const dstd::String& name : build_host.registered_sinks())
            dataframe::unregister_sink(name);
        for (const dstd::String& name :
             build_host.registered_index_extensions())
            trace::index::extensions::unregister_extension(name);
    };

    // A factory that reached outside the registration surface built itself on
    // a host that was not there; whether it noticed the NULL or not, the load
    // fails here rather than at the first batch.
    if (!build_host.denied().empty()) {
        unregister_all();
        return common::make_error<
            "plugin '{}' factory asked for '{}', which the build-phase host "
            "does not provide. A factory may only register ops, state types "
            "and ports; the full host arrives with the first fold callback">(
            common::ErrorCode::InvalidArgument, path, build_host.denied());
    }

    if (built < 0) {
        unregister_all();
        const dstd::Error why = factory_ctx.error(built, "factory");
        return common::make_error<"plugin '{}' factory failed: {}">(
            common::ErrorCode::InvalidArgument, path, why.message());
    }

    if (!plugin || !plugin_struct_size_ok(
                       PluginStruct::Plugin, plugin->struct_size,
                       static_cast<std::uint32_t>(sizeof(dftu_plugin)))) {
        const std::uint32_t got = plugin ? plugin->struct_size : 0;
        unregister_all();
        return common::make_error<
            "plugin '{}' descriptor dftu_plugin struct_size {} is outside "
            "[{}, {}]">(common::ErrorCode::InvalidArgument, path, got,
                        plugin_struct_min_size(PluginStruct::Plugin),
                        static_cast<std::uint32_t>(sizeof(dftu_plugin)));
    }
    auto plugin_copy = dstd::make_unique<dftu_plugin>();
    if (!common::copy_struct(plugin_copy.get(), sizeof(dftu_plugin), plugin,
                             plugin->struct_size,
                             plugin_struct_min_size(PluginStruct::Plugin))) {
        unregister_all();
        return common::make_error<
            "plugin '{}' descriptor dftu_plugin struct_size {} is below {}">(
            common::ErrorCode::InvalidArgument, path, plugin->struct_size,
            plugin_struct_min_size(PluginStruct::Plugin));
    }
    plugin = plugin_copy.get();

    if (dstd::String bad = config_violation(plugin, config); !bad.empty()) {
        unregister_all();
        return common::make_error<"plugin '{}' config: {}">(
            common::ErrorCode::InvalidArgument, path, bad);
    }

    // A plan_duql that does not parse used to be logged and dropped, which
    // silently cost two things: plugin_union_prune_query gave up, so the whole
    // SET lost its index prune, and PluginFold ran with no filter, folding the
    // plugin over events its own predicate excluded. The second is a wrong
    // answer, not a lost optimisation, so a broken predicate fails the load.
    if (const char* pq =
            plugin->plan_duql ? plugin->plan_duql(plugin->self) : nullptr;
        pq != nullptr && *pq != '\0') {
        // Copied before the teardown below: the string belongs to the plugin
        // (the C++ SDK backs it with the holder's `plan` member), so destroy()
        // frees it and dlclose() unmaps the code it came from. Reading it
        // afterwards to build the message is a use-after-free.
        const dstd::String plan(pq);
        auto parsed = Query::from_string(plan);
        if (!parsed) {
            unregister_all();
            return common::make_error<
                "plugin '{}' plan_duql '{}' does not parse: {}">(
                common::ErrorCode::InvalidArgument, path, plan,
                dstd::StringView(parsed.error().message));
        }
    }

    // The library runs the checked copy's destroy once the last object the
    // plugin made is gone. It owns that copy, so the pointer stays valid for
    // as long as the library is mapped. It must adopt only after the size
    // check, and only through the checked copy, so the destructor never reads
    // a field beyond the descriptor's declared size.
    library->adopt(std::move(plugin_copy));

    return Plugins::Impl::Loaded{
        std::move(library),
        plugin,
        true,
        plugin_name_from_path(path),
        path,
        build_host.take_states(),
        build_host.take_registered_ops(),
        build_host.take_registered_udfs(),
        build_host.take_registered_udafs(),
        build_host.take_registered_providers(),
        build_host.take_registered_nodes(),
        build_host.take_registered_file_formats(),
        build_host.take_registered_sinks(),
        build_host.take_registered_index_extensions(),
        build_host.memory(),
        build_host.declared_name(),
        dstd::String(info_copy.version ? info_copy.version : ""),
        build_host.take_ports()};
}

// Kahn topological sort of `n` nodes over `from -> to` edges (from must precede
// to). Ready nodes are drained lowest-index first so an independent set keeps
// its registration order. A short return signals a cycle.
dstd::Vector<std::size_t> topo_sort(
    std::size_t n,
    const dstd::Vector<dstd::Pair<std::size_t, std::size_t>>& edges) {
    dstd::Vector<dstd::Vector<std::size_t>> succ(n);
    dstd::Vector<std::size_t> indeg(n, 0);
    for (const auto& [a, b] : edges) {
        succ[a].push_back(b);
        ++indeg[b];
    }
    dstd::PriorityQueue<std::size_t, std::greater<std::size_t>> ready;
    for (std::size_t i = 0; i < n; ++i)
        if (indeg[i] == 0) ready.push(i);
    dstd::Vector<std::size_t> order;
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
dstd::String plugin_label(const Plugins::Impl& impl, std::size_t i) {
    const dstd::String& name = impl.plugins[i].name;
    return name.empty() ? dstd::fmt<"plugin {}">(i) : dstd::fmt<"'{}'">(name);
}

// The name a plugin's channels and phase results are declared under.
dstd::String port_namespace(const Plugins::Impl::Loaded& loaded) {
    return loaded.declared_name.empty() ? loaded.name : loaded.declared_name;
}

dstd::Vector<dstd::String> name_list(const char* const* (*slot)(void*),
                                     void* self) {
    dstd::Vector<dstd::String> out;
    if (!slot) return out;
    const char* const* names = slot(self);
    if (!names) return out;
    for (; *names; ++names)
        if (**names) out.emplace_back(*names);
    return out;
}

const char* overflow_name(std::int32_t overflow) {
    switch (overflow) {
        case DFTU_OVERFLOW_BLOCK:
            return "Block";
        case DFTU_OVERFLOW_FAIL:
            return "Fail";
        default:
            return "DropOldest";
    }
}

/// The plan-time rules of the ports the factories declared: one namespace per
/// plugin, every channel end agrees with its owner, a Block channel has its
/// ends in separately scheduled tasks, and a Block channel adds an edge from
/// each sending plugin to each receiving plugin to the order graph, as its ends
/// declared them, so a cycle of Block channels is refused with the
/// provides/consumes cycles. A Fail or DropOldest channel never waits and adds
/// no edge.
dstd::Result<void> settle_ports(
    Plugins::Impl& impl,
    dstd::Vector<dstd::Pair<std::size_t, std::size_t>>& edges,
    dstd::Vector<dstd::String>& labels) {
    impl.channels.clear();
    impl.phases.clear();
    dstd::HashMap<dstd::String, std::size_t> spaces;
    for (std::size_t i = 0; i < impl.plugins.size(); ++i) {
        const auto& loaded = impl.plugins[i];
        if (loaded.ports.empty()) continue;
        auto [it, fresh] = spaces.emplace(loaded.declared_name, i);
        if (!fresh) {
            return common::make_error<
                "plugins {} and {} both declare ports under the namespace "
                "'plugin.{}.'; each plugin needs a name of its own">(
                common::ErrorCode::InvalidArgument,
                plugin_label(impl, it->second), plugin_label(impl, i),
                loaded.declared_name);
        }
        for (const PhaseDecl& phase : loaded.ports.phases)
            impl.phases.push_back(phase);
    }

    struct Owner {
        std::size_t plugin;
        const ChannelDecl* decl;
    };
    dstd::HashMap<dstd::String, Owner> owners;
    for (std::size_t i = 0; i < impl.plugins.size(); ++i) {
        for (const ChannelDecl& decl : impl.plugins[i].ports.channels) {
            if (decl.is_ref) continue;
            owners.emplace(decl.name, Owner{i, &decl});
            impl.channels.push_back(decl);
        }
    }

    dstd::HashMap<dstd::String, bool> ends_in_tasks;
    for (const auto& [name, owner] : owners)
        ends_in_tasks.emplace(name, owner.decl->task_end);
    for (std::size_t i = 0; i < impl.plugins.size(); ++i) {
        for (const ChannelDecl& decl : impl.plugins[i].ports.channels) {
            if (!decl.is_ref) continue;
            const auto it = owners.find(decl.name);
            if (it == owners.end()) {
                return common::make_error<
                    "plugin {} opens channel '{}' which no loaded plugin "
                    "declares">(common::ErrorCode::NotFound,
                                plugin_label(impl, i), decl.name);
            }
            const ChannelDecl& own = *it->second.decl;
            if (own.credits != decl.credits || own.overflow != decl.overflow ||
                own.max_msg_bytes != decl.max_msg_bytes) {
                return common::make_error<
                    "channel '{}': plugin {} declares {} credits, overflow {} "
                    "and {} bytes a message, but its owner {} declares {}, {} "
                    "and {}">(common::ErrorCode::InvalidArgument, decl.name,
                              plugin_label(impl, i), decl.credits,
                              overflow_name(decl.overflow), decl.max_msg_bytes,
                              plugin_label(impl, it->second.plugin),
                              own.credits, overflow_name(own.overflow),
                              own.max_msg_bytes);
            }
            ends_in_tasks[decl.name] =
                ends_in_tasks[decl.name] && decl.task_end;
        }
    }
    for (const auto& [name, owner] : owners) {
        if (owner.decl->overflow != DFTU_OVERFLOW_BLOCK ||
            ends_in_tasks[name]) {
            continue;
        }
        return common::make_error<
            "channel '{}' has overflow Block, but its ends share the fused "
            "scan stage, where a full channel would stop the loop that must "
            "drain it. Declare DFTU_CHANNEL_F_TASK_END at every end that "
            "runs in a task, or use Fail or DropOldest">(
            common::ErrorCode::InvalidArgument, name);
    }
    for (const auto& [name, owner] : owners) {
        if (owner.decl->overflow != DFTU_OVERFLOW_BLOCK) continue;
        dstd::Vector<std::size_t> senders;
        dstd::Vector<std::size_t> receivers;
        for (std::size_t i = 0; i < impl.plugins.size(); ++i) {
            for (const ChannelDecl& decl : impl.plugins[i].ports.channels) {
                if (decl.name != name) continue;
                if (!decl.sends && !decl.receives) {
                    return common::make_error<
                        "channel '{}' has overflow Block, but plugin {} "
                        "declares its end without DFTU_CHANNEL_F_SENDS or "
                        "DFTU_CHANNEL_F_RECEIVES; a blocking channel needs "
                        "the direction of every end to order the plugins">(
                        common::ErrorCode::InvalidArgument, name,
                        plugin_label(impl, i));
                }
                if (decl.sends) senders.push_back(i);
                if (decl.receives) receivers.push_back(i);
            }
        }
        for (const std::size_t from : senders) {
            for (const std::size_t to : receivers) {
                if (from == to) continue;
                edges.emplace_back(from, to);
                labels.push_back(name);
            }
        }
    }
    return {};
}

// The cycle that keeps `sorted[i]` false: every node Kahn's sort could not
// place has an unplaced predecessor, so walking back from one reaches a node
// twice, and the nodes between the two visits are the cycle.
dstd::String describe_cycle(
    const Plugins::Impl& impl, const dstd::Vector<bool>& sorted,
    const dstd::Vector<dstd::Pair<std::size_t, std::size_t>>& edges,
    const dstd::Vector<dstd::String>& labels) {
    std::size_t at = 0;
    while (at < sorted.size() && sorted[at]) ++at;
    dstd::Vector<std::size_t> walk;
    dstd::Vector<std::size_t> via;
    dstd::Vector<bool> seen(sorted.size(), false);
    while (!seen[at]) {
        seen[at] = true;
        walk.push_back(at);
        std::size_t edge = edges.size();
        for (std::size_t e = 0; e < edges.size(); ++e) {
            if (edges[e].second == at && !sorted[edges[e].first]) {
                edge = e;
                break;
            }
        }
        via.push_back(edge);
        at = edges[edge].first;
    }
    std::size_t start = 0;
    while (walk[start] != at) ++start;
    const std::size_t last = walk.size() - 1;
    dstd::String text;
    for (std::size_t k = last; k > start; --k) {
        dstd::fmt_append<"{} -[{}]-> ">(text, plugin_label(impl, walk[k]),
                                        labels[via[k - 1]]);
    }
    dstd::fmt_append<"{} -[{}]-> ">(text, plugin_label(impl, walk[start]),
                                    labels[via[last]]);
    text += plugin_label(impl, walk[last]);
    return text;
}

/// Order the fold so every provider of a name runs before the plugins that
/// consume it, from the plugins' declared provides/consumes.
dstd::Result<void> settle_order(Plugins::Impl& impl) {
    for (std::size_t i = 0; i < impl.plugins.size(); ++i) {
        const dftu_plugin* pl = impl.plugins[i].plugin;
        for (const auto* slot : {&pl->provides, &pl->consumes})
            for (const dstd::String& name : name_list(*slot, pl->self))
                if (is_host_namespace(name))
                    return common::make_error<
                        "plugin {} declares '{}'; the 'dftu.' namespace "
                        "belongs to the host and is refused to plugins">(
                        common::ErrorCode::InvalidArgument,
                        plugin_label(impl, i), name);
    }

    dstd::HashMap<dstd::String, std::size_t> producer;
    for (std::size_t i = 0; i < impl.plugins.size(); ++i) {
        const dftu_plugin* pl = impl.plugins[i].plugin;
        for (const dstd::String& name : name_list(pl->provides, pl->self)) {
            auto [it, fresh] = producer.emplace(name, i);
            if (!fresh)
                return common::make_error<
                    "plugins {} and {} both provide '{}'. Names are "
                    "lowercased, so two identifiers that differ only in case "
                    "collide here">(common::ErrorCode::InvalidArgument,
                                    plugin_label(impl, it->second),
                                    plugin_label(impl, i), name);
        }
    }

    dstd::Vector<dstd::Pair<std::size_t, std::size_t>> edges;
    dstd::Vector<dstd::String> labels;
    for (std::size_t i = 0; i < impl.plugins.size(); ++i) {
        const dftu_plugin* pl = impl.plugins[i].plugin;
        for (const dstd::String& name : name_list(pl->consumes, pl->self)) {
            auto it = producer.find(name);
            if (it == producer.end())
                return common::make_error<
                    "plugin {} consumes '{}' which no loaded plugin "
                    "provides">(common::ErrorCode::NotFound,
                                plugin_label(impl, i), name);
            if (it->second != i) {
                edges.emplace_back(it->second, i);
                labels.push_back(name);
            }
        }
    }

    DFTRACER_UTILS_TRY_VOID(settle_ports(impl, edges, labels));

    impl.order = topo_sort(impl.plugins.size(), edges);
    if (impl.order.size() != impl.plugins.size()) {
        dstd::Vector<bool> sorted(impl.plugins.size(), false);
        for (std::size_t i : impl.order) sorted[i] = true;
        impl.order.clear();
        return common::make_error<
            "plugin provides/consumes graph has a cycle: {}">(
            common::ErrorCode::InvalidArgument,
            describe_cycle(impl, sorted, edges, labels));
    }
    return {};
}

void settle_prune(Plugins::Impl& impl) {
    dstd::Vector<const char*> plan_queries;
    plan_queries.reserve(impl.plugins.size());
    for (const auto& p : impl.plugins)
        plan_queries.push_back(p.plugin->plan_duql
                                   ? p.plugin->plan_duql(p.plugin->self)
                                   : nullptr);
    impl.prune = detail::plugin_union_prune_query(plan_queries);
}

}  // namespace

namespace detail {

dstd::Optional<Query> plugin_union_prune_query(
    const dstd::Vector<const char*>& plan_queries) {
    if (plan_queries.empty()) return dstd::nullopt;
    dstd::Vector<dstd::StringView> distinct;
    for (const char* s : plan_queries) {
        if (!s || !*s) return dstd::nullopt;
        dstd::StringView sv{s};
        if (!duql::try_parse(sv)) {
            DFTRACER_UTILS_LOG_DEBUG(
                "Plugin plan_duql '{}' failed to parse; scanning all", s);
            return dstd::nullopt;
        }
        if (std::find(distinct.begin(), distinct.end(), sv) == distinct.end())
            distinct.emplace_back(sv);
    }
    if (distinct.size() == 1) return duql::try_parse(distinct.front());
    dstd::String uni;
    for (const auto& d : distinct) {
        if (!uni.empty()) uni += " or ";
        uni += '(';
        uni += d;
        uni += ')';
    }
    return duql::try_parse(uni);
}

}  // namespace detail

Plugins::Builder::Builder() : state_(dstd::make_unique<State>()) {}
Plugins::Builder::~Builder() = default;
Plugins::Builder::Builder(Builder&&) noexcept = default;
Plugins::Builder& Plugins::Builder::operator=(Builder&&) noexcept = default;

Plugins::Builder& Plugins::Builder::add(dstd::String path, ConfigTree config) {
    state_->pending.push_back(
        {std::move(path), std::move(config), true, dstd::nullopt});
    return *this;
}

Plugins::Builder& Plugins::Builder::add(dstd::String path,
                                        dstd::String config_json) {
    State::Pending pending;
    pending.path = std::move(path);
    pending.config_json = std::move(config_json);
    state_->pending.push_back(std::move(pending));
    return *this;
}

Plugins::Builder& Plugins::Builder::add(dstd::String path) {
    state_->pending.push_back(
        {std::move(path), ConfigTree{}, false, dstd::nullopt});
    return *this;
}

dstd::Result<Plugins> Plugins::Builder::build() {
    auto impl = dstd::make_unique<Impl>();
    impl->plugins.reserve(state_->pending.size());
    for (auto& pending : state_->pending) {
        const dftu_value* root = nullptr;
        if (pending.config_json) {
            auto parsed = parse_config_json(pending.path, *pending.config_json);
            if (!parsed) return dstd::Unexpected(std::move(parsed).error());
            impl->configs.push_back(std::move(*parsed));
            root = impl->configs.back().root();
        } else if (pending.has_config) {
            impl->configs.push_back(std::move(pending.config));
            root = impl->configs.back().root();
        }
        auto loaded = load_plugin(pending.path, root);
        if (!loaded) return dstd::Unexpected(std::move(loaded).error());
        impl->plugins.push_back(std::move(*loaded));
    }
    auto ordered = settle_order(*impl);
    if (!ordered) return dstd::Unexpected(std::move(ordered).error());
    settle_prune(*impl);
    return PluginsInternalAccess::make(std::move(impl));
}

Plugins::Builder Plugins::builder() { return Builder{}; }

Plugins::Plugins(dstd::UniquePtr<Impl> impl) : impl_(std::move(impl)) {}
Plugins::Plugins(Plugins&&) noexcept = default;
Plugins& Plugins::operator=(Plugins&&) noexcept = default;
Plugins::~Plugins() = default;

std::size_t Plugins::size() const { return impl_->plugins.size(); }

dstd::Vector<Plugins::PluginInfo> Plugins::describe() const {
    dstd::Vector<PluginInfo> out;
    out.reserve(impl_->plugins.size());
    for (const auto& p : impl_->plugins) {
        PluginInfo info;
        info.path = p.path;
        info.name = p.declared_name;
        info.version = p.version;
        info.abi_version = DFTRACER_UTILS_PLUGIN_ABI_VERSION;
        info.has_plan_duql = p.plugin->plan_duql != nullptr;
        info.has_fold =
            p.plugin->make_slice != nullptr || p.plugin->transform != nullptr;
        info.provides = name_list(p.plugin->provides, p.plugin->self);
        info.consumes = name_list(p.plugin->consumes, p.plugin->self);
        info.reads = name_list(p.plugin->reads, p.plugin->self);
        for (const dstd::String& op : p.registered_ops)
            info.ops.push_back(op_summary(op));
        info.udfs = p.registered_udfs;
        info.udafs = p.registered_udafs;
        info.providers = p.registered_providers;
        info.nodes = p.registered_nodes;
        info.index_extensions = p.registered_index_extensions;
        info.file_formats = p.registered_file_formats;
        info.sinks = p.registered_sinks;
        for (const RegisteredState& state : p.states)
            info.states.push_back(state.name);
        if (p.plugin->config_keys) {
            const dftu_config_key* keys = p.plugin->config_keys(p.plugin->self);
            for (; keys && keys->name; ++keys) {
                dstd::String line = keys->name;
                line += " (";
                line += keys->kind == DFTU_CONFIG_ANY
                            ? "any"
                            : value_kind_name(keys->kind);
                if (keys->required) line += ", required";
                line += ")";
                if (keys->doc && *keys->doc)
                    line.append(" - ").append(keys->doc);
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

coro::CoroTask<dstd::Result<PluginRun>> Plugins::run(const View& view) const {
    dftracer::utils::dstd::StringIntern intern;
    PluginRun out;
    // One registry for the whole scan; every plugin's finalize publishes its
    // merged accumulators here and later plugins read them by name.
    SharedResultRegistry shared;
    PortHub hub(impl_->channels, impl_->phases);
    dstd::Vector<dstd::UniquePtr<PluginFold>> owned;
    owned.reserve(impl_->plugins.size());
    dstd::Vector<Fold*> folds;
    folds.reserve(impl_->plugins.size());
    for (std::size_t i : impl_->order) {
        owned.push_back(dstd::make_unique<PluginFold>(
            impl_->plugins[i].plugin, intern, &shared, &out.results,
            impl_->plugins[i].name, view.memory_budget_bytes(),
            &impl_->plugins[i].states, impl_->plugins[i].memory));
        owned.back()->bind_ports(&hub, port_namespace(impl_->plugins[i]));
        folds.push_back(owned.back().get());
    }

    const View pruned = prune(view);
    auto ran = co_await pruned.run_folds(folds, intern);
    hub.close_all();
    if (!ran) co_return ran.error();
    out.stats = *ran;
    co_return out;
}

trace::views::Deferred<PluginRun> Plugins::attach(
    trace::views::ViewSession& session) const {
    // Captured by the factory closures (owned by the session through execute)
    // so it outlives the scan.
    auto shared = dstd::make_shared<SharedResultRegistry>();
    auto out = dstd::make_shared<PluginRun>();
    auto executed = dstd::make_shared<bool>(false);
    auto hub = dstd::make_shared<PortHub>(impl_->channels, impl_->phases);
    utils::plugins::NamedResultRegistry* named = &out->results;
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
        dstd::String name = impl_->plugins[i].name;
        dstd::String space = port_namespace(impl_->plugins[i]);
        const StateRegistry* states = &impl_->plugins[i].states;
        dstd::SharedPtr<PluginMemory> memory = impl_->plugins[i].memory;
        // A session does not expose its plan's budget here, so an attached
        // plugin's accumulators spill on the auto policy rather than on an
        // explicit session budget.
        session.attach_fold_factory(
            [plugin, shared, named, name, states, memory, hub,
             space](dftracer::utils::dstd::StringIntern& intern)
                -> dstd::UniquePtr<trace::views::detail::Fold> {
                auto fold = dstd::make_unique<PluginFold>(
                    plugin, intern, shared.get(), named, name, 0, states,
                    memory);
                fold->bind_ports(hub.get(), space);
                return fold;
            },
            // The fused scan finalizes every attached fold together, so the
            // last one to finalize marks the handle resolved.
            idx + 1 == n
                ? dstd::Function<dstd::Result<void>()>([executed, hub]() {
                      hub->close_all();
                      *executed = true;
                      return dstd::Result<void>{};
                  })
                : dstd::Function<dstd::Result<void>()>(
                      []() { return dstd::Result<void>{}; }));
    }
    return {out, executed};
}

dstd::Result<Plugins> build_injected_plugins(
    dstd::Vector<dftu_plugin*> plugins) {
    auto impl = dstd::make_unique<Plugins::Impl>();
    impl->plugins.reserve(plugins.size());
    for (dftu_plugin* pl : plugins)
        impl->plugins.push_back({nullptr,
                                 pl,
                                 false,
                                 {},
                                 {},
                                 {},
                                 {},
                                 {},
                                 {},
                                 {},
                                 {},
                                 {},
                                 {},
                                 {},
                                 {},
                                 {},
                                 {},
                                 {}});
    auto ordered = settle_order(*impl);
    if (!ordered) return dstd::Unexpected(std::move(ordered).error());
    settle_prune(*impl);
    return PluginsInternalAccess::make(std::move(impl));
}

dstd::Result<Plugins> build_injected_plugins_declaring(
    dstd::Vector<InjectedPlugin> plugins) {
    auto impl = dstd::make_unique<Plugins::Impl>();
    impl->plugins.reserve(plugins.size());
    for (const InjectedPlugin& injected : plugins) {
        Plugins::Impl::Loaded loaded;
        loaded.plugin = injected.plugin;
        loaded.owned = false;
        loaded.name = injected.name;
        loaded.declared_name = injected.name;
        if (injected.declare != nullptr) {
            BuildHost build_host(injected.name);
            plugins::AbiContext ctx;
            const dftu_status declared =
                injected.declare(ctx.get(), build_host.host());
            if (declared < 0) {
                return common::make_error<"plugin '{}' factory failed: {}">(
                    common::ErrorCode::InvalidArgument, injected.name,
                    ctx.error(declared, "declare").message());
            }
            loaded.ports = build_host.take_ports();
            loaded.states = build_host.take_states();
        }
        impl->plugins.push_back(std::move(loaded));
    }
    auto ordered = settle_order(*impl);
    if (!ordered) return dstd::Unexpected(std::move(ordered).error());
    settle_prune(*impl);
    return PluginsInternalAccess::make(std::move(impl));
}

dstd::UniquePtr<PortHub> make_port_hub(const Plugins& set) {
    const Plugins::Impl& impl = PluginsInternalAccess::impl(set);
    return dstd::make_unique<PortHub>(impl.channels, impl.phases);
}

dstd::Vector<std::size_t> fold_order(const Plugins& set) {
    return PluginsInternalAccess::impl(set).order;
}

}  // namespace dftracer::utils::plugins::host
