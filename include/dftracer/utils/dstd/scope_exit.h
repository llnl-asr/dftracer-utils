// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_SCOPE_EXIT_H
#define DFTRACER_UTILS_DSTD_SCOPE_EXIT_H

#include <type_traits>
#include <utility>

namespace dftracer::utils::dstd {

/**
 * @brief Runs a callable when it goes out of scope, unless released.
 *
 * Move-only. The callable must not throw, because it runs from the noexcept
 * destructor. Keep a named local, since a discarded temporary runs at once.
 */
template <class F>
class ScopeExit {
   public:
    explicit ScopeExit(F f) noexcept(std::is_nothrow_move_constructible_v<F>)
        : f_(std::move(f)) {}
    ScopeExit(ScopeExit&& other) noexcept(
        std::is_nothrow_move_constructible_v<F>)
        : f_(std::move(other.f_)), active_(other.active_) {
        other.active_ = false;
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

    ~ScopeExit() {
        if (active_) f_();
    }

    /// Deactivates the guard, so the callable does not run at scope exit.
    void release() noexcept { active_ = false; }

   private:
    F f_;
    bool active_ = true;
};

template <class F>
ScopeExit<std::decay_t<F>> make_scope_exit(F&& f) {
    return ScopeExit<std::decay_t<F>>(std::forward<F>(f));
}

/// Short name for make_scope_exit, as in `auto guard = defer([&] { f(); });`
template <class F>
[[nodiscard]] ScopeExit<std::decay_t<F>> defer(F&& f) noexcept {
    return ScopeExit<std::decay_t<F>>(std::forward<F>(f));
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_SCOPE_EXIT_H
