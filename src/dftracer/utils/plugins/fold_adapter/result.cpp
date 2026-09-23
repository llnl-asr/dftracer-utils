#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/fold_adapter/ext.h>

#include <cstdint>

namespace dftracer::utils::plugins {
namespace {

int host_result_emit(void* h, const char* name, ::dftu_result_value* v) {
    return static_cast<PluginFold*>(h)->result_emit(name, v);
}

const dftu_svc_result g_result = {host_result_emit};

}  // namespace

const void* detail::result_ext_vtable() { return &g_result; }

int PluginFold::result_emit(const char* name, ::dftu_result_value* v) {
    if (!named_results_ || !name || !v) return -1;
    switch (static_cast<dftu_result_kind>(v->kind)) {
        case DFTU_RESULT_KIND_BYTES:
            named_results_->emit_blob(name, v->u.bytes.data, v->u.bytes.len);
            return 0;
        case DFTU_RESULT_KIND_ARROW:
            return named_results_->emit_arrow(name, v->u.arrow.array,
                                              v->u.arrow.schema);
        case DFTU_RESULT_KIND_FRAME:
            named_results_->emit_frame(name, v->u.frame);
            return 0;
        case DFTU_RESULT_KIND_LAZYFRAME:
            named_results_->emit_lazyframe(name, v->u.lazyframe);
            return 0;
    }
    return -1;
}

}  // namespace dftracer::utils::plugins
