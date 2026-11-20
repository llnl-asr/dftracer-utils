// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/internal/file_format_registry.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/plugins/host/global_plugins.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstddef>
#include <mutex>
#include <utility>

namespace dftracer::utils::plugins::host {
namespace {

struct Entry {
    dstd::UniquePtr<Plugins> set;
    std::size_t holders = 0;
    bool from_env = false;
};

struct State {
    std::mutex mutex;
    dstd::Optional<dstd::HashMap<dstd::String, Entry>> entries;
    bool env_done = false;
    bool hooked = false;
};

// Set while this thread builds a global entry with the lock held, so that
// build does not look for itself in the set.
thread_local bool t_loading = false;

State& state() { return dstd::Singleton<State>::get(); }

dstd::HashMap<dstd::String, Entry>& entries(State& s) {
    if (!s.entries) s.entries.emplace();
    return *s.entries;
}

dstd::String key_of(dstd::StringView path) {
    if (auto canonical = dstd::fs::canonical(dstd::PathView(path)))
        return dstd::String(canonical->str());
    return dstd::String(path);
}

// With the lock held, before the state allocates anything: frees it all at
// shutdown. The registry hooks itself on its first use, which this forces
// first, so this hook runs before it and the plugins unregister their formats
// while the registry still stands.
void hook(State& s) {
    if (s.hooked) return;
    s.hooked = true;
    dataframe::has_file_formats();
    dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, []() noexcept {
        State& dropped = state();
        dstd::Optional<dstd::HashMap<dstd::String, Entry>> gone;
        {
            const std::lock_guard<std::mutex> lock(dropped.mutex);
            gone = std::move(dropped.entries);
            dropped.entries.reset();
        }
    });
}

// With the lock held. Loads `path` or joins it; `from_env` marks it for the
// process.
dstd::Result<void> load_locked(State& s, dstd::StringView path, bool from_env) {
    hook(s);
    const dstd::String key = key_of(path);
    auto& all = entries(s);
    if (auto found = all.find(key); found != all.end()) {
        if (from_env)
            found->second.from_env = true;
        else
            ++found->second.holders;
        return {};
    }
    t_loading = true;
    auto built = Plugins::builder().add(dstd::String(path)).build();
    t_loading = false;
    if (!built) return built.error();
    Entry entry;
    entry.set = dstd::make_unique<Plugins>(std::move(*built));
    entry.holders = from_env ? 0 : 1;
    entry.from_env = from_env;
    all.emplace(key, std::move(entry));
    return {};
}

}  // namespace

dstd::Result<void> ensure_global_plugins() {
    trace::views::register_trace_reader();
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (s.env_done) return {};
    const auto list = common::Env::get(common::constants::plugins::PLUGINS_ENV);
    dstd::StringView rest = list ? *list : dstd::StringView();
    while (!rest.empty()) {
        const std::size_t cut = rest.find(':');
        const dstd::StringView path =
            cut == dstd::StringView::npos ? rest : rest.substr(0, cut);
        rest = cut == dstd::StringView::npos ? dstd::StringView()
                                             : rest.substr(cut + 1);
        if (path.empty()) continue;
        // A failure leaves the environment not done: the next call retries the
        // paths that are not loaded yet.
        if (auto loaded = load_locked(s, path, true); !loaded)
            return loaded.error();
    }
    s.env_done = true;
    return {};
}

bool with_global_plugin(dstd::StringView path,
                        dstd::Function<void(const Plugins&)> use) {
    if (t_loading) return false;
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.entries) return false;
    const auto found = s.entries->find(key_of(path));
    if (found == s.entries->end()) return false;
    use(*found->second.set);
    return true;
}

dstd::Result<void> load_plugin(dstd::StringView path) {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    return load_locked(s, path, false);
}

dstd::Result<void> unload_plugin(dstd::StringView path) {
    State& s = state();
    dstd::UniquePtr<Plugins> dropped;
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        hook(s);
        auto& all = entries(s);
        auto found = all.find(key_of(path));
        if (found == all.end() || found->second.holders == 0)
            return common::make_error<"plugin '{}' is not loaded">(
                common::ErrorCode::NotFound, path);
        if (found->second.holders == 1 && !found->second.from_env &&
            found->second.set->in_use())
            return common::make_error<
                "plugin '{}' cannot be unloaded: a scan through one of its "
                "formats is still open">(common::ErrorCode::InvalidArgument,
                                         path);
        if (--found->second.holders > 0 || found->second.from_env) return {};
        dropped = std::move(found->second.set);
        all.erase(found);
    }
    // Released outside the lock: the plugin's destructors run its code.
    return {};
}

}  // namespace dftracer::utils::plugins::host
