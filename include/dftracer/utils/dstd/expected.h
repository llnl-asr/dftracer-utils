// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_EXPECTED_H
#define DFTRACER_UTILS_DSTD_EXPECTED_H

#include <dftracer/utils/dstd/fatal.h>

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

namespace dftracer::utils::dstd {

/// An error on its way into an @ref Expected. `Unexpected(x)` deduces the type.
template <class E>
class Unexpected {
   public:
    template <class G = E>
        requires(std::is_constructible_v<E, G &&> &&
                 !std::is_same_v<std::remove_cvref_t<G>, Unexpected>)
    constexpr explicit Unexpected(G&& error) : error_(std::forward<G>(error)) {}

    constexpr const E& error() const& noexcept { return error_; }
    constexpr E& error() & noexcept { return error_; }
    constexpr E&& error() && noexcept { return std::move(error_); }

   private:
    E error_;
};

template <class E>
Unexpected(E) -> Unexpected<E>;

template <class T, class E>
class Expected;

/// What `co_await r.or_fail()` awaits; a coroutine task turns it into a wait
/// that ends the task with the error.
template <class T, class E>
struct OrFail {
    Expected<T, E> result;
};

namespace detail {

template <class T>
inline constexpr bool is_unexpected_v = false;
template <class E>
inline constexpr bool is_unexpected_v<Unexpected<E>> = true;

template <class E>
[[noreturn]] void expected_fatal(const char* what, const E& error) noexcept {
    if constexpr (requires { error.format().c_str(); }) {
        const auto text = error.format();
        fatal(what, text.c_str());
    } else {
        fatal(what);
    }
}

/// Reads the state without counting as a check, for code that hands the
/// result on to the one who must check it.
struct ExpectedAccess {
    template <class R>
    static bool holds_value(const R& result) noexcept {
        return result.has_;
    }
};

}  // namespace detail

/**
 * @brief A value of type `T`, or the error of type `E` that kept it from being
 * made.
 *
 * The result must be checked: test it (`if (r)`, has_value()), read its error,
 * @ref match it or pass it on with `or_fail`. A build with
 * `DFTRACER_UTILS_EXPECTED_CHECKS` aborts through @ref fatal when an error
 * result is destroyed or overwritten unchecked, and on `*r` of an error.
 * value() and error() on the wrong side always end through @ref fatal; they
 * never throw. A copy or move must be checked again. The moved-from object
 * needs no check.
 *
 * It converts from an `Unexpected<G>` and, when `T` cannot be made from `E`,
 * from a bare `E`, so `if (!r) return r.error();` passes an error on.
 */
template <class T, class E>
class [[nodiscard]] Expected {
    static_assert(!std::is_reference_v<T> && !std::is_reference_v<E>);
    static_assert(!detail::is_unexpected_v<std::remove_cv_t<T>>);

   public:
    using value_type = T;
    using error_type = E;

    Expected()
        requires std::is_default_constructible_v<T>
        : has_(true) {
        std::construct_at(&storage_.value);
    }

    template <class U = T>
        requires(std::is_constructible_v<T, U &&> &&
                 !std::is_same_v<std::remove_cvref_t<U>, Expected> &&
                 !detail::is_unexpected_v<std::remove_cvref_t<U>>)
    explicit(!std::is_convertible_v<U&&, T>) Expected(U&& value) : has_(true) {
        std::construct_at(&storage_.value, std::forward<U>(value));
    }

    Expected(const E& error)
        requires(!std::is_constructible_v<T, const E&>)
        : has_(false) {
        std::construct_at(&storage_.error, error);
    }

    Expected(E&& error)
        requires(!std::is_constructible_v<T, E &&>)
        : has_(false) {
        std::construct_at(&storage_.error, std::move(error));
    }

    template <class G>
        requires std::is_constructible_v<E, const G&>
    Expected(const Unexpected<G>& error) : has_(false) {
        std::construct_at(&storage_.error, error.error());
    }

    template <class G>
        requires std::is_constructible_v<E, G&&>
    Expected(Unexpected<G>&& error) : has_(false) {
        std::construct_at(&storage_.error, std::move(error).error());
    }

    Expected(const Expected& other)
        requires(std::is_copy_constructible_v<T> &&
                 std::is_copy_constructible_v<E>)
        : has_(other.has_) {
        if (has_) {
            std::construct_at(&storage_.value, other.storage_.value);
        } else {
            std::construct_at(&storage_.error, other.storage_.error);
        }
    }

