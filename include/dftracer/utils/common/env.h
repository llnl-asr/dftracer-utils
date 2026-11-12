// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_ENV_H
#define DFTRACER_UTILS_COMMON_ENV_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>
#include <type_traits>

namespace dftracer::utils::common {

class Env {
   public:
    template <typename T = dstd::StringView>
    static dstd::Optional<T> get(dstd::StringView name);
};

template <typename T>
dstd::Optional<T> Env::get(dstd::StringView) {
    static_assert(sizeof(T) == 0,
                  "Env::get<T>() requires an explicit specialization");
    return dstd::nullopt;
}

template <>
dstd::Optional<dstd::StringView> Env::get<dstd::StringView>(
    dstd::StringView name);

template <>
dstd::Optional<int> Env::get<int>(dstd::StringView name);

template <>
dstd::Optional<std::uint64_t> Env::get<std::uint64_t>(dstd::StringView name);

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_ENV_H
