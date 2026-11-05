// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_STAGE_H
#define DFTRACER_UTILS_CORO_STAGE_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/adaptor.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/cancelled.h>
#include <dftracer/utils/coro/combinators.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/stream.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/either.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

namespace detail {

template <class S>
concept HasStageName = requires(const S& s) { dstd::StringView(s.name()); } ||
                       requires { dstd::StringView(S::NAME); };

}  // namespace detail

/**
 * @brief A stage that turns one value into one value.
 *
 * It names `Input` and `Output`, has a name (a static `NAME` or a member
 * `name()`, see @ref dftracer::utils::coro::stage_name) and a `run` that takes
 * the input by value and gives a @ref dftracer::utils::coro::CoroTask of the
 * output. `run` is const and safe to call from several tasks at once. The stage
 * must outlive every run.
 */
template <class S>
concept BatchStage =
    detail::HasStageName<S> && requires(const S& s, typename S::Input in) {
        typename S::Output;
        { s.run(std::move(in)) } -> std::same_as<CoroTask<typename S::Output>>;
    };

/**
 * @brief A stage that turns a stream of values into a stream of values.
 *
 * Same shape as @ref dftracer::utils::coro::BatchStage, but `run` takes and
 * gives an
 * @ref dftracer::utils::coro::AsyncGenerator.
 */
template <class S>
concept StreamStage =
    detail::HasStageName<S> &&
    requires(const S& s, AsyncGenerator<typename S::Input> in) {
        typename S::Output;
        {
            s.run(std::move(in))
        } -> std::same_as<AsyncGenerator<typename S::Output>>;
    };

template <class S>
concept AnyStage = BatchStage<S> || StreamStage<S>;

/// The name of a stage: its member `name()` when it has one, else `S::NAME`.
/// The view is borrowed from the stage.
template <class S>
dstd::StringView stage_name(const S& stage) {
    if constexpr (requires { dstd::StringView(stage.name()); }) {
        return stage.name();
    } else {
        return dstd::StringView(S::NAME);
    }
}

namespace detail {

template <class A, class B>
concept BatchPair = BatchStage<A> && BatchStage<B> &&
                    std::same_as<typename A::Input, typename B::Input>;

template <class A, class B>
concept StreamPair = StreamStage<A> && StreamStage<B> &&
                     std::same_as<typename A::Input, typename B::Input>;

template <class E>
struct is_stage_either : std::false_type {};
template <class A, class B>
struct is_stage_either<dstd::Either<A, B>>
    : std::bool_constant<BatchPair<A, B> || StreamPair<A, B>> {};

template <class E, bool Left, class T>
CoroTask<E> tag_task(CoroTask<T> task) {
    T value = co_await std::move(task);
    if constexpr (Left) {
        co_return E::left(std::move(value));
    } else {
        co_return E::right(std::move(value));
    }
}

template <class E, bool Left, class T>
AsyncGenerator<E> tag_stream(AsyncGenerator<T> source) {
    for (;;) {
        auto item = co_await source.next();
        if (!item) break;
        if constexpr (Left) {
            co_yield E::left(std::move(*item));
        } else {
            co_yield E::right(std::move(*item));
        }
    }
}

}  // namespace detail

/// Two stages of the same kind and the same input, one of which is held.
template <class E>
concept StageEither = detail::is_stage_either<std::remove_cvref_t<E>>::value;

/**
 * @brief A @ref dstd::Either of two stages that runs the one it holds.
 *
 * The output is the shared output type when both agree, else a
 * @ref dstd::Either of the two outputs on the side that ran.
 */
template <class A, class B>
class EitherStage {
   public:
    using Input = typename A::Input;
    static constexpr bool SAME_OUTPUT =
        std::is_same_v<typename A::Output, typename B::Output>;
    using Output = std::conditional_t<
        SAME_OUTPUT, typename A::Output,
        dstd::Either<typename A::Output, typename B::Output>>;

