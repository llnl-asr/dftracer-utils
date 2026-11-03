// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGINS_H
#define DFTRACER_UTILS_PLUGINS_PLUGINS_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/result_registry.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstddef>
#include <cstdint>

// Load plugin shared libraries and run them as folds over one fused scan of a
// View. Free of the C ABI: the ABI types and the test seams live in
// src/dftracer/utils/plugins/plugins_internal.h.
namespace dftracer::utils::plugins {

class ConfigTree;

/// One run's output: the scan counters and the named results the plugins
/// emitted. Owned by the value, so one plugin set can back many runs.
struct PluginRun {
    trace::views::ExportStats stats;
    NamedResultRegistry results;
};

/// An immutable set of loaded plugins. Everything a run needs - dlopen, the ABI
/// gate, the fold order, and the index prune - is settled by Builder::build(),
/// so the set holds no per-run state. Adding a plugin means building a new set.
class Plugins {
   public:
    class Builder {
       public:
        Builder();
        ~Builder();
        Builder(Builder&&) noexcept;
        Builder& operator=(Builder&&) noexcept;

        /// Queue a shared library and the config tree its factory receives.
        Builder& add(dstd::String path, ConfigTree config);

        /// Queue a shared library and a JSON object literal for its config.
        /// The caller owns @p config_json only for the duration of this call;
        /// it is copied and parsed at build(), so a parse failure or a
        /// non-object root is reported there, as a `build()` error naming
        /// this plugin's path, not thrown by add().
        Builder& add(dstd::String path, dstd::String config_json);

        Builder& add(dstd::String path);

        /// dlopen and gate the ABI version in queue order, then order the fold
        /// from the plugins' declared provides/consumes. The first failure
        /// names the plugin and the cause; nothing is partially built and no
        /// plugin outlives the failed call.
        common::Result<Plugins> build();

       private:
        struct State;
        dstd::UniquePtr<State> state_;
    };

    static Builder builder();

    /// The set's private state; nameable so the internal seams can reach it,
    /// defined only in the implementation.
    struct Impl;

    Plugins(Plugins&&) noexcept;
    Plugins& operator=(Plugins&&) noexcept;
    ~Plugins();

    std::size_t size() const;

    /// Static facts about one loaded plugin: the path it was loaded from and
    /// what build() already resolved from its descriptor, with no scan. The
    /// only way to discover a compiled plugin's provides/consumes without its
    /// source.
    struct PluginInfo {
        dstd::String path;  ///< the shared-library path given to Builder::add
        std::uint32_t abi_version = 0;
        bool has_plan_duql =
            false;          ///< true if the plugin declares a plan_duql
        dstd::Vector<dstd::String> provides;
        dstd::Vector<dstd::String> consumes;
        /// One entry per op the plugin's factory added to the host op
        /// registry: `name(arg, ...) -> ret [kind]`.
        dstd::Vector<dstd::String> ops;
        /// The batch columns the plugin declared it reads. Empty means it
        /// declared none, so it is handed every column.
        dstd::Vector<dstd::String> reads;
        /// One entry per declared config key: `name (kind[, required]) - doc`.
        /// Empty when the plugin declares none, in which case its config is
        /// not validated either.
        dstd::Vector<dstd::String> config_keys;
    };

    /// One entry per loaded plugin, in registration order.
    dstd::Vector<PluginInfo> describe() const;

    /// Drive every plugin as a fold over one fused, pruned scan of `view`.
    coro::CoroTask<common::Result<PluginRun>> run(
        const trace::views::View& view) const;

    /// Attach every plugin to `session` as a fused fold branch, in fold order,
    /// so they co-scan with its other branches. Call before session.execute().
    /// The returned handle resolves like every other @ref
    /// dftracer::utils::trace::views::ViewSession "ViewSession" handle:
    /// `->results` and `->stats` throw if read before session.execute()
    /// completes.
    ///
    /// The index prune that run() applies would starve a co-scanning branch
    /// (e.g. a collect()) of events it is entitled to, so attach() only
    /// OFFERS it, via ViewSession::propose_base_prune; the session applies it
    /// at execute() and only when the plugins are its sole branch. Nothing is
    /// required of the caller either way.
    ///
    /// `stats` stays default: the shared scan's own counters are
    /// session.execute()'s return, not a plugin-only count.
    trace::views::Deferred<PluginRun> attach(
        trace::views::ViewSession& session) const;

   private:
    explicit Plugins(dstd::UniquePtr<Impl> impl);
    friend struct PluginsInternalAccess;

    /// `view` narrowed by the union of the plugins' plan_duql filters (the
    /// weakest predicate that still selects every event any plugin keeps), or
    /// unchanged when no prune applies. Only run() calls this: it owns the
    /// scan it runs, so it can narrow directly rather than offering the
    /// narrowing as attach() does.
    trace::views::View prune(const trace::views::View& view) const;

    dstd::UniquePtr<Impl> impl_;
};

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_PLUGINS_H
