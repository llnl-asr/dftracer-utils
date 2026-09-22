#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/fold_adapter/ext.h>
#include <dftracer/utils/plugins/reserved_names.h>

#include <cstdint>

namespace dftracer::utils::plugins {
namespace {

::dftu_error not_found_error() {
    return ::dftu_error{dftracer::utils::CORE_DOMAIN.id,
                        static_cast<std::int32_t>(DFTU_COND_NOT_FOUND),
                        DFTU_COND_NOT_FOUND, "no such op"};
}

::dftu_error rejected_error() {
    return ::dftu_error{dftracer::utils::CORE_DOMAIN.id,
                        static_cast<std::int32_t>(DFTU_COND_INVALID_ARGUMENT),
                        DFTU_COND_INVALID_ARGUMENT,
                        "op rejected: kind/arity/shape mismatch"};
}

::dftu_result_series host_ops_run(void*, const char* name,
                                  const ::dftu_series* const* in,
                                  std::uint32_t n_in,
                                  const ::dftu_op_arg* args) {
    const ::dftu_op_desc* op = name ? ::dftu_op_find(name) : nullptr;
    if (!op) return ::dftu_result_series{0, {.err = not_found_error()}};
    ::dftu_series* result = ::dftu_op_run(op, in, n_in, args);
    if (!result) return ::dftu_result_series{0, {.err = rejected_error()}};
    return ::dftu_result_series{1, {.value = result}};
}

::dftu_result_scalar host_ops_run_aggregate(void*, const char* name,
                                            const ::dftu_series* const* in,
                                            std::uint32_t n_in,
                                            const ::dftu_op_arg* args) {
    const ::dftu_op_desc* op = name ? ::dftu_op_find(name) : nullptr;
    if (!op) return ::dftu_result_scalar{0, {.err = not_found_error()}};
    if (n_in != 1) return ::dftu_result_scalar{0, {.err = rejected_error()}};
    int run_ok = 0;
    ::dftu_scalar result = ::dftu_op_run_aggregate(op, in[0], args, &run_ok);
    if (!run_ok) return ::dftu_result_scalar{0, {.err = rejected_error()}};
    return ::dftu_result_scalar{1, {.value = result}};
}

::dftu_result_frame host_ops_run_frame(void*, const char* name,
                                       const ::dftu_dataframe* const* in,
                                       std::uint32_t n_in,
                                       const ::dftu_op_arg* args) {
    const ::dftu_op_desc* op = name ? ::dftu_op_find(name) : nullptr;
    if (!op) return ::dftu_result_frame{0, {.err = not_found_error()}};
    ::dftu_dataframe* result = ::dftu_op_run_frame(op, in, n_in, args);
    if (!result) return ::dftu_result_frame{0, {.err = rejected_error()}};
    return ::dftu_result_frame{1, {.value = result}};
}

::dftu_result_lazyframe host_ops_run_lazy(void*, const char* name,
                                          const ::dftu_lazyframe* const* in,
                                          std::uint32_t n_in,
                                          const ::dftu_op_arg* args) {
    const ::dftu_op_desc* op = name ? ::dftu_op_find(name) : nullptr;
    if (!op) return ::dftu_result_lazyframe{0, {.err = not_found_error()}};
    ::dftu_lazyframe* result = ::dftu_op_run_lazy(op, in, n_in, args);
    if (!result) return ::dftu_result_lazyframe{0, {.err = rejected_error()}};
    return ::dftu_result_lazyframe{1, {.value = result}};
}

const ::dftu_op_desc* host_ops_find(void*, const char* name) {
    return ::dftu_op_find(name);
}

// Registration is a load-time concern: an op registered mid-scan lands after
// settle_order, settle_prune and the planner, so it cannot be ordered, pruned
// or planned, and whether a peer sees it depends on which worker claimed which
// morsel first. Talk to a peer during the scan through ports instead.
int host_ops_register(void*, const ::dftu_op_desc* desc) {
    DFTRACER_UTILS_LOG_ERROR(
        "Plugin op '%s' refused: register from the plugin factory, which runs "
        "before the scan; the scan-time host does not register ops",
        desc && desc->name ? desc->name : "(null)");
    return -1;
}

const ::dftu_svc_ops g_ops = {host_ops_run,       host_ops_run_aggregate,
                              host_ops_run_frame, host_ops_find,
                              host_ops_register,  host_ops_run_lazy};

}  // namespace

int detail::register_plugin_op(const ::dftu_op_desc* desc) {
    if (desc && refuse_plugin_op_name(desc->name)) {
        DFTRACER_UTILS_LOG_ERROR(
            "Plugin op '%s' refused: the bare and 'dftu.' namespaces hold the "
            "host's built-in ops, so a plugin op must be '<plugin>.<name>'",
            desc->name ? desc->name : "(null)");
        return -1;
    }
    return ::dftu_op_register(desc);
}

const void* detail::ops_ext_vtable() { return &g_ops; }

}  // namespace dftracer::utils::plugins
