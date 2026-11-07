// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>

#include <mutex>

namespace dftracer::utils::dataframe {

namespace {

struct Tables {
    dstd::Deque<dstd::String> names{dstd::String()};
    dstd::StringViewMap<std::uint32_t> ids;
};

struct Zones {
    std::mutex mutex;
    dstd::Optional<Tables> tables;
};

Zones& zones() {
    Zones& held = dstd::Singleton<Zones>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, [] {
            Zones& dropped = dstd::Singleton<Zones>::get();
            const std::lock_guard<std::mutex> lock(dropped.mutex);
            dropped.tables.reset();
        });
        return true;
    }();
    return held;
}

Tables& tables(Zones& z) {
    if (!z.tables) z.tables.emplace();
    return *z.tables;
}

}  // namespace

std::uint32_t intern_timezone(dstd::StringView name) {
    if (name.empty()) return 0;
    Zones& z = zones();
    const std::lock_guard<std::mutex> lock(z.mutex);
    Tables& t = tables(z);
    if (const auto it = t.ids.find(name); it != t.ids.end()) return it->second;
    const auto id = static_cast<std::uint32_t>(t.names.size());
    t.names.emplace_back(name);
    t.ids.emplace(dstd::String(name), id);
    return id;
}

dstd::StringView timezone_name(std::uint32_t id) {
    Zones& z = zones();
    const std::lock_guard<std::mutex> lock(z.mutex);
    return tables(z).names.at(id);
}

}  // namespace dftracer::utils::dataframe
