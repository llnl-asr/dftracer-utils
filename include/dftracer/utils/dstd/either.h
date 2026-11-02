// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_EITHER_H
#define DFTRACER_UTILS_DSTD_EITHER_H

#include <dftracer/utils/dstd/variant.h>

#include <cassert>
#include <cstddef>
#include <utility>

namespace dftracer::utils::dstd {

/// Exactly one of two values, named left and right. Unlike Variant<L, R> it
/// works when L and R are the same type. Use Expected for value-or-error.
/// left() and right() require the matching side to be held.
template <class L, class R>
class Either {
   public:
    static Either left(L value) {
        return Either(std::in_place_index<0>, std::move(value));
    }
    static Either right(R value) {
        return Either(std::in_place_index<1>, std::move(value));
    }

    bool is_left() const noexcept { return value_.index() == 0; }
    bool is_right() const noexcept { return value_.index() == 1; }

    L& left() & noexcept { return *checked<0>(); }
    const L& left() const& noexcept { return *checked<0>(); }
    L&& left() && noexcept { return std::move(*checked<0>()); }
    R& right() & noexcept { return *checked<1>(); }
    const R& right() const& noexcept { return *checked<1>(); }
    R&& right() && noexcept { return std::move(*checked<1>()); }

    /// Calls on_left or on_right with the held value. Both must return the
    /// same type.
    template <class F, class G>
    decltype(auto) fold(F&& on_left, G&& on_right) & {
        if (is_left()) return std::forward<F>(on_left)(left());
        return std::forward<G>(on_right)(right());
    }
    template <class F, class G>
    decltype(auto) fold(F&& on_left, G&& on_right) const& {
        if (is_left()) return std::forward<F>(on_left)(left());
        return std::forward<G>(on_right)(right());
    }
    template <class F, class G>
    decltype(auto) fold(F&& on_left, G&& on_right) && {
        if (is_left()) return std::forward<F>(on_left)(std::move(*this).left());
        return std::forward<G>(on_right)(std::move(*this).right());
    }

   private:
    template <std::size_t I, class T>
    Either(std::in_place_index_t<I> side, T&& value)
        : value_(side, std::forward<T>(value)) {}

    template <std::size_t I>
    auto* checked() noexcept {
        auto* held = std::get_if<I>(&value_);
        assert(held != nullptr);
        return held;
    }
    template <std::size_t I>
    const auto* checked() const noexcept {
        const auto* held = std::get_if<I>(&value_);
        assert(held != nullptr);
        return held;
    }

    Variant<L, R> value_;
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_EITHER_H
