#ifndef DFTRACER_UTILS_DSTD_ANY_H
#define DFTRACER_UTILS_DSTD_ANY_H

#include <dftracer/utils/memory/heap.h>

#include <any>
#include <cstddef>
#include <new>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace dftracer::utils::dstd {

/**
 * @brief A std::any whose heap values come from memory::allocate: 32 bytes,
 * with a 24-byte inline buffer.
 *
 * A value that fits the buffer with its alignment and is nothrow move
 * constructible stays inline; any other is placed by memory::allocate and
 * freed with the sized memory::deallocate. Copyable like std::any, so a value
 * must be copy constructible. A moved-from Any is empty. A failed any_cast
 * throws std::bad_any_cast.
 */
class Any {
   public:
    static constexpr std::size_t BUFFER_SIZE = 3 * sizeof(void*);

    Any() noexcept = default;

    template <class T, class = std::enable_if_t<
                           !std::is_same_v<std::decay_t<T>, Any> &&
                           std::is_copy_constructible_v<std::decay_t<T>>>>
    Any(T&& value) {
        emplace<std::decay_t<T>>(std::forward<T>(value));
    }

    Any(const Any& other) : ops_(other.ops_) {
        if (ops_ != nullptr) ops_->copy(buffer_, other.buffer_);
    }

    Any(Any&& other) noexcept { take(other); }

    Any& operator=(const Any& other) {
        if (this != &other) *this = Any(other);
        return *this;
    }

    Any& operator=(Any&& other) noexcept {
        if (this != &other) {
            reset();
            take(other);
        }
        return *this;
    }

    template <class T, class = std::enable_if_t<
                           !std::is_same_v<std::decay_t<T>, Any> &&
                           std::is_copy_constructible_v<std::decay_t<T>>>>
    Any& operator=(T&& value) {
        return *this = Any(std::forward<T>(value));
    }

    ~Any() { reset(); }

    /// Destroy the held value, then construct a T from @p args. When the
    /// constructor throws, the Any is empty.
    template <class T, class... Args>
    std::decay_t<T>& emplace(Args&&... args) {
        using U = std::decay_t<T>;
        static_assert(std::is_copy_constructible_v<U>,
                      "Any holds only copy constructible values");
        reset();
        if constexpr (fits_inline<U>()) {
            U* p = ::new (static_cast<void*>(buffer_))
                U(std::forward<Args>(args)...);
            ops_ = &INLINE_OPS<U>;
            return *p;
        } else {
            void* raw = memory::allocate(sizeof(U), alignof(U));
            U* p;
            try {
                p = ::new (raw) U(std::forward<Args>(args)...);
            } catch (...) {
                memory::deallocate(raw, sizeof(U));
                throw;
            }
            ::new (static_cast<void*>(buffer_)) void*(raw);
            ops_ = &HEAP_OPS<U>;
            return *p;
        }
    }

    void reset() noexcept {
        if (ops_ != nullptr) {
            ops_->destroy(buffer_);
            ops_ = nullptr;
        }
    }

    bool has_value() const noexcept { return ops_ != nullptr; }

    /// typeid of the held value, typeid(void) when empty.
    const std::type_info& type() const noexcept {
        return ops_ != nullptr ? ops_->type() : typeid(void);
    }

    void swap(Any& other) noexcept {
        Any tmp(std::move(other));
        other = std::move(*this);
        *this = std::move(tmp);
    }

    template <class T>
    friend const T* any_cast(const Any* any) noexcept;
    template <class T>
    friend T* any_cast(Any* any) noexcept;

   private:
    struct Ops {
        void (*copy)(void* dst, const void* src);
        /// Move the value from @p src to @p dst and end it in @p src.
        void (*relocate)(void* dst, void* src) noexcept;
        void (*destroy)(void* storage) noexcept;
        const std::type_info& (*type)() noexcept;
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