    Expected(Expected&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> &&
        std::is_nothrow_move_constructible_v<E>)
        requires(std::is_move_constructible_v<T> &&
                 std::is_move_constructible_v<E>)
        : has_(other.has_) {
        if (has_) {
            std::construct_at(&storage_.value, std::move(other.storage_.value));
        } else {
            std::construct_at(&storage_.error, std::move(other.storage_.error));
        }
        other.checked_.store(true, std::memory_order_relaxed);
    }

    /// Copies first, so a copy that throws leaves this result as it was.
    Expected& operator=(const Expected& other)
        requires(std::is_copy_constructible_v<T> &&
                 std::is_copy_constructible_v<E> &&
                 std::is_nothrow_move_constructible_v<T> &&
                 std::is_nothrow_move_constructible_v<E>)
    {
        if (this != &other) {
            Expected copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    /// Needs moves that cannot throw: the old side is gone before the new
    /// one is made.
    Expected& operator=(Expected&& other) noexcept
        requires(std::is_nothrow_move_constructible_v<T> &&
                 std::is_nothrow_move_constructible_v<E>)
    {
        if (this != &other) {
            release();
            has_ = other.has_;
            checked_.store(false, std::memory_order_relaxed);
            if (has_) {
                std::construct_at(&storage_.value,
                                  std::move(other.storage_.value));
            } else {
                std::construct_at(&storage_.error,
                                  std::move(other.storage_.error));
            }
            other.checked_.store(true, std::memory_order_relaxed);
        }
        return *this;
    }

    ~Expected() { release(); }

    bool has_value() const noexcept {
        mark_checked();
        return has_;
    }
    /// Absent for a bool value, where `if (result)` would read as the value
    /// and mean the state: use has_value().
    explicit operator bool() const noexcept
        requires(!std::is_same_v<std::remove_cv_t<T>, bool>)
    {
        return has_value();
    }

    /// Marks the result as handled with nothing to do, for a caller that may
    /// drop an error. Say why at the call.
    void discard() const noexcept { mark_checked(); }

    T& operator*() & noexcept {
        check_value_access();
        return storage_.value;
    }
    const T& operator*() const& noexcept {
        check_value_access();
        return storage_.value;
    }
    T&& operator*() && noexcept {
        check_value_access();
        return std::move(storage_.value);
    }
    T* operator->() noexcept {
        check_value_access();
        return std::addressof(storage_.value);
    }
    const T* operator->() const noexcept {
        check_value_access();
        return std::addressof(storage_.value);
    }

    T& value() & noexcept {
        require_value();
        return storage_.value;
    }
    const T& value() const& noexcept {
        require_value();
        return storage_.value;
    }
    T&& value() && noexcept {
        require_value();
        return std::move(storage_.value);
    }

    E& error() & noexcept {
        require_error();
        return storage_.error;
    }
    const E& error() const& noexcept {
        require_error();
        return storage_.error;
    }
    E&& error() && noexcept {
        require_error();
        return std::move(storage_.error);
    }

    template <class U>
    T value_or(U&& fallback) const& {
        return has_value() ? storage_.value
                           : static_cast<T>(std::forward<U>(fallback));
    }
    template <class U>
    T value_or(U&& fallback) && {
        return has_value() ? std::move(storage_.value)
                           : static_cast<T>(std::forward<U>(fallback));
    }

    /// Calls `on_value` with the value or `on_error` with the error. Both are
    /// required and must return the same type.
    template <class OnValue, class OnError>
    decltype(auto) match(OnValue&& on_value, OnError&& on_error) & {
        return match_impl(*this, std::forward<OnValue>(on_value),
                          std::forward<OnError>(on_error));
    }
    template <class OnValue, class OnError>
    decltype(auto) match(OnValue&& on_value, OnError&& on_error) const& {
        return match_impl(*this, std::forward<OnValue>(on_value),
                          std::forward<OnError>(on_error));
    }
    template <class OnValue, class OnError>
    decltype(auto) match(OnValue&& on_value, OnError&& on_error) && {
        return match_impl(std::move(*this), std::forward<OnValue>(on_value),
                          std::forward<OnError>(on_error));
    }

    /// The result of `fn(value)` as an Expected with the same error.
    template <class F>
    auto transform(F&& fn) && {
        using U = std::remove_cv_t<std::invoke_result_t<F, T&&>>;
        if (!has_value())
            return Expected<U, E>(Unexpected(std::move(storage_.error)));
        if constexpr (std::is_void_v<U>) {
            std::forward<F>(fn)(std::move(storage_.value));
            return Expected<U, E>();
        } else {
            return Expected<U, E>(
                std::forward<F>(fn)(std::move(storage_.value)));
        }
    }

    /// `fn(value)`, which returns an Expected with the same error.
    template <class F>
    auto and_then(F&& fn) && {
        using R = std::remove_cvref_t<std::invoke_result_t<F, T&&>>;
        static_assert(std::is_same_v<typename R::error_type, E>,
                      "and_then needs a function that returns an Expected "
                      "with the same error type");
        if (!has_value()) return R(Unexpected(std::move(storage_.error)));
        return std::forward<F>(fn)(std::move(storage_.value));
    }

    /// `co_await std::move(r).or_fail()` in a task that returns an Expected
    /// gives the value or ends the task with the error.
    OrFail<T, E> or_fail() && { return {std::move(*this)}; }

   private:
    friend struct detail::ExpectedAccess;

    void mark_checked() const noexcept {
        if (!checked_.load(std::memory_order_relaxed))
            checked_.store(true, std::memory_order_relaxed);
    }

    template <class Self, class OnValue, class OnError>
    static decltype(auto) match_impl(Self&& self, OnValue&& on_value,
                                     OnError&& on_error) {
        using ValueRef = decltype((std::forward<Self>(self).storage_.value));
        using ErrorRef = decltype((std::forward<Self>(self).storage_.error));
        static_assert(std::is_invocable_v<OnValue, ValueRef>,
                      "match needs a handler for the value");
        static_assert(std::is_invocable_v<OnError, ErrorRef>,
                      "match needs a handler for the error");
        static_assert(std::is_same_v<std::invoke_result_t<OnValue, ValueRef>,
                                     std::invoke_result_t<OnError, ErrorRef>>,
                      "both match handlers must return the same type");
        if (self.has_value()) {
            return std::forward<OnValue>(on_value)(
                std::forward<Self>(self).storage_.value);
        }
        return std::forward<OnError>(on_error)(
            std::forward<Self>(self).storage_.error);
    }

    void check_value_access() const noexcept {
#ifdef DFTRACER_UTILS_EXPECTED_CHECKS
        if (!has_)
            detail::expected_fatal("operator* on an error result",
                                   storage_.error);
#endif
    }

    void require_value() const noexcept {
        mark_checked();
        if (!has_)
            detail::expected_fatal("value() on an error result",
                                   storage_.error);
    }

    void require_error() const noexcept {
        mark_checked();
        if (has_) fatal("error() on a result that holds a value");
    }

    void release() noexcept {
        if (has_) {
            std::destroy_at(&storage_.value);
            return;
        }
#ifdef DFTRACER_UTILS_EXPECTED_CHECKS
        if (!checked_.load(std::memory_order_relaxed))
            detail::expected_fatal("an error result was never checked",
                                   storage_.error);
#endif
        std::destroy_at(&storage_.error);
    }

    union Storage {
        Storage() noexcept {}
        ~Storage() {}
        T value;
        E error;
    };
    Storage storage_;
    bool has_;
    mutable std::atomic<bool> checked_{false};
};

/// An Expected with no value: success, or the error.
template <class E>
class [[nodiscard]] Expected<void, E> {
    static_assert(!std::is_reference_v<E>);

   public:
    using value_type = void;
    using error_type = E;

    Expected() noexcept : has_(true) {}

    Expected(const E& error) : has_(false) {
        std::construct_at(&storage_.error, error);
    }
    Expected(E&& error) : has_(false) {
        std::construct_at(&storage_.error, std::move(error));
    }

    template <class G>
        requires std::is_constructible_v<E, const G&>
    Expected(const Unexpected<G>& error) : has_(false) {
        std::construct_at(&storage_.error, error.error());
    }

    template <class G>
        requires std::is_constructible_v<E, G&&>
    Expected(Unexpected<G>&& error) : has_(false) {
        std::construct_at(&storage_.error, std::move(error).error());
    }

    Expected(const Expected& other)
        requires std::is_copy_constructible_v<E>
        : has_(other.has_) {
        if (!has_) std::construct_at(&storage_.error, other.storage_.error);
    }

    Expected(Expected&& other) noexcept(std::is_nothrow_move_constructible_v<E>)
        requires std::is_move_constructible_v<E>
        : has_(other.has_) {
        if (!has_)
            std::construct_at(&storage_.error, std::move(other.storage_.error));
        other.checked_.store(true, std::memory_order_relaxed);
    }

    Expected& operator=(const Expected& other)
        requires(std::is_copy_constructible_v<E> &&
                 std::is_nothrow_move_constructible_v<E>)
    {
        if (this != &other) {
            Expected copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    Expected& operator=(Expected&& other) noexcept
        requires std::is_nothrow_move_constructible_v<E>
    {
        if (this != &other) {
            release();
            has_ = other.has_;
            checked_.store(false, std::memory_order_relaxed);
            if (!has_)
                std::construct_at(&storage_.error,
                                  std::move(other.storage_.error));
            other.checked_.store(true, std::memory_order_relaxed);
        }
        return *this;
    }

    ~Expected() { release(); }

    bool has_value() const noexcept {
        mark_checked();
        return has_;
    }
    explicit operator bool() const noexcept { return has_value(); }

    /// Marks the result as handled with nothing to do, for a caller that may
    /// drop an error. Say why at the call.
    void discard() const noexcept { mark_checked(); }

    void operator*() const noexcept {
#ifdef DFTRACER_UTILS_EXPECTED_CHECKS
        if (!has_)
            detail::expected_fatal("operator* on an error result",
                                   storage_.error);
#endif
    }
    void value() const noexcept {
        mark_checked();
        if (!has_)
            detail::expected_fatal("value() on an error result",
                                   storage_.error);
    }

    E& error() & noexcept {
        require_error();
        return storage_.error;
    }
    const E& error() const& noexcept {
        require_error();
        return storage_.error;
    }
    E&& error() && noexcept {
        require_error();
        return std::move(storage_.error);
    }

    template <class OnValue, class OnError>
    decltype(auto) match(OnValue&& on_value, OnError&& on_error) & {
        return match_impl(*this, std::forward<OnValue>(on_value),
                          std::forward<OnError>(on_error));
    }
    template <class OnValue, class OnError>
    decltype(auto) match(OnValue&& on_value, OnError&& on_error) const& {
        return match_impl(*this, std::forward<OnValue>(on_value),
                          std::forward<OnError>(on_error));
    }
    template <class OnValue, class OnError>
    decltype(auto) match(OnValue&& on_value, OnError&& on_error) && {
        return match_impl(std::move(*this), std::forward<OnValue>(on_value),
                          std::forward<OnError>(on_error));
    }

    template <class F>
    auto transform(F&& fn) && {
        using U = std::remove_cv_t<std::invoke_result_t<F>>;
        if (!has_value())
            return Expected<U, E>(Unexpected(std::move(storage_.error)));
        if constexpr (std::is_void_v<U>) {
            std::forward<F>(fn)();
            return Expected<U, E>();
        } else {
            return Expected<U, E>(std::forward<F>(fn)());
        }
    }

    template <class F>
    auto and_then(F&& fn) && {
        using R = std::remove_cvref_t<std::invoke_result_t<F>>;
        static_assert(std::is_same_v<typename R::error_type, E>,
                      "and_then needs a function that returns an Expected "
                      "with the same error type");
        if (!has_value()) return R(Unexpected(std::move(storage_.error)));
        return std::forward<F>(fn)();
    }

    OrFail<void, E> or_fail() && { return {std::move(*this)}; }

   private:
    friend struct detail::ExpectedAccess;

    void mark_checked() const noexcept {
        if (!checked_.load(std::memory_order_relaxed))
            checked_.store(true, std::memory_order_relaxed);
    }

    template <class Self, class OnValue, class OnError>
    static decltype(auto) match_impl(Self&& self, OnValue&& on_value,
                                     OnError&& on_error) {
        using ErrorRef = decltype((std::forward<Self>(self).storage_.error));
        static_assert(std::is_invocable_v<OnValue>,
                      "match needs a handler for success");
        static_assert(std::is_invocable_v<OnError, ErrorRef>,
                      "match needs a handler for the error");
        static_assert(std::is_same_v<std::invoke_result_t<OnValue>,
                                     std::invoke_result_t<OnError, ErrorRef>>,
                      "both match handlers must return the same type");
        if (self.has_value()) return std::forward<OnValue>(on_value)();
        return std::forward<OnError>(on_error)(
            std::forward<Self>(self).storage_.error);
    }

    void require_error() const noexcept {
        mark_checked();
        if (has_) fatal("error() on a result that holds no error");
    }

    void release() noexcept {
        if (has_) return;
#ifdef DFTRACER_UTILS_EXPECTED_CHECKS
        if (!checked_.load(std::memory_order_relaxed))
            detail::expected_fatal("an error result was never checked",
                                   storage_.error);
#endif
        std::destroy_at(&storage_.error);
    }

    union Storage {
        Storage() noexcept {}
        ~Storage() {}
        E error;
    };
    Storage storage_;
    bool has_;
    mutable std::atomic<bool> checked_{false};
};

/// Marks `value` as handled when it is an @ref Expected or an optional that
/// holds one, for a container that drops what no reader took.
template <class V>
void discard_unread(const V& value) noexcept {
    if constexpr (requires { value.discard(); }) {
        value.discard();
    } else if constexpr (requires {
                             value.has_value();
                             (*value).discard();
                         }) {
        if (value.has_value()) (*value).discard();
    }
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_EXPECTED_H