    explicit EitherStage(dstd::Either<A, B> stage) : stage_(std::move(stage)) {}

    dstd::StringView name() const {
        return stage_.is_left() ? stage_name(stage_.left())
                                : stage_name(stage_.right());
    }

    CoroTask<Output> run(Input in) const
        requires(BatchStage<A>)
    {
        if (stage_.is_left()) {
            if constexpr (SAME_OUTPUT) {
                return stage_.left().run(std::move(in));
            } else {
                return detail::tag_task<Output, true>(
                    stage_.left().run(std::move(in)));
            }
        }
        if constexpr (SAME_OUTPUT) {
            return stage_.right().run(std::move(in));
        } else {
            return detail::tag_task<Output, false>(
                stage_.right().run(std::move(in)));
        }
    }

    AsyncGenerator<Output> run(AsyncGenerator<Input> in) const
        requires(StreamStage<A>)
    {
        if (stage_.is_left()) {
            if constexpr (SAME_OUTPUT) {
                return stage_.left().run(std::move(in));
            } else {
                return detail::tag_stream<Output, true>(
                    stage_.left().run(std::move(in)));
            }
        }
        if constexpr (SAME_OUTPUT) {
            return stage_.right().run(std::move(in));
        } else {
            return detail::tag_stream<Output, false>(
                stage_.right().run(std::move(in)));
        }
    }

   private:
    dstd::Either<A, B> stage_;
};

namespace detail {
template <class A, class B>
EitherStage<A, B> to_either_stage(dstd::Either<A, B> stage) {
    return EitherStage<A, B>(std::move(stage));
}
}  // namespace detail

/// A stage as a stage value: an @ref EitherStage for an Either of stages, a
/// copy of any other stage.
template <class S>
    requires(AnyStage<std::remove_cvref_t<S>> || StageEither<S>)
auto as_stage(S&& stage) {
    if constexpr (StageEither<S>) {
        return detail::to_either_stage(std::forward<S>(stage));
    } else {
        return std::remove_cvref_t<S>(std::forward<S>(stage));
    }
}

namespace detail {

template <class T>
struct AlwaysFalse : std::false_type {};

template <class N, class V>
decltype(auto) call_next(N& next, V&& value) {
    if constexpr (BatchStage<N>) {
        return next.run(std::forward<V>(value));
    } else {
        return std::invoke(next, std::forward<V>(value));
    }
}

template <class N, class T>
using NextOutput = typename UnwrapTask<decltype(call_next(
    std::declval<N&>(), std::declval<T>()))>::type;

template <class T, class N>
CoroTask<NextOutput<N, T>> then_task(CoroTask<T> source, N next) {
    T value = co_await std::move(source);
    using R = decltype(call_next(next, std::move(value)));
    if constexpr (is_coro_task_v<R>) {
        co_return co_await call_next(next, std::move(value));
    } else {
        co_return call_next(next, std::move(value));
    }
}

template <class Out, class N, class In>
CoroTask<Out> call_task(N next, In input) {
    using R = decltype(call_next(next, std::move(input)));
    if constexpr (is_coro_task_v<R>) {
        co_return co_await call_next(next, std::move(input));
    } else {
        co_return call_next(next, std::move(input));
    }
}

template <class N, class T>
AsyncGenerator<typename N::Output> run_stream_stage(N stage,
                                                    AsyncGenerator<T> source) {
    auto out = stage.run(std::move(source));
    for (;;) {
        auto item = co_await out.next();
        if (!item) break;
        co_yield std::move(*item);
    }
}

template <class N>
struct StageFn {
    N stage;

    auto operator()(typename N::Input value) const {
        return stage.run(std::move(value));
    }
};

template <class F, class In>
struct CallStage {
    using Input = In;
    using Output = NextOutput<F, In>;
    static constexpr const char* NAME = "callable";

    F fn;