    static void* heap_slot(const void* storage) noexcept {
        return *std::launder(static_cast<void* const*>(storage));
    }

    template <class T>
    static const std::type_info& type_of() noexcept {
        return typeid(T);
    }

    template <class T>
    static constexpr Ops INLINE_OPS{
        [](void* dst, const void* src) {
            ::new (dst) T(inline_target<T>(const_cast<void*>(src)));
        },
        [](void* dst, void* src) noexcept {
            T& from = inline_target<T>(src);
            ::new (dst) T(std::move(from));
            from.~T();
        },
        [](void* s) noexcept { inline_target<T>(s).~T(); }, &type_of<T>};

    template <class T>
    static constexpr Ops HEAP_OPS{
        [](void* dst, const void* src) {
            const T& from = *static_cast<const T*>(heap_slot(src));
            void* raw = memory::allocate(sizeof(T), alignof(T));
            try {
                ::new (raw) T(from);
            } catch (...) {
                memory::deallocate(raw, sizeof(T));
                throw;
            }
            ::new (dst) void*(raw);
        },
        [](void* dst, void* src) noexcept {
            ::new (dst) void*(heap_slot(src));
        },
        [](void* s) noexcept {
            T* target = static_cast<T*>(heap_slot(s));
            target->~T();
            memory::deallocate(target, sizeof(T));
        },
        &type_of<T>};

    // The table address identifies the type without RTTI; a table copied
    // into another shared library has another address, so type() decides.
    template <class T>
    bool holds() const noexcept {
        if constexpr (!std::is_copy_constructible_v<T>) {
            return false;
        } else {
            if (ops_ == nullptr) return false;
            if constexpr (fits_inline<T>()) {
                if (ops_ == &INLINE_OPS<T>) return true;
            } else {
                if (ops_ == &HEAP_OPS<T>) return true;
            }
            return ops_->type() == typeid(T);
        }
    }

    template <class T>
    T* target() noexcept {
        if constexpr (fits_inline<T>()) {
            return &inline_target<T>(buffer_);
        } else {
            return static_cast<T*>(heap_slot(buffer_));
        }
    }

    void take(Any& other) noexcept {
        ops_ = other.ops_;
        if (ops_ != nullptr) ops_->relocate(buffer_, other.buffer_);
        other.ops_ = nullptr;
    }

    alignas(void*) unsigned char buffer_[BUFFER_SIZE];
    const Ops* ops_ = nullptr;
};

static_assert(sizeof(Any) == 32);

template <class T>
const T* any_cast(const Any* any) noexcept {
    static_assert(!std::is_reference_v<T>);
    using U = std::remove_cv_t<T>;
    if (any == nullptr || !any->holds<U>()) return nullptr;
    return const_cast<Any*>(any)->target<U>();
}

template <class T>
T* any_cast(Any* any) noexcept {
    static_assert(!std::is_reference_v<T>);
    using U = std::remove_cv_t<T>;
    if (any == nullptr || !any->holds<U>()) return nullptr;
    return any->target<U>();
}

template <class T>
T any_cast(const Any& any) {
    using U = std::remove_cvref_t<T>;
    static_assert(std::is_constructible_v<T, const U&>);
    const U* p = any_cast<U>(&any);
    if (p == nullptr) throw std::bad_any_cast();
    return static_cast<T>(*p);
}

template <class T>
T any_cast(Any& any) {
    using U = std::remove_cvref_t<T>;
    static_assert(std::is_constructible_v<T, U&>);
    U* p = any_cast<U>(&any);
    if (p == nullptr) throw std::bad_any_cast();
    return static_cast<T>(*p);
}

template <class T>
T any_cast(Any&& any) {
    using U = std::remove_cvref_t<T>;
    static_assert(std::is_constructible_v<T, U>);
    U* p = any_cast<U>(&any);
    if (p == nullptr) throw std::bad_any_cast();
    return static_cast<T>(std::move(*p));
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_ANY_H
