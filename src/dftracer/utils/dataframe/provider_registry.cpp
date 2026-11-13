// SPDX-License-Identifier: MIT
// A process-lifetime, name-keyed registry of source providers, mirroring the op
// registry in op_registry.cpp, and dftu_lazyframe_from_provider, which builds a
// plan over a provider by name.

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/lazyframe_handle.h>
#include <dftracer/utils/dataframe/internal/provider_registry.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>

#include <mutex>
#include <utility>

namespace dftracer::utils::dataframe {
namespace {

struct Providers {
    std::mutex mutex;
    dstd::Optional<
        dstd::HashMap<dstd::String, dstd::SharedPtr<ProviderFactory>>>
        by_name;
};

Providers& registry() {
    Providers& held = dstd::Singleton<Providers>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, []() noexcept {
            Providers& dropped = dstd::Singleton<Providers>::get();
            const std::lock_guard<std::mutex> lock(dropped.mutex);
            dropped.by_name.reset();
        });
        return true;
    }();
    return held;
}

dstd::HashMap<dstd::String, dstd::SharedPtr<ProviderFactory>>& providers() {
    Providers& p = registry();
    if (!p.by_name) p.by_name.emplace();
    return *p.by_name;
}

}  // namespace

dstd::Result<void> register_provider(dstd::StringView name,
                                     dstd::SharedPtr<ProviderFactory> factory) {
    if (name.empty() || !factory) {
        return dstd::Error::from_format<
            "provider register: the name and the provider must be set">(
            dstd::Condition::InvalidArgument);
    }
    std::lock_guard<std::mutex> lock(registry().mutex);
    auto& reg = providers();
    if (reg.find(name) != reg.end()) {
        return dstd::Error::from_format<"provider '{}' is already registered">(
            dstd::Condition::InvalidArgument, name);
    }
    reg.emplace(dstd::String(name), std::move(factory));
    return {};
}

bool unregister_provider(dstd::StringView name) {
    dstd::SharedPtr<ProviderFactory> removed;
    {
        std::lock_guard<std::mutex> lock(registry().mutex);
        auto& reg = providers();
        auto it = reg.find(name);
        if (it == reg.end()) return false;
        removed = std::move(it->second);
        reg.erase(it);
    }
    // Released outside the lock: the provider's destructor may be plugin code.
    return true;
}

dstd::SharedPtr<ProviderFactory> find_provider(dstd::StringView name) {
    std::lock_guard<std::mutex> lock(registry().mutex);
    auto it = providers().find(name);
    if (it == providers().end()) return nullptr;
    return it->second;
}

}  // namespace dftracer::utils::dataframe

extern "C" {

int dftu_provider_unregister(const char* name) {
    if (!name) return 1;
    return dftracer::utils::dataframe::unregister_provider(name) ? 0 : 1;
}

dftu_lazyframe* dftu_lazyframe_from_provider(const char* name) {
    if (!name) return nullptr;
    dftracer::utils::dstd::SharedPtr<
        dftracer::utils::dataframe::ProviderFactory>
        factory = dftracer::utils::dataframe::find_provider(name);
    if (!factory) return nullptr;
    return dftracer::utils::dataframe::lazyframe_handle_wrap(
        dftracer::utils::dataframe::LazyFrame::scan(factory->make_source()));
}

}  // extern "C"
