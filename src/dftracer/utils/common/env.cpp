#include <dftracer/utils/common/env.h>

#include <charconv>
#include <cstdlib>
#include <string>

namespace dftracer::utils::common {

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

template <>
std::optional<std::uint64_t> Env::get<std::uint64_t>(std::string_view name) {
    auto value = get<std::string_view>(name);
    if (!value.has_value()) {
        return std::nullopt;
    }

    std::uint64_t parsed = 0;
    auto* begin = value->data();
    auto* end = begin + value->size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

}  // namespace dftracer::utils::common
