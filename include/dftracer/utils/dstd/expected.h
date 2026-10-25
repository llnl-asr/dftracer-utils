#ifndef DFTRACER_UTILS_DSTD_EXPECTED_H
#define DFTRACER_UTILS_DSTD_EXPECTED_H

#if __cplusplus >= 202302L && __has_include(<expected>)

#include <expected>

namespace dftracer::utils::dstd {

template <class T, class E>
using Expected = std::expected<T, E>;

template <class E>
class Unexpected : public std::unexpected<E> {
   public:
    using std::unexpected<E>::unexpected;
};

template <class E>
Unexpected(E) -> Unexpected<E>;

}  // namespace dftracer::utils::dstd

#else

#include <tl/expected.hpp>

namespace dftracer::utils::dstd {

template <class T, class E>
using Expected = tl::expected<T, E>;

// A wrapper, not an alias: AppleClang has no CTAD for alias templates, and
// callers write Unexpected(x) with no template argument.
template <class E>
class Unexpected : public tl::unexpected<E> {
   public:
    using tl::unexpected<E>::unexpected;
};

template <class E>
Unexpected(E) -> Unexpected<E>;

}  // namespace dftracer::utils::dstd

#endif

#endif  // DFTRACER_UTILS_DSTD_EXPECTED_H