    CoroTask<Output> run(In in) const {
        return call_task<Output>(fn, std::move(in));
    }
};

template <class A, class B>
CoroTask<typename B::Output> compose_run(const A& first, const B& second,
                                         typename A::Input in) {
    auto mid = co_await first.run(std::move(in));
    co_return co_await second.run(std::move(mid));
}

}  // namespace detail

/// Two stages run one after the other. Named after the first.
template <class A, class B>
class ComposedStage {
    static_assert(!(BatchStage<A> && StreamStage<B>),
                  "a batch stage cannot feed a stream stage: put the stream "
                  "stage first, or collect the batch result into one value");
    static_assert(std::is_same_v<typename A::Output, typename B::Input>,
                  "the second stage must take the output of the first");

   public:
    using Input = typename A::Input;
    using Output = typename B::Output;

    ComposedStage(A first, B second)
        : first_(std::move(first)), second_(std::move(second)) {}

    dstd::StringView name() const { return stage_name(first_); }

    CoroTask<Output> run(Input in) const
        requires(BatchStage<A> && BatchStage<B>)
    {
        return detail::compose_run(first_, second_, std::move(in));
    }

    AsyncGenerator<Output> run(AsyncGenerator<Input> in) const
        requires(StreamStage<A>)
    {
        if constexpr (StreamStage<B>) {
            return second_.run(first_.run(std::move(in)));
        } else {
            return coro::map(first_.run(std::move(in)),
                             detail::StageFn<B>{second_});
        }
    }

   private:
    A first_;
    B second_;
};

namespace detail {

// An adaptor can apply many times, so a move-only stage is shared by its uses.
template <class S>
class SharedStage {
   public:
    using Input = typename S::Input;
    using Output = typename S::Output;

    explicit SharedStage(S stage)
        : stage_(dstd::make_shared<S>(std::move(stage))) {}

    dstd::StringView name() const { return stage_name(*stage_); }

    auto run(Input in) const
        requires(BatchStage<S>)
    {
        return stage_->run(std::move(in));
    }

    auto run(AsyncGenerator<Input> in) const
        requires(StreamStage<S>)
    {
        return stage_->run(std::move(in));
    }

   private:
    dstd::SharedPtr<S> stage_;
};

template <class N>
auto hold_next(N&& next) {
    using ND = std::remove_cvref_t<N>;
    if constexpr ((AnyStage<ND> || StageEither<ND>) &&
                  !std::is_copy_constructible_v<ND>) {
        auto stage = as_stage(std::forward<N>(next));
        return SharedStage<decltype(stage)>(std::move(stage));
    } else {
        return ND(std::forward<N>(next));
    }
}

template <class N>
auto prepare_next(const N& next) {
    if constexpr (StageEither<N> || AnyStage<N>) {
        return as_stage(next);
    } else {
        return N(next);
    }
}

template <class X, class N>
auto apply_then(X&& x, const N& next) {
    using XD = std::remove_cvref_t<X>;
    auto n = prepare_next(next);
    using ND = decltype(n);
    if constexpr (is_coro_task_v<XD>) {
        static_assert(!StreamStage<ND>,
                      "a task gives one value: then needs a batch stage or a "
                      "function, not a stream stage");
        return then_task(std::forward<X>(x), std::move(n));
    } else if constexpr (is_async_generator_v<XD>) {
        if constexpr (StreamStage<ND>) {
            return run_stream_stage(std::move(n), std::forward<X>(x));
        } else if constexpr (BatchStage<ND>) {
            return coro::map(std::forward<X>(x), StageFn<ND>{std::move(n)});
        } else {
            return coro::map(std::forward<X>(x), std::move(n));
        }
    } else if constexpr (is_generator_v<XD>) {
        static_assert(!AnyStage<ND>,
                      "a sync Generator cannot wait: then needs a function");
        return coro::map(std::forward<X>(x), std::move(n));
    } else if constexpr (AnyStage<XD> || StageEither<XD>) {
        auto first = as_stage(std::forward<X>(x));
        using A = decltype(first);
        if constexpr (AnyStage<ND>) {
            return ComposedStage<A, ND>(std::move(first), std::move(n));
        } else {
            return ComposedStage<A, CallStage<ND, typename A::Output>>(
                std::move(first),
                CallStage<ND, typename A::Output>{std::move(n)});
        }
    } else {
        static_assert(AlwaysFalse<X>::value,
                      "then applies to a CoroTask, a generator or a stage");
    }
}

}  // namespace detail

