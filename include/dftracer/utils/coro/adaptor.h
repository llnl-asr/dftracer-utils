// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_ADAPTOR_H
#define DFTRACER_UTILS_CORO_ADAPTOR_H

#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

/// A callable that composes with the pipe: `x | adaptor` is `adaptor(x)`, and
/// `a | b` is an adaptor that applies @p a and then @p b.
template <class F>
struct Adaptor {
    F fn;

    template <class X>
    decltype(auto) operator()(X&& x) const {
        return fn(std::forward<X>(x));
    }
};

namespace detail {

template <class T>
struct is_adaptor : std::false_type {};
template <class F>
struct is_adaptor<Adaptor<F>> : std::true_type {};

template <class F>
Adaptor<F> make_adaptor(F fn) {
    return Adaptor<F>{std::move(fn)};
}

}  // namespace detail

template <class X, class F>
    requires(!detail::is_adaptor<std::remove_cvref_t<X>>::value)
decltype(auto) operator|(X&& x, const Adaptor<F>& adaptor) {
    return adaptor(std::forward<X>(x));
}

template <class F, class G>
auto operator|(Adaptor<F> first, Adaptor<G> second) {
    return detail::make_adaptor(
        [first = std::move(first), second = std::move(second)](auto&& x) {
            return second(first(std::forward<decltype(x)>(x)));
        });
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_ADAPTOR_H
