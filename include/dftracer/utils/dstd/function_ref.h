// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_FUNCTION_REF_H
#define DFTRACER_UTILS_DSTD_FUNCTION_REF_H

#include <memory>
#include <type_traits>

namespace dftracer::utils::dstd {

/// A non-owning reference to a callable, after std::function_ref (P0792,
/// C++26), for a callback used only inside the call that takes it. It
/// allocates nothing and is trivially copyable. A callable object must outlive
/// every use. A function or function pointer is stored by value.
template <class Signature>
class FunctionRef;

template <class R, class... Args>
class FunctionRef<R(Args...)> {
    template <class T>
    static constexpr bool IS_FUNCTION_POINTER =
        std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>;

   public:
    FunctionRef() = delete;

    template <class F>
        requires std::is_function_v<F> && std::is_invocable_r_v<R, F&, Args...>
    FunctionRef(F* fn) noexcept : call_(&call_function<F>) {
        bound_.function = reinterpret_cast<void (*)()>(fn);
    }

    template <class F>
        requires std::is_function_v<F> && std::is_invocable_r_v<R, F&, Args...>
    FunctionRef(F& fn) noexcept : FunctionRef(&fn) {}

    template <class F, class T = std::remove_reference_t<F>>
        requires(!std::is_same_v<std::remove_cv_t<T>, FunctionRef> &&
                 !std::is_function_v<T> &&
                 !IS_FUNCTION_POINTER<std::remove_cv_t<T>> &&
                 std::is_invocable_r_v<R, T&, Args...>)
    FunctionRef(F&& fn) noexcept : call_(&call_object<T>) {
        bound_.object =
            const_cast<void*>(static_cast<const void*>(std::addressof(fn)));
    }

    FunctionRef(const FunctionRef&) = default;
    FunctionRef& operator=(const FunctionRef&) = default;

    R operator()(Args... args) const {
        return call_(bound_, static_cast<Args&&>(args)...);
    }

   private:
    union Bound {
        void* object;
        void (*function)();
    };

    // T keeps the callable's const, so a const object is called as const.
    template <class T>
    static R call_object(Bound bound, Args... args) {
        return (*static_cast<T*>(bound.object))(static_cast<Args&&>(args)...);
    }
    template <class F>
    static R call_function(Bound bound, Args... args) {
        return reinterpret_cast<F*>(bound.function)(
            static_cast<Args&&>(args)...);
    }

    Bound bound_;
    R (*call_)(Bound, Args...);
};

static_assert(sizeof(FunctionRef<void()>) == 2 * sizeof(void*));

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_FUNCTION_REF_H