/**
 * @brief Adaptor that feeds the value, the items or the output of what it is
 * applied to into @p next.
 *
 * @p next is a stage or a callable that returns a value or a @ref CoroTask.
 * On a @ref CoroTask it gives a @ref CoroTask. On an @ref AsyncGenerator it
 * runs a batch stage or a callable per item, or a stream stage on the whole
 * stream. On a stage it gives a composed stage, named after the first stage:
 * batch then batch is batch, stream then stream is stream, stream then batch
 * is stream, and batch then stream does not compile.
 */
template <class N>
auto then(N next) {
    return detail::make_adaptor(
        [next = detail::hold_next(std::move(next))](auto&& x) {
            return detail::apply_then(std::forward<decltype(x)>(x), next);
        });
}

/// @ref then applied to @p x.
template <class X, class N>
auto then(X&& x, N&& next) {
    return detail::apply_then(std::forward<X>(x),
                              detail::hold_next(std::forward<N>(next)));
}

namespace detail {

template <class T, class F>
CoroTask<T> recover_task(CoroTask<T> task, F handler) {
    std::exception_ptr failure;
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(task);
            co_return;
        } else {
            T value = co_await std::move(task);
            co_return value;
        }
    } catch (...) {
        failure = std::current_exception();
    }
    if (is_cancelled_error(failure)) common::rethrow_and_clear(failure);
    dstd::Error error = error_from_exception(failure);
    failure = nullptr;
    using R = std::invoke_result_t<F&, dstd::Error>;
    if constexpr (is_coro_task_v<R>) {
        co_return co_await handler(std::move(error));
    } else {
        co_return handler(std::move(error));
    }
}

template <class T>
CoroTask<T> limit_task(dstd::SharedPtr<AsyncSemaphore> slots,
                       CoroTask<T> task) {
    auto permit = co_await slots->permit();
    co_return co_await std::move(task);
}

template <class T>
AsyncGenerator<T> limit_stream(dstd::SharedPtr<AsyncSemaphore> slots,
                               AsyncGenerator<T> source) {
    auto permit = co_await slots->permit();
    for (;;) {
        auto item = co_await source.next();
        if (!item) break;
        co_yield std::move(*item);
    }
}

struct NamedPolicy {
    static constexpr bool STREAMS = true;
    dstd::StringView label;

    template <class S>
    dstd::StringView name(const S&) const {
        return label;
    }
    template <class S>
    auto batch(const S& s, typename S::Input in) const {
        return s.run(std::move(in));
    }
    template <class S>
    auto stream(const S& s, AsyncGenerator<typename S::Input> in) const {
        return s.run(std::move(in));
    }
    template <class X>
    auto direct(X&& x) const {
        return coro::named(label, std::forward<X>(x));
    }
};

template <class F>
struct RecoverPolicy {
    static constexpr bool STREAMS = false;
    F handler;

    template <class S>
    dstd::StringView name(const S& s) const {
        return stage_name(s);
    }
    template <class S>
    auto batch(const S& s, typename S::Input in) const {
        return recover_task(s.run(std::move(in)), handler);
    }
    template <class T>
    auto direct(CoroTask<T> task) const {
        return recover_task(std::move(task), handler);
    }
};

template <class Rep, class Period>
struct TimeoutPolicy {
    static constexpr bool STREAMS = true;
    std::chrono::duration<Rep, Period> wait;

