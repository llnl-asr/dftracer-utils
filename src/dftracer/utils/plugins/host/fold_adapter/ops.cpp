// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dataframe/udaf_registry.h>
#include <dftracer/utils/dataframe/udf_registry.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/duql/lower.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/reserved_names.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::plugins::host {
namespace {

dftu_status find_op(::dftu_ctx* ctx, const char* name,
                    const ::dftu_op_desc** op) {
    *op = name ? ::dftu_op_find(name) : nullptr;
    if (*op != nullptr) return DFTU_OK;
    return host_fail(ctx, DFTU_E_NOT_FOUND,
                     dstd::fmt<"no op named '{}'">(
                         dstd::StringView(name ? name : "(null)")));
}

dftu_status rejected(::dftu_ctx* ctx, const char* name) {
    return host_fail(ctx, DFTU_E_INVALID,
                     dstd::fmt<"op '{}' rejected: kind/arity/shape mismatch">(
                         dstd::StringView(name)));
}

dftu_status host_ops_run(::dftu_ctx* ctx, void*, const char* name,
                         const ::dftu_series* const* in, std::uint32_t n_in,
                         const ::dftu_op_arg* args, ::dftu_series** out) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "run: out is NULL");
    const ::dftu_op_desc* op = nullptr;
    if (const dftu_status found = find_op(ctx, name, &op); found < 0)
        return found;
    ::dftu_series* result = ::dftu_op_run(op, in, n_in, args);
    if (result == nullptr) return rejected(ctx, name);
    *out = result;
    return DFTU_OK;
}

dftu_status host_ops_run_aggregate(::dftu_ctx* ctx, void*, const char* name,
                                   const ::dftu_series* const* in,
                                   std::uint32_t n_in,
                                   const ::dftu_op_arg* args,
                                   ::dftu_scalar* out) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "run_aggregate: out is NULL");
    const ::dftu_op_desc* op = nullptr;
    if (const dftu_status found = find_op(ctx, name, &op); found < 0)
        return found;
    if (n_in != 1) return rejected(ctx, name);
    int run_ok = 0;
    const ::dftu_scalar result =
        ::dftu_op_run_aggregate(op, in[0], args, &run_ok);
    if (!run_ok) return rejected(ctx, name);
    *out = result;
    return DFTU_OK;
}

dftu_status host_ops_run_frame(::dftu_ctx* ctx, void*, const char* name,
                               const ::dftu_dataframe* const* frames,
                               std::uint32_t n_in, const ::dftu_op_arg* args,
                               ::dftu_dataframe** out) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "run_frame: out is NULL");
    const ::dftu_op_desc* op = nullptr;
    if (const dftu_status found = find_op(ctx, name, &op); found < 0)
        return found;
    ::dftu_dataframe* result = ::dftu_op_run_frame(op, frames, n_in, args);
    if (result == nullptr) return rejected(ctx, name);
    *out = result;
    return DFTU_OK;
}

dftu_status host_ops_run_lazy(::dftu_ctx* ctx, void*, const char* name,
                              const ::dftu_lazyframe* const* in,
                              std::uint32_t n_in, const ::dftu_op_arg* args,
                              ::dftu_lazyframe** out) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "run_lazy: out is NULL");
    const ::dftu_op_desc* op = nullptr;
    if (const dftu_status found = find_op(ctx, name, &op); found < 0)
        return found;
    ::dftu_lazyframe* result = ::dftu_op_run_lazy(op, in, n_in, args);
    if (result == nullptr) return rejected(ctx, name);
    *out = result;
    return DFTU_OK;
}

const ::dftu_op_desc* host_ops_find(void*, const char* name) {
    return ::dftu_op_find(name);
}

// Registration is a load-time concern: an op registered mid-scan lands after
// settle_order, settle_prune and the planner, so it cannot be ordered, pruned
// or planned, and whether a peer sees it depends on which worker claimed which
// morsel first. Talk to a peer during the scan through ports instead.
dftu_status host_ops_register(::dftu_ctx* ctx, void*,
                              const ::dftu_op_desc* desc) {
    return host_fail(
        ctx, DFTU_E_UNSUPPORTED,
        dstd::fmt<"op '{}' refused: register from the plugin factory, which "
                  "runs before the scan; the scan-time host does not register "
                  "ops">(
            dstd::StringView(desc && desc->name ? desc->name : "(null)")));
}

