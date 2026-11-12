// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_BENCHMARKS_BENCH_OK_H
#define DFTRACER_UTILS_BENCHMARKS_BENCH_OK_H

#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/result.h>

#include <type_traits>
#include <utility>

// The value of a Result a bench expects to succeed. An error ends the bench
// with its text.
namespace dftu::bench {

template <typename E>
[[noreturn]] void fail(const E& error) {
    dftracer::utils::dstd::fatal("bench: ", error.format().c_str());
}

template <typename T>
T ok(dftracer::utils::dstd::Result<T>&& result) {
    if (!result.has_value()) fail(result.error());
    if constexpr (!std::is_void_v<T>) return std::move(*result);
}

}  // namespace dftu::bench

#endif  // DFTRACER_UTILS_BENCHMARKS_BENCH_OK_H