    template <class S>
    dstd::StringView name(const S& s) const {
        return stage_name(s);
    }
    template <class S>
    auto batch(const S& s, typename S::Input in) const {
        return with_timeout(s.run(std::move(in)), wait);
    }
    template <class S>
    auto stream(const S& s, AsyncGenerator<typename S::Input> in) const {
        return with_item_timeout(s.run(std::move(in)), wait);
    }
    template <class T>
    auto direct(CoroTask<T> task) const {
        return with_timeout(std::move(task), wait);
    }
    template <class T>
    auto direct(AsyncGenerator<T> source) const {
        return with_item_timeout(std::move(source), wait);
    }
};

struct LimitPolicy {
    static constexpr bool STREAMS = true;
    dstd::SharedPtr<AsyncSemaphore> slots;

    template <class S>
    dstd::StringView name(const S& s) const {
        return stage_name(s);
    }
    template <class S>
    auto batch(const S& s, typename S::Input in) const {
        return limit_task(slots, s.run(std::move(in)));
    }
    template <class S>
    auto stream(const S& s, AsyncGenerator<typename S::Input> in) const {
        return limit_stream(slots, s.run(std::move(in)));
    }
    template <class T>
    auto direct(CoroTask<T> task) const {
        return limit_task(slots, std::move(task));
    }
    template <class T>
    auto direct(AsyncGenerator<T> source) const {
        return limit_stream(slots, std::move(source));
    }
};

template <class S, class W>
class WrappedStage {
   public:
    using Input = typename S::Input;
    using Output = typename S::Output;

    WrappedStage(S inner, W policy)
        : inner_(std::move(inner)), policy_(std::move(policy)) {}

    dstd::StringView name() const { return policy_.name(inner_); }

    CoroTask<Output> run(Input in) const
        requires(BatchStage<S>)
    {
        return policy_.batch(inner_, std::move(in));
    }

    AsyncGenerator<Output> run(AsyncGenerator<Input> in) const
        requires(StreamStage<S> && W::STREAMS)
    {
        return policy_.stream(inner_, std::move(in));
    }

   private:
    S inner_;
    W policy_;
};

template <class X, class W>
auto apply_wrap(X&& x, W policy) {
    using XD = std::remove_cvref_t<X>;
    if constexpr (StageEither<XD>) {
        return apply_wrap(as_stage(std::forward<X>(x)), std::move(policy));
    } else if constexpr (AnyStage<XD>) {
        static_assert(BatchStage<XD> || W::STREAMS,
                      "this adaptor applies to batch stages, not stream "
                      "stages");
        return WrappedStage<XD, W>(std::forward<X>(x), std::move(policy));
    } else {
        static_assert(!is_async_generator_v<XD> || W::STREAMS,
                      "this adaptor does not apply to an AsyncGenerator");
        return policy.direct(std::forward<X>(x));
    }
}

}  // namespace detail

/**
 * @brief Adaptor that runs @p handler when what it is applied to fails.
 *
 * Applies to a @ref CoroTask and to a batch stage. @p handler takes the
 * dstd::Error and returns the value or a @ref CoroTask of it. A cancelled
 * task stays cancelled and never reaches @p handler.
 */
template <class F>
auto recover(F handler) {
    return detail::make_adaptor([handler = std::move(handler)](auto&& x) {
        return detail::apply_wrap(std::forward<decltype(x)>(x),
                                  detail::RecoverPolicy<F>{handler});
    });
}

/**
 * @brief A stage that reports @p label as its name and is otherwise @p stage.
 *
 * The label is borrowed, so it must outlive every run. It wins over the
 * generic @ref named, which wraps a task.
 */
template <class S>
    requires(AnyStage<std::remove_cvref_t<S>> || StageEither<S>)
auto named(dstd::StringView label, S&& stage) {
    return detail::apply_wrap(std::forward<S>(stage),
                              detail::NamedPolicy{label});
}

