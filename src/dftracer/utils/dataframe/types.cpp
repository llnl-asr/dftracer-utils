#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/dataframe/types.h>

#include <deque>
#include <mutex>

namespace dftracer::utils::dataframe {

namespace {

struct Zones {
    std::mutex mutex;
    std::deque<std::string> names{std::string()};
    StringViewMap<std::uint32_t> ids;
};

Zones& zones() {
    static auto* z = new Zones();
    return *z;
}

}  // namespace

std::uint32_t intern_timezone(std::string_view name) {
    if (name.empty()) return 0;
    Zones& z = zones();
    const std::lock_guard<std::mutex> lock(z.mutex);
    if (const auto it = z.ids.find(name); it != z.ids.end()) return it->second;
    const auto id = static_cast<std::uint32_t>(z.names.size());
    z.names.emplace_back(name);
    z.ids.emplace(std::string(name), id);
    return id;
}

std::string_view timezone_name(std::uint32_t id) {
    Zones& z = zones();
    const std::lock_guard<std::mutex> lock(z.mutex);
    return z.names.at(id);
}

}  // namespace dftracer::utils::dataframe
