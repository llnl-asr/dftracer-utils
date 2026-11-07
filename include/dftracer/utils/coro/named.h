// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_NAMED_H
#define DFTRACER_UTILS_CORO_NAMED_H

#include <dftracer/utils/dstd/string.h>

#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

/// A task or stage with the name that progress and watchdog reports show.
/// The name is not copied, so it must outlive the run.
template <class T>
struct Named {
    dstd::StringView name;
    T value;
};

template <class T>
Named<std::remove_cvref_t<T>> named(dstd::StringView name, T&& value) {
    return {name, std::forward<T>(value)};
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_NAMED_H