/// Adaptor form of @ref named. On a stage it renames it; on anything else it
/// is the generic @ref named.
inline auto named(dstd::StringView label) {
    return detail::make_adaptor([label](auto&& x) {
        return detail::apply_wrap(std::forward<decltype(x)>(x),
                                  detail::NamedPolicy{label});
    });
}

/**
 * @brief Adaptor that ends a run with a dstd::Condition::Timeout error when
 * @p wait passes.
 *
 * On a @ref CoroTask and a batch stage it bounds the run, as @ref with_timeout.
 * On an @ref AsyncGenerator and a stream stage it bounds the wait for each
 * item, as @ref with_item_timeout.
 */
template <class Rep, class Period>
auto timeout(std::chrono::duration<Rep, Period> wait) {
    return detail::make_adaptor([wait](auto&& x) {
        return detail::apply_wrap(std::forward<decltype(x)>(x),
                                  detail::TimeoutPolicy<Rep, Period>{wait});
    });
}

/**
 * @brief Adaptor that lets at most @p runs of the things it is applied to run
 * at once, across all copies of the adaptor and everything built from them.
 *
 * A stream holds its slot until it ends, fails or is dropped. Raises
 * dstd::Condition::InvalidArgument when @p runs is 0.
 */
inline auto limit(std::size_t runs) {
    if (runs == 0) {
        throw common::DFTUtilsException(dstd::Error{
            dstd::Condition::InvalidArgument, "limit needs at least one run"});
    }
    auto slots = dstd::make_shared<AsyncSemaphore>(runs);
    return detail::make_adaptor([slots](auto&& x) {
        return detail::apply_wrap(std::forward<decltype(x)>(x),
                                  detail::LimitPolicy{slots});
    });
}

namespace detail {

template <class S, bool ORDERED>
class ParallelStage {
    static_assert(BatchStage<S>,
                  "ordered and unordered apply to a batch stage");

   public:
    using Input = typename S::Input;
    using Output = typename S::Output;

    ParallelStage(S stage, std::size_t tokens)
        : stage_(std::move(stage)), tokens_(tokens) {}

    dstd::StringView name() const { return stage_.name(); }

    AsyncGenerator<Output> run(AsyncGenerator<Input> in) const {
        StageFn<SharedStage<S>> fn{stage_};
        if constexpr (ORDERED) {
            return map_ordered(std::move(in), std::move(fn), tokens_);
        } else {
            return map_unordered(std::move(in), std::move(fn), tokens_);
        }
    }

   private:
    SharedStage<S> stage_;
    std::size_t tokens_;
};

template <bool ORDERED>
auto parallel_adaptor(std::size_t tokens) {
    if (tokens == 0) {
        throw common::DFTUtilsException(
            dstd::Error{dstd::Condition::InvalidArgument,
                        "a stage needs at least 1 token"});
    }
    return make_adaptor([tokens](auto&& x) {
        auto stage = as_stage(std::forward<decltype(x)>(x));
        return ParallelStage<decltype(stage), ORDERED>(std::move(stage),
                                                       tokens);
    });
}

}  // namespace detail

/**
 * @brief Adaptor that turns a batch stage into a stream stage that runs it on
 * up to @p tokens items at once and keeps the input order.
 *
 * @p tokens bounds the running items plus the results that wait for an earlier
 * one. See @ref map_ordered. Raises dstd::Condition::InvalidArgument on 0.
 */
inline auto ordered(std::size_t tokens) {
    return detail::parallel_adaptor<true>(tokens);
}

/// @ref ordered that yields results as they finish. See @ref map_unordered.
inline auto unordered(std::size_t tokens) {
    return detail::parallel_adaptor<false>(tokens);
}

/**
 * @brief Adaptor that applies @p adaptor when @p condition is true and passes
 * the value through otherwise.
 *
 * @p adaptor must give back the type it is given.
 */
