#ifndef DFTRACER_UTILS_CORE_COMPOSE_H
#define DFTRACER_UTILS_CORE_COMPOSE_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/coro/when_any.h>

#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities {

namespace detail {

template <typename T>
struct coro_value;
template <typename T>
struct coro_value<coro::CoroTask<T>> {
    using type = T;
};

/// The O in the coro::CoroTask<O> that op F yields when called with input I.
template <typename F, typename I>
using async_result_t = typename coro_value<std::invoke_result_t<F&, I>>::type;

}  // namespace detail

/// An async op is any callable of shape `I -> coro::CoroTask<O>`. This is the
/// whole authoring contract: satisfy it with a lambda, a struct with
/// operator(), or a free function.
template <typename F, typename I>
concept AsyncOpFor = requires(F& f, I in) {
    typename detail::coro_value<std::invoke_result_t<F&, I>>::type;
};

/// Wraps a plain async callable so it carries the ranges-style composition
/// operators. The callable is stored by value; a composed op is a static
/// nested type, so composition is fully inlinable with no type erasure. The
/// only erasure in the whole model is the coroutine frame each op already
/// allocates, scheduled by its std::coroutine_handle.
template <typename F>
struct Op {
    F fn;

    template <typename I>
    auto operator()(I&& in) const {
        return fn(std::forward<I>(in));
    }
};

template <typename F>
Op<std::decay_t<F>> op(F&& f) {
    return Op<std::decay_t<F>>{std::forward<F>(f)};
}

namespace detail {

template <typename A, typename B, typename I>
coro::CoroTask<async_result_t<B, async_result_t<A, I>>> pipe_run(A a, B b,
                                                                 I in) {
    auto mid = co_await a(std::move(in));
    co_return co_await b(std::move(mid));
}

template <typename A, typename B, typename I>
coro::CoroTask<std::tuple<async_result_t<A, I>, async_result_t<B, I>>> all_run(
    A a, B b, I in) {
    auto ra = co_await a(in);
    auto rb = co_await b(in);
    co_return std::make_tuple(std::move(ra), std::move(rb));
}

template <typename A, typename B, typename I>
coro::CoroTask<async_result_t<A, I>> any_run(A a, B b, I in) {
    auto r = co_await coro::when_any(a(in), b(in));
    co_return std::move(r.result);
}

template <typename F, typename Item, typename In>
coro::CoroTask<std::vector<async_result_t<F, Item>>> map_run(F f, In in) {
    std::vector<coro::CoroTask<async_result_t<F, Item>>> tasks;
    tasks.reserve(in.size());
    for (auto& x : in) tasks.push_back(f(x));
    co_return co_await coro::when_all(std::move(tasks));
}

template <typename U, typename Combine, typename In>
coro::CoroTask<U> fold_run(U acc, Combine combine, In in) {
    for (auto& x : in) acc = combine(std::move(acc), x);
    co_return acc;
}

}  // namespace detail

/// map: lift an op `Item -> CoroTask<O>` to `vector<Item> ->
/// CoroTask<vector<O>>`, running every element concurrently via when_all.
template <typename F>
auto map(F f) {
    return op([f = std::move(f)](auto&& in) {
        using In = std::decay_t<decltype(in)>;
        using Item = typename In::value_type;
        return detail::map_run<F, Item, In>(f, std::forward<decltype(in)>(in));
    });
}

/// fold: collapse a `vector<Item>` into a single U by a synchronous combine,
/// yielding `vector<Item> -> CoroTask<U>`. Combine must be associative to be
/// meaningfully reorderable, but this op folds left-to-right.
template <typename U, typename Combine>
auto fold(U init, Combine combine) {
    return op(
        [init = std::move(init), combine = std::move(combine)](auto&& in) {
            using In = std::decay_t<decltype(in)>;
            return detail::fold_run<U, Combine, In>(
                init, combine, std::forward<decltype(in)>(in));
        });
}

/// Pipe (ranges-style `|`): (a | b)(x) runs a, then feeds its result into b.
template <typename A, typename B>
auto operator|(Op<A> a, Op<B> b) {
    return op([fa = std::move(a.fn), fb = std::move(b.fn)](auto&& in) {
        using I = std::decay_t<decltype(in)>;
        return detail::pipe_run<A, B, I>(fa, fb,
                                         std::forward<decltype(in)>(in));
    });
}

/// All (`&&`): (a && b)(x) runs both ops on x and yields a tuple of results.
template <typename A, typename B>
auto operator&&(Op<A> a, Op<B> b) {
    return op([fa = std::move(a.fn), fb = std::move(b.fn)](auto&& in) {
        using I = std::decay_t<decltype(in)>;
        return detail::all_run<A, B, I>(fa, fb, std::forward<decltype(in)>(in));
    });
}

/// Any (`||`): (a || b)(x) races both ops on x and yields the first to finish.
/// Both ops must yield the same output type.
template <typename A, typename B>
auto operator||(Op<A> a, Op<B> b) {
    return op([fa = std::move(a.fn), fb = std::move(b.fn)](auto&& in) {
        using I = std::decay_t<decltype(in)>;
        return detail::any_run<A, B, I>(fa, fb, std::forward<decltype(in)>(in));
    });
}

}  // namespace dftracer::utils::utilities

#endif  // DFTRACER_UTILS_CORE_COMPOSE_H
