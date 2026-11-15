// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_UDF_SUPPORT_H
#define DFTRACER_UTILS_DATAFRAME_UDF_SUPPORT_H

// What the scalar UDF registry and the UDAF registry share: the dftu_ctx a
// callback reports through, the name rules and the types a user function takes.

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// The dftu_ctx of one callback: it keeps the message the callback sets and
/// reports the cancel that @ref UdfCancelScope holds on the thread.
class CallContext {
   public:
    CallContext() noexcept;
    CallContext(const CallContext&) = delete;
    CallContext& operator=(const CallContext&) = delete;

    dftu_ctx* get() noexcept { return &ctx_; }
    dstd::StringView message() const noexcept { return message_; }
    void set_message(dstd::StringView text) { message_.assign(text); }

   private:
    dftu_ctx ctx_;
    dstd::String message_;
};

/// Letters, digits and `_`, with `.` between non-empty parts, none starting
/// with a digit.
bool is_function_name(dstd::StringView name);

/// Whether a user function takes or gives a column of this type.
bool takes_type(TypeId id);

/// The error of a failed callback: `kind` is "udf" or "udaf", `step` the
/// callback, and the message the callback set on `ctx` when it set one.
dstd::Error call_error(dstd::StringView kind, dstd::StringView name,
                       dstd::StringView step, dftu_status status,
                       const CallContext& ctx);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_UDF_SUPPORT_H
