#ifndef DFTRACER_UTILS_CORE_ENV_H
#define DFTRACER_UTILS_CORE_ENV_H

#include <optional>
#include <string_view>
#include <type_traits>

namespace dftracer::utils {

class Env {
   public:
    template <typename T = std::string_view>
    static std::optional<T> get(std::string_view name);

    static int rocksdb_max_open_files();
};

template <typename T>
std::optional<T> Env::get(std::string_view name) {
    static_assert(sizeof(T) == 0,
                  "Env::get<T>() requires an explicit specialization");
    (void)name;
    return std::nullopt;
}

template <>
std::optional<std::string_view> Env::get<std::string_view>(
    std::string_view name);

template <>
std::optional<int> Env::get<int>(std::string_view name);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_ENV_H
