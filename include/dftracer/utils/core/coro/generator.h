#ifndef DFTRACER_UTILS_CORE_CORO_GENERATOR_H
#define DFTRACER_UTILS_CORE_CORO_GENERATOR_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/exception_helpers.h>

#include <coroutine>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace dftracer::utils::coro {

/**
 * Generator<T> - Synchronous lazy sequence generator
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
class Generator {
   public:
    /**
     * Promise type for coroutine
     */
    struct promise_type {
        std::optional<T> current_value_;
        std::exception_ptr exception_;

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
            current_value_ = std::move(value);
            return {};
        }

        /**
         * Generator completed
         */
        void return_void() noexcept {}

        /**
         * Capture exception
         */
        void unhandled_exception() { exception_ = std::current_exception(); }
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
                if (handle_.promise().exception_) {
                    rethrow_and_clear(handle_.promise().exception_);
                }
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
            if (!handle_ || !handle_.promise().current_value_) {
                throw DFTUtilsException(
                    ErrorCode::INVALID_ARGUMENT,
                    "Generator iterator: no value available");
            }
            return *handle_.promise().current_value_;
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

        if (handle_.promise().exception_) {
            rethrow_and_clear(handle_.promise().exception_);
        }

        if (handle_.done()) {
            return iterator{nullptr};
        }

        return iterator{handle_};
    }

    /**
     * End iterator
     */
    iterator end() { return iterator{nullptr}; }

    /**
     * Manual control: advance to next value
     * @return true if value available, false if done
     */
    bool next() {
        if (!handle_ || handle_.done()) {
            return false;
        }

        handle_.resume();

        if (handle_.promise().exception_) {
            rethrow_and_clear(handle_.promise().exception_);
        }

        return !handle_.done();
    }

    /**
     * Get current value (call after next() returns true)
     * @throws std::runtime_error if no value available
     */
    const T& value() const {
        if (!handle_ || !handle_.promise().current_value_) {
            throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                    "Generator: no value available");
        }
        return *handle_.promise().current_value_;
    }

    /**
     * Check if generator is done
     */
    bool done() const { return !handle_ || handle_.done(); }

    /**
     * Check if generator has pending exception
     */
    bool has_exception() const {
        return handle_ && handle_.promise().exception_ != nullptr;
    }

    /**
     * Rethrow pending exception
     */
    void rethrow_if_exception() {
        if (handle_ && handle_.promise().exception_) {
            rethrow_and_clear(handle_.promise().exception_);
        }
    }
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_GENERATOR_H
