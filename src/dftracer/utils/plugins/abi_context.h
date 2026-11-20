// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_CONTEXT_H
#define DFTRACER_UTILS_PLUGINS_ABI_CONTEXT_H

#include <dftracer/utils/common/c_api.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/lazyframe.h>
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
    /// @p owner names the registered source, format or node the call belongs
    /// to, and @p counters is the run its counters add to (null outside an
    /// EXPLAIN ANALYZE run); both are borrowed and must outlive the call.
    explicit AbiContext(coro::CancelView cancel = {},
                        dstd::StringView owner = {},
                        dataframe::RunCounters* counters = nullptr) noexcept;
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
    static void counter(dftu_ctx* c, const char* name, std::uint32_t len,
                        std::int64_t delta);

    static const dftu_ctx_vt VT;

    dftu_ctx ctx_;
    coro::CancelView cancel_;
    dstd::StringView owner_;
    dataframe::RunCounters* counters_;
    dstd::String message_;
};

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_ABI_CONTEXT_H