template <class A>
auto maybe(bool condition, A adaptor) {
    return detail::make_adaptor(
        [condition, adaptor = std::move(adaptor)](auto&& x) {
            using X = std::remove_cvref_t<decltype(x)>;
            using R = std::remove_cvref_t<decltype(adaptor(
                std::forward<decltype(x)>(x)))>;
            static_assert(std::is_same_v<R, X>,
                          "maybe needs an adaptor that gives back the type it "
                          "is given");
            if (condition) return X(adaptor(std::forward<decltype(x)>(x)));
            return X(std::forward<decltype(x)>(x));
        });
}

/// Holds @p a when @p condition is true and @p b otherwise. The other value is
/// dropped.
template <class A, class B>
dstd::Either<A, B> either(bool condition, A a, B b) {
    if (condition) return dstd::Either<A, B>::left(std::move(a));
    return dstd::Either<A, B>::right(std::move(b));
}

/// A batch stage with its type erased. Move-only. @ref name forwards to the
/// stage it holds.
template <class In, class Out>
class DynStage {
    struct Base {
        virtual ~Base() = default;
        virtual CoroTask<Out> run(In in) const = 0;
        virtual dstd::StringView name() const = 0;
    };

    template <class S>
    struct Impl final : Base {
        explicit Impl(S s) : stage(std::move(s)) {}
        CoroTask<Out> run(In in) const override {
            return stage.run(std::move(in));
        }
        dstd::StringView name() const override { return stage_name(stage); }
        S stage;
    };

   public:
    using Input = In;
    using Output = Out;

    template <class S>
        requires(BatchStage<S> && std::same_as<typename S::Input, In> &&
                 std::same_as<typename S::Output, Out> &&
                 !std::same_as<S, DynStage>)
    explicit DynStage(S stage)
        : impl_(dstd::make_unique<Impl<S>>(std::move(stage))) {}

    dstd::StringView name() const { return impl_->name(); }
    CoroTask<Out> run(In in) const { return impl_->run(std::move(in)); }

   private:
    dstd::UniquePtr<Base> impl_;
};

/// A stream stage with its type erased. Move-only.
template <class In, class Out>
class DynStreamStage {
    struct Base {
        virtual ~Base() = default;
        virtual AsyncGenerator<Out> run(AsyncGenerator<In> in) const = 0;
        virtual dstd::StringView name() const = 0;
    };

    template <class S>
    struct Impl final : Base {
        explicit Impl(S s) : stage(std::move(s)) {}
        AsyncGenerator<Out> run(AsyncGenerator<In> in) const override {
            return stage.run(std::move(in));
        }
        dstd::StringView name() const override { return stage_name(stage); }
        S stage;
    };

   public:
    using Input = In;
    using Output = Out;

    template <class S>
        requires(StreamStage<S> && std::same_as<typename S::Input, In> &&
                 std::same_as<typename S::Output, Out> &&
                 !std::same_as<S, DynStreamStage>)
    explicit DynStreamStage(S stage)
        : impl_(dstd::make_unique<Impl<S>>(std::move(stage))) {}

    dstd::StringView name() const { return impl_->name(); }
    AsyncGenerator<Out> run(AsyncGenerator<In> in) const {
        return impl_->run(std::move(in));
    }

   private:
    dstd::UniquePtr<Base> impl_;
};

/// @p stage as a @ref DynStage or a @ref DynStreamStage of its kind.
template <class S>
    requires(AnyStage<std::remove_cvref_t<S>> || StageEither<S>)
auto erase(S&& stage) {
    auto held = as_stage(std::forward<S>(stage));
    using H = decltype(held);
    if constexpr (BatchStage<H>) {
        return DynStage<typename H::Input, typename H::Output>(std::move(held));
    } else {
        return DynStreamStage<typename H::Input, typename H::Output>(
            std::move(held));
    }
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_STAGE_H
