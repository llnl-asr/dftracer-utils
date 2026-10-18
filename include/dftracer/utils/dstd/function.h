#ifndef DFTRACER_UTILS_DSTD_FUNCTION_H
#define DFTRACER_UTILS_DSTD_FUNCTION_H

#include <dftracer/utils/memory/heap.h>

#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace dftracer::utils::dstd {

template <class Signature>
class Function;

/**
 * @brief A move-only std::function: 32 bytes, with a 24-byte inline buffer.
 *
 * A callable that fits the buffer with its alignment and is nothrow move
 * constructible stays inline; any other is placed by memory::allocate and
 * freed with the sized memory::deallocate. A moved-from Function is empty.
 * Calling an empty Function throws std::bad_function_call. operator() is
 * const and calls the target as non-const, as std::function does.
 */
template <class R, class... Args>
class Function<R(Args...)> {
   public:
    static constexpr std::size_t BUFFER_SIZE = 3 * sizeof(void*);

    Function() noexcept = default;
    Function(std::nullptr_t) noexcept {}

    template <class F, class = std::enable_if_t<
                           !std::is_same_v<std::decay_t<F>, Function> &&
                           std::is_invocable_r_v<R, std::decay_t<F>&, Args...>>>
    Function(F&& f) {
        using T = std::decay_t<F>;
        if constexpr (std::is_pointer_v<T> || std::is_member_pointer_v<T>) {
            if (f == nullptr) return;
        }
        if constexpr (fits_inline<T>()) {
            ::new (static_cast<void*>(buffer_)) T(std::forward<F>(f));
            ops_ = &INLINE_OPS<T>;
        } else {
            void* raw = memory::allocate(sizeof(T), alignof(T));
            try {
                ::new (raw) T(std::forward<F>(f));
            } catch (...) {
                memory::deallocate(raw, sizeof(T));
                throw;
            }
            ::new (static_cast<void*>(buffer_)) void*(raw);
            ops_ = &HEAP_OPS<T>;
        }
    }

    Function(Function&& other) noexcept { take(other); }

    Function& operator=(Function&& other) noexcept {
        if (this != &other) {
            reset();
            take(other);
        }
        return *this;
    }

    Function& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    template <class F, class = std::enable_if_t<
                           !std::is_same_v<std::decay_t<F>, Function> &&
                           std::is_invocable_r_v<R, std::decay_t<F>&, Args...>>>
    Function& operator=(F&& f) {
        return *this = Function(std::forward<F>(f));
    }

    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;

    ~Function() { reset(); }

    R operator()(Args... args) const {
        return ops_->invoke(const_cast<unsigned char*>(buffer_),
                            std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept { return !ops_->empty; }

    void swap(Function& other) noexcept {
        Function tmp(std::move(other));
        other = std::move(*this);
        *this = std::move(tmp);
    }

    friend bool operator==(const Function& f, std::nullptr_t) noexcept {
        return !f;
    }

   private:
    struct Ops {
        R (*invoke)(void* storage, Args&&... args);
        /// Move the target from @p src to @p dst and end it in @p src.
        void (*relocate)(void* dst, void* src) noexcept;
        void (*destroy)(void* storage) noexcept;
        // A flag, not the address of the empty table: a table built in
        // another shared library has another address.
        bool empty;
    };

    template <class T>
    static constexpr bool fits_inline() noexcept {
        return sizeof(T) <= BUFFER_SIZE && alignof(T) <= alignof(void*) &&
               std::is_nothrow_move_constructible_v<T>;
    }

    template <class T>
    static T& inline_target(void* storage) noexcept {
        return *std::launder(static_cast<T*>(storage));
    }

    static void*& heap_slot(void* storage) noexcept {
        return *std::launder(static_cast<void**>(storage));
    }

    template <class T>
    static T& heap_target(void* storage) noexcept {
        return *static_cast<T*>(heap_slot(storage));
    }

    template <class T>
    static R call(T& target, Args&&... args) {
        if constexpr (std::is_void_v<R>) {
            std::invoke(target, std::forward<Args>(args)...);
        } else {
            return std::invoke(target, std::forward<Args>(args)...);
        }
    }

    [[noreturn]] static R invoke_empty(void*, Args&&...) {
        throw std::bad_function_call();
    }

    static constexpr Ops EMPTY_OPS{&invoke_empty, [](void*, void*) noexcept {},
                                   [](void*) noexcept {}, true};

    template <class T>
    static constexpr Ops INLINE_OPS{
        [](void* s, Args&&... args) -> R {
            return call(inline_target<T>(s), std::forward<Args>(args)...);
        },
        [](void* dst, void* src) noexcept {
            T& from = inline_target<T>(src);
            ::new (dst) T(std::move(from));
            from.~T();
        },
        [](void* s) noexcept { inline_target<T>(s).~T(); }, false};

    template <class T>
    static constexpr Ops HEAP_OPS{[](void* s, Args&&... args) -> R {
                                      return call(heap_target<T>(s),
                                                  std::forward<Args>(args)...);
                                  },
                                  [](void* dst, void* src) noexcept {
                                      ::new (dst) void*(heap_slot(src));
                                  },
                                  [](void* s) noexcept {
                                      T& target = heap_target<T>(s);
                                      target.~T();
                                      memory::deallocate(&target, sizeof(T));
                                  },
                                  false};

    void take(Function& other) noexcept {
        ops_ = other.ops_;
        ops_->relocate(buffer_, other.buffer_);
        other.ops_ = &EMPTY_OPS;
    }

    void reset() noexcept {
        ops_->destroy(buffer_);
        ops_ = &EMPTY_OPS;
    }

    alignas(void*) unsigned char buffer_[BUFFER_SIZE];
    const Ops* ops_ = &EMPTY_OPS;
};

static_assert(sizeof(Function<void()>) == 32);

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_FUNCTION_H