// A UDF registered mid-scan reaches no plan that was lowered before it, for
// the same reason.
dftu_status host_ops_register_udf(::dftu_ctx* ctx, void*,
                                  const ::dftu_udf_desc* desc, void*) {
    return host_fail(
        ctx, DFTU_E_UNSUPPORTED,
        dstd::fmt<"udf '{}' refused: register from the plugin factory, which "
                  "runs before the scan; the scan-time host does not register "
                  "udfs">(
            dstd::StringView(desc && desc->name ? desc->name : "(null)")));
}

dftu_status host_ops_register_udaf(::dftu_ctx* ctx, void*,
                                   const ::dftu_udaf_desc* desc, void*) {
    return host_fail(
        ctx, DFTU_E_UNSUPPORTED,
        dstd::fmt<"udaf '{}' refused: register from the plugin factory, which "
                  "runs before the scan; the scan-time host does not register "
                  "udafs">(
            dstd::StringView(desc && desc->name ? desc->name : "(null)")));
}

const ::dftu_svc_ops g_ops = {
    sizeof(::dftu_svc_ops), host_ops_run,          host_ops_run_aggregate,
    host_ops_run_frame,     host_ops_find,         host_ops_register,
    host_ops_run_lazy,      host_ops_register_udf, host_ops_register_udaf};

}  // namespace

dftu_status detail::register_plugin_op(::dftu_ctx* ctx,
                                       const ::dftu_op_desc* desc,
                                       dstd::SharedPtr<PluginLibrary> library) {
    if (desc == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "register_op: desc is NULL");
    if (refuse_plugin_op_name(desc->name)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"op '{}' refused: the bare and 'dftu.' namespaces hold "
                      "the host's built-in ops, so a plugin op must be "
                      "'<plugin>.<name>'">(
                dstd::StringView(desc->name ? desc->name : "(null)")));
    }
    if (!dataframe::register_op(*desc, std::move(library))) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"op '{}' refused: the name is already registered">(
                dstd::StringView(desc->name)));
    }
    return DFTU_OK;
}

dftu_status detail::register_plugin_udf(
    ::dftu_ctx* ctx, const ::dftu_udf_desc* desc, void* self,
    dstd::SharedPtr<PluginLibrary> library) {
    if (desc == nullptr || desc->name == nullptr)
        return host_fail(ctx, DFTU_E_INVALID,
                         "register_udf: desc or its name is NULL");
    if (desc->struct_size < sizeof(::dftu_udf_desc))
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"udf '{}' refused: struct_size {} is below {}">(
                dstd::StringView(desc->name), desc->struct_size,
                static_cast<std::uint32_t>(sizeof(::dftu_udf_desc))));
    auto registered = dataframe::add_udf(*desc, self, std::move(library));
    if (!registered) return host_fail(ctx, registered.error());
    return DFTU_OK;
}

dftu_status detail::register_plugin_udaf(
    ::dftu_ctx* ctx, const ::dftu_udaf_desc* desc, void* self,
    dstd::SharedPtr<PluginLibrary> library) {
    if (desc == nullptr || desc->name == nullptr)
        return host_fail(ctx, DFTU_E_INVALID,
                         "register_udaf: desc or its name is NULL");
    if (desc->struct_size < sizeof(::dftu_udaf_desc))
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"udaf '{}' refused: struct_size {} is below {}">(
                dstd::StringView(desc->name), desc->struct_size,
                static_cast<std::uint32_t>(sizeof(::dftu_udaf_desc))));
    auto registered = dataframe::add_udaf(*desc, self, std::move(library));
    if (!registered) return host_fail(ctx, registered.error());
    return DFTU_OK;
}

const void* detail::ops_ext_vtable() { return &g_ops; }

}  // namespace dftracer::utils::plugins::host
