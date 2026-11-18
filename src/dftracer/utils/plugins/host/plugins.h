// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PLUGINS_H
#define DFTRACER_UTILS_PLUGINS_HOST_PLUGINS_H

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
// src/dftracer/utils/plugins/host/plugins_internal.h.
namespace dftracer::utils::plugins::host {

class ConfigTree;

/// Where the seeded phase results of one run came from.
enum class SeedSource : std::uint8_t {
    /// The plan has no seeded phase.
    None,
    /// The statistics in the indexes of the input files.
    Index,
    /// The host's cache of an earlier pass, keyed by the files' stat and
    /// sample hash.
    Cache,
    /// A pass of the host over the files, with no plugin code, run now.
    Pass
};

/// One run's output: the scan counters and the named results the plugins
/// emitted. Owned by the value, so one plugin set can back many runs.
struct PluginRun {
    trace::views::ExportStats stats;
    /// Where the values of the seeded phases came from; see @ref SeedSource.
    SeedSource seed_source = SeedSource::None;
    ::dftracer::utils::plugins::NamedResultRegistry results;
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
        dstd::Result<Plugins> build();

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
        /// The name the plugin declares in its dftu_plugin_info, or its
        /// load-path stem when it declares none.
        dstd::String name;
        /// The version text the plugin declares, empty when it declares none.
        dstd::String version;
        std::uint32_t abi_version = 0;
        bool has_plan_duql =
            false;  ///< true if the plugin declares a plan_duql
        /// True when the descriptor carries a fold at all: it makes slices or
        /// rewrites batches. A hand-written descriptor that only registers
        /// names reports false; one built with the C++ SDK reports true, since
        /// the SDK fills an empty fold so the host can drive the states.
        bool has_fold = false;
        dstd::Vector<dstd::String> provides;
        dstd::Vector<dstd::String> consumes;
        /// One entry per op the plugin's factory added to the host op
        /// registry: `name(arg, ...) -> ret [kind]`.
        dstd::Vector<dstd::String> ops;
        /// One entry per column op the factory registered as a UDF.
        dstd::Vector<dstd::String> udfs;
        /// One entry per accumulator the factory registered as a UDAF.
        dstd::Vector<dstd::String> udafs;
        /// One entry per state type the factory registered.
        dstd::Vector<dstd::String> states;
        /// One entry per scan provider the factory registered.
        dstd::Vector<dstd::String> providers;
        /// One entry per plan node the factory registered.
        dstd::Vector<dstd::String> nodes;
        /// One entry per index extension the factory registered.
        dstd::Vector<dstd::String> index_extensions;
        /// One entry per file format the factory registered.
        dstd::Vector<dstd::String> file_formats;
        /// One entry per sink the factory registered.
        dstd::Vector<dstd::String> sinks;
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

    /// Whether a file opened through a file format of this set is still open,
    /// as a plan of @ref dataframe::LazyFrame::scan_file holds it.
    bool in_use() const;

    /// Drive every plugin as a fold over one fused, pruned scan of `view`.
    coro::CoroTask<dstd::Result<PluginRun>> run(
        const trace::views::View& view) const;

    /// Attach every plugin to `session` as a fused fold branch, in fold order,
    /// so they co-scan with its other branches. Call before session.execute().
    /// The returned handle resolves like every other @ref
    /// dftracer::utils::trace::views::ViewSession "ViewSession" handle:
    /// `->results` and `->stats` end the process if read before
    /// session.execute() completes; execute() returns the error of a failed
    /// scan as a Result.
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

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_PLUGINS_H
