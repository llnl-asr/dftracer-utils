// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/dstd/to_chars.h>

#include <cstdlib>

namespace dftracer::utils::common {

template <>
dstd::Optional<dstd::StringView> Env::get<dstd::StringView>(
    dstd::StringView name) {
    dstd::String key(name);
    const char* value = std::getenv(key.c_str());
    if (value == nullptr || value[0] == '\0') {
        return dstd::nullopt;
    }
    return dstd::StringView(value);
}

template <>
dstd::Optional<int> Env::get<int>(dstd::StringView name) {
    auto value = get<dstd::StringView>(name);
    if (!value.has_value()) {
        return dstd::nullopt;
    }

    return dstd::from_chars<int>(*value);
}

template <>
dstd::Optional<std::uint64_t> Env::get<std::uint64_t>(dstd::StringView name) {
    auto value = get<dstd::StringView>(name);
    if (!value.has_value()) {
        return dstd::nullopt;
    }

    return dstd::from_chars<std::uint64_t>(*value);
}

}  // namespace dftracer::utils::common
