// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>

#include <cstdint>

namespace dftracer::utils::plugins::host {
namespace {

dftu_status host_result_emit(::dftu_ctx* ctx, void* h, const char* name,
                             ::dftu_result_value* v) {
    const dstd::Result<void> emitted =
        static_cast<PluginFold*>(h)->emit_result(name, v);
    if (!emitted) return host_fail(ctx, emitted.error());
    return DFTU_OK;
}

const dftu_svc_result g_result = {sizeof(dftu_svc_result), host_result_emit};

}  // namespace

const void* detail::result_ext_vtable() { return &g_result; }

dstd::Result<void> PluginFold::emit_result(const char* name,
                                           ::dftu_result_value* v) {
    if (!name || !v) {
        return common::make_error<
            "plugin '{}' emit: name and value must be "
            "set">(common::ErrorCode::InvalidArgument, plugin_label());
    }
    if (!named_results_) {
        return common::make_error<
            "plugin '{}' emit '{}': this run keeps no named results">(
            dstd::Condition::Unsupported, plugin_label(), name);
    }
    switch (static_cast<dftu_result_kind>(v->kind)) {
        case DFTU_RESULT_KIND_BYTES:
            named_results_->emit_blob(name, v->u.bytes.data, v->u.bytes.len);
            return {};
        case DFTU_RESULT_KIND_FRAME:
            named_results_->emit_frame(name, v->u.frame);
            return {};
        case DFTU_RESULT_KIND_LAZYFRAME:
            named_results_->emit_lazyframe(name, v->u.lazyframe);
            return {};
    }
    return common::make_error<"plugin '{}' emit '{}': unknown result kind {}">(
        common::ErrorCode::InvalidArgument, plugin_label(), name, v->kind);
}

}  // namespace dftracer::utils::plugins::host
