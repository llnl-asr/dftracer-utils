// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/dataframe/internal/run_counters.h>
#include <dftracer/utils/plugins/abi_context.h>

namespace dftracer::utils::plugins {

const dftu_ctx_vt AbiContext::VT = {
    sizeof(dftu_ctx_vt),
    &AbiContext::set_error,
    &AbiContext::check,
    &AbiContext::counter,
};

AbiContext::AbiContext(coro::CancelView cancel, dstd::StringView owner,
                       dataframe::RunCounters* counters) noexcept
    : ctx_{sizeof(dftu_ctx), 0, &VT, this},
      cancel_(cancel),
      owner_(owner),
      counters_(counters) {}

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

void AbiContext::counter(dftu_ctx* c, const char* name, std::uint32_t len,
                         std::int64_t delta) {
    const auto* self = static_cast<const AbiContext*>(c->h);
    if (self->counters_ == nullptr || name == nullptr) return;
    self->counters_->add(self->owner_, dstd::StringView(name, len), delta);
}

dstd::Error AbiContext::error(dftu_status status, dstd::StringView what) const {
    const dstd::Condition condition = common::to_condition(status);
    if (!message_.empty()) return dstd::Error(condition, message_);
    return dstd::Error::from_format<"{} failed">(condition, what);
}

}  // namespace dftracer::utils::plugins
