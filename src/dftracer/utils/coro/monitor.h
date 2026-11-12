// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_MONITOR_H
#define DFTRACER_UTILS_CORO_MONITOR_H

namespace dftracer::utils::coro::detail {

/// Installs the coroutine monitor as the @ref CoroutineHooks table when
/// DFTRACER_UTILS_MONITOR or DFTRACER_UTILS_MONITOR_FILE asks for it. Reads the
/// environment once. Modes:
///   1 or summary -> aggregate table at exit
///   tree         -> call tree at exit
///   deep         -> tree with co_await'd tasks that never suspend
///   trace        -> one log line per coroutine
void install_monitor_from_env();

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_CORO_MONITOR_H
