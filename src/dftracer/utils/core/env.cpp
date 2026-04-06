#include <dftracer/utils/core/env.h>

#include <charconv>
#include <cstdlib>
#include <string>

namespace dftracer::utils {

template <>
std::optional<std::string_view> Env::get<std::string_view>(
    std::string_view name) {
    std::string key(name);
    const char* value = std::getenv(key.c_str());
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string_view(value);
}

template <>
std::optional<int> Env::get<int>(std::string_view name) {
    auto value = get<std::string_view>(name);
    if (!value.has_value()) {
        return std::nullopt;
    }

    int parsed = 0;
    auto* begin = value->data();
    auto* end = begin + value->size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

int Env::rocksdb_max_open_files() {
    static const int cached_value = [] {
        constexpr int default_max_open_files = 32;
        constexpr std::string_view env_name =
            "DFTRACER_UTILS_ROCKSDB_MAX_OPEN_FILES";

        auto configured = get<int>(env_name);
        if (!configured.has_value()) {
            return default_max_open_files;
        }

        if (*configured == -1 || *configured > 0) {
            return *configured;
        }
        return default_max_open_files;
    }();

    return cached_value;
}

}  // namespace dftracer::utils
