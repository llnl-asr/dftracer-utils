// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/abi_error.h>
#include <dftracer/utils/plugins/abi_context.h>

namespace dftracer::utils::plugins {

const dftu_ctx_vt AbiContext::VT = {
    sizeof(dftu_ctx_vt),
    &AbiContext::set_error,
    &AbiContext::check,
};

AbiContext::AbiContext(coro::CancelView cancel) noexcept
    : ctx_{sizeof(dftu_ctx), 0, &VT, this}, cancel_(cancel) {}

dftu_status AbiContext::set_error(dftu_ctx* c, dftu_status code,
                                  const char* msg, std::uint32_t len) {
    auto* self = static_cast<AbiContext*>(c->h);
    if (msg != nullptr) self->message_.assign(msg, len);
    return code;
}

dftu_status AbiContext::check(dftu_ctx* c) {
    const auto* self = static_cast<const AbiContext*>(c->h);
    return self->cancel_.requested() ? DFTU_E_CANCELLED : DFTU_OK;
}

dstd::Error AbiContext::error(dftu_status status, dstd::StringView what) const {
    const dstd::Condition condition = common::to_condition(status);
    if (!message_.empty()) return dstd::Error(condition, message_);
    return dstd::Error::from_format<"{} failed">(condition, what);
}

}  // namespace dftracer::utils::plugins
