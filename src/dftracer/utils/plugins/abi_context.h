// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_CONTEXT_H
#define DFTRACER_UTILS_PLUGINS_ABI_CONTEXT_H

#include <dftracer/utils/common/c_api.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::plugins {

/**
 * @brief The host side of one @ref dftu_ctx: keeps the message a callback
 * reports and reads the cancel of the task that makes the call.
 *
 * One per callback call, on the caller's frame. Not movable, since the
 * dftu_ctx points back at it.
 */
class AbiContext {
   public:
    explicit AbiContext(coro::CancelView cancel = {}) noexcept;
    AbiContext(const AbiContext&) = delete;
    AbiContext& operator=(const AbiContext&) = delete;

    dftu_ctx* get() noexcept { return &ctx_; }

    /// The message the callback set, empty when it set none.
    dstd::StringView message() const noexcept { return message_; }

    /// The error for a callback that returned @p status; @p what names the
    /// call when the callback set no message.
    dstd::Error error(dftu_status status, dstd::StringView what) const;

   private:
    static dftu_status set_error(dftu_ctx* c, dftu_status code, const char* msg,
                                 std::uint32_t len);
    static dftu_status check(dftu_ctx* c);

    static const dftu_ctx_vt VT;

    dftu_ctx ctx_;
    coro::CancelView cancel_;
    dstd::String message_;
};

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_ABI_CONTEXT_H
