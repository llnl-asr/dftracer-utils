// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_GENERATOR_H
#define DFTRACER_UTILS_CORO_GENERATOR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/detail/frame.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/optional.h>

#include <coroutine>
#include <iterator>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

/**
 * Generator<T> - Synchronous lazy sequence generator
 *
 * The operations on it are in `coro/stream.h`, with the names that
 * @ref AsyncGenerator uses. A body cannot fail: it yields items, and an item
 * that can fail is a dstd::Result.
 *
 * Usage:
 * @code
 * Generator<int> fibonacci(int n) {
 *     int a = 0, b = 1;
 *     for (int i = 0; i < n; ++i) {
 *         co_yield a;
 *         auto next = a + b;
 *         a = b;
 *         b = next;
 *     }
 * }
 *
 * for (int value : fibonacci(10)) {
 *     std::cout << value << " ";
 * }
 * @endcode
 */
template <typename T>
class Generator;

namespace detail {
template <typename T>
struct is_generator : std::false_type {};
template <typename T>
struct is_generator<Generator<T>> : std::true_type {};
template <typename T>
inline constexpr bool is_generator_v = is_generator<T>::value;
}  // namespace detail

template <typename T>
class Generator {
   public:
    using value_type = T;

    /**
     * Promise type for coroutine
     */
    struct promise_type : detail::FrameAllocation {
        dstd::Optional<T> current_value;

        /**
         * Create Generator from promise
         */
        Generator get_return_object() {
            return Generator{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        /**
         * Suspend at start
         */
        std::suspend_always initial_suspend() noexcept { return {}; }

        /**
         * Suspend at end
         */
        std::suspend_always final_suspend() noexcept { return {}; }

        /**
         * Store yielded value
         */
        std::suspend_always yield_value(T value) {
            current_value.emplace(std::move(value));
            return {};
        }

        /**
         * Generator completed
         */
        void return_void() noexcept {}

        void unhandled_exception() noexcept {
            dstd::fatal("coro: an exception escaped a coroutine");
        }
    };

    /**
     * Iterator
     */
    class iterator {
       private:
        std::coroutine_handle<promise_type> handle_;

       public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        /**
         * Construct iterator from coroutine handle
         */
        explicit iterator(std::coroutine_handle<promise_type> handle)
            : handle_(handle) {}

        /**
         * Advance to next value
         */
        iterator& operator++() {
            if (handle_ && !handle_.done()) {
                handle_.resume();
            }
            return *this;
        }

        /**
         * Post-increment
         */
        iterator operator++(int) {
            iterator temp = *this;
            ++(*this);
            return temp;
        }

        /**
         * Dereference iterator
         */
        reference operator*() const {
            if (!handle_ || !handle_.promise().current_value) {
                dstd::fatal("coro: a generator iterator has no value");
            }
            return *handle_.promise().current_value;
        }

        /**
         * Member access operator
         */
        pointer operator->() const { return &(operator*()); }

        /**
         * Compare iterators
         */
        bool operator==(const iterator& other) const {
            // Both done or both same handle
            if (!handle_ || handle_.done()) {
                return !other.handle_ || other.handle_.done();
            }
            return handle_ == other.handle_;
        }

        bool operator!=(const iterator& other) const {
            return !(*this == other);
        }
    };

   private:
    std::coroutine_handle<promise_type> handle_;

   public:
    /**
     * Construct Generator from coroutine handle
     */
    explicit Generator(std::coroutine_handle<promise_type> handle)
        : handle_(handle) {}

    /**
     * Destructor - clean up coroutine state
     */
    ~Generator() {
        if (handle_) {
            handle_.destroy();
        }
    }

    /**
     * Move-only semantics
     */
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;

    Generator(Generator&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Generator& operator=(Generator&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    /**
     * Begin iterator
     */
    iterator begin() {
        if (!handle_) {
            return iterator{nullptr};
        }

        handle_.resume();

        if (handle_.done()) {
            return iterator{nullptr};
        }

        return iterator{handle_};
    }

    /**
     * End iterator
     */
    iterator end() { return iterator{nullptr}; }

    /// The next item, or nothing at the end, as @ref AsyncGenerator gives it.
    /// The item moves out, so a move-only type works.
    dstd::Optional<T> next() {
        if (!handle_ || handle_.done()) {
            return dstd::nullopt;
        }
        handle_.resume();
        if (handle_.done()) {
            return dstd::nullopt;
        }
        auto& current = handle_.promise().current_value;
        dstd::Optional<T> out(std::move(*current));
        current.reset();
        return out;
    }

    bool done() const { return !handle_ || handle_.done(); }
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_GENERATOR_H
