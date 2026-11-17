// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/host/build_host.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/plugin_abi.h>
#include <dftracer/utils/plugins/host/state_registry.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::plugins::host {
namespace {

BuildHost& self_of(void* h) { return *static_cast<BuildHost*>(h); }

dftu_status denied(::dftu_ctx* ctx, BuildHost& host, const char* what) {
    host.deny(what);
    return host_fail(ctx, DFTU_E_UNSUPPORTED,
                     "not available at load: the factory host offers "
                     "registration only");
}

dftu_status build_ops_run(::dftu_ctx* ctx, void* h, const char*,
                          const ::dftu_series* const*, std::uint32_t,
                          const ::dftu_op_arg*, ::dftu_series**) {
    return denied(ctx, self_of(h), DFTU_SVC_OPS "::run");
}

dftu_status build_ops_run_aggregate(::dftu_ctx* ctx, void* h, const char*,
                                    const ::dftu_series* const*, std::uint32_t,
                                    const ::dftu_op_arg*, ::dftu_scalar*) {
    return denied(ctx, self_of(h), DFTU_SVC_OPS "::run_aggregate");
}

dftu_status build_ops_run_frame(::dftu_ctx* ctx, void* h, const char*,
                                const ::dftu_dataframe* const*, std::uint32_t,
                                const ::dftu_op_arg*, ::dftu_dataframe**) {
    return denied(ctx, self_of(h), DFTU_SVC_OPS "::run_frame");
}

// Registering an op means checking the name is free, so find() is part of the
// registration surface rather than an escape from it.
const ::dftu_op_desc* build_ops_find(void*, const char* name) {
    return ::dftu_op_find(name);
}

dftu_status build_ops_register(::dftu_ctx* ctx, void* h,
                               const ::dftu_op_desc* desc) {
    const dftu_status status =
        detail::register_plugin_op(ctx, desc, self_of(h).library());
    if (status >= 0) self_of(h).registered_ops().emplace_back(desc->name);
    return status;
}

dftu_status build_ops_run_lazy(::dftu_ctx* ctx, void* h, const char*,
                               const ::dftu_lazyframe* const*, std::uint32_t,
                               const ::dftu_op_arg*, ::dftu_lazyframe**) {
    return denied(ctx, self_of(h), DFTU_SVC_OPS "::run_lazy");
}

dftu_status build_ops_register_udf(::dftu_ctx* ctx, void* h,
                                   const ::dftu_udf_desc* desc, void* self) {
    const dftu_status status =
        detail::register_plugin_udf(ctx, desc, self, self_of(h).library());
    if (status >= 0) self_of(h).registered_udfs().emplace_back(desc->name);
    return status;
}

dftu_status build_ops_register_udaf(::dftu_ctx* ctx, void* h,
                                    const ::dftu_udaf_desc* desc, void* self) {
    const dftu_status status =
        detail::register_plugin_udaf(ctx, desc, self, self_of(h).library());
    if (status >= 0) self_of(h).registered_udafs().emplace_back(desc->name);
    return status;
}

const ::dftu_svc_ops g_build_ops = {
    sizeof(::dftu_svc_ops), build_ops_run,          build_ops_run_aggregate,
    build_ops_run_frame,    build_ops_find,         build_ops_register,
    build_ops_run_lazy,     build_ops_register_udf, build_ops_register_udaf};

dftu_status build_agg_new(::dftu_ctx* ctx, void* h, const char*,
                          const char* const*, std::uint32_t,
                          const ::dftu_agg_col*, std::uint32_t, ::dftu_agg**) {
    return denied(ctx, self_of(h), DFTU_SVC_AGG "::agg_new");
}

dftu_status build_agg_accumulate(::dftu_ctx* ctx, void* h, ::dftu_agg*,
                                 const ::dftu_dataframe*) {
    return denied(ctx, self_of(h), DFTU_SVC_AGG "::agg_accumulate");
}

dftu_status build_agg_result(::dftu_ctx* ctx, void* h, const char*,
                             ::dftu_dataframe**) {
    return denied(ctx, self_of(h), DFTU_SVC_AGG "::agg_result");
}

dftu_status build_register_state(::dftu_ctx* ctx, void* h,
                                 const ::dftu_state_desc* desc, void* self) {
    return register_state_into(self_of(h).states(), ctx, desc, self);
}

const ::dftu_svc_agg g_build_agg = {sizeof(::dftu_svc_agg), build_agg_new,
                                    build_agg_accumulate, build_agg_result,
                                    build_register_state};

// A port key is a pure hash of the name, so wiring a port up is available at
// load; moving bytes through one needs a batch that does not exist yet.
std::uint64_t build_port_key(void*, const char* name) {
    return name ? dftracer::utils::dstd::hash::fnv1a_hash(name) : 0;
}

void build_port_publish(void* h, std::uint64_t, const void*, std::uint32_t) {
    self_of(h).deny(DFTU_SVC_PORTS "::publish");
}

const void* build_port_consume(void* h, std::uint64_t,
                               std::uint32_t* out_size) {
    self_of(h).deny(DFTU_SVC_PORTS "::consume");
    if (out_size) *out_size = 0;
    return nullptr;
}

dftu_status build_port_try_publish(::dftu_ctx* ctx, void* h, std::uint64_t,
                                   const void*, std::uint32_t,
                                   ::dftu_port_status*) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::try_publish");
}

dftu_status build_port_try_consume(::dftu_ctx* ctx, void* h, std::uint64_t,
                                   void*, std::uint32_t, std::uint32_t*,
                                   ::dftu_port_status*) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::try_consume");
}

dftu_status build_channel_open(::dftu_ctx* ctx, void* h,
                               const ::dftu_channel_desc* desc,
                               std::uint64_t* out) {
    BuildHost& bh = self_of(h);
    if (desc == nullptr || out == nullptr) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "channel_open: NULL descriptor or out");
    }
    if (!plugin_struct_size_ok(
            PluginStruct::ChannelDesc, desc->struct_size,
            static_cast<std::uint32_t>(sizeof(::dftu_channel_desc)))) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"channel_open: struct_size {} is not {}">(
                desc->struct_size,
                static_cast<std::uint32_t>(sizeof(::dftu_channel_desc))));
    }
    bool is_own = false;
    auto full = qualify_port_name(bh.declared_name(),
                                  desc->name ? desc->name : "", &is_own);
    if (!full) return host_fail(ctx, full.error());
    ChannelDecl decl;
    decl.name = std::move(*full);
    decl.declarer = bh.declared_name();
    decl.is_ref = !is_own;
    decl.credits = (desc->field_mask & DFTU_CHANNEL_F_CREDITS)
                       ? desc->credits
                       : common::constants::plugins::CHANNEL_DEFAULT_CREDITS;
    decl.overflow = (desc->field_mask & DFTU_CHANNEL_F_OVERFLOW)
                        ? desc->overflow
                        : DFTU_OVERFLOW_FAIL;
    decl.max_msg_bytes =
        (desc->field_mask & DFTU_CHANNEL_F_MAX_MSG_BYTES)
            ? desc->max_msg_bytes
            : common::constants::plugins::CHANNEL_DEFAULT_MAX_MSG_BYTES;
    decl.task_end = (desc->field_mask & DFTU_CHANNEL_F_TASK_END) != 0;
    decl.sends = (desc->field_mask & DFTU_CHANNEL_F_SENDS) != 0;
    decl.receives = (desc->field_mask & DFTU_CHANNEL_F_RECEIVES) != 0;
    if (decl.credits == 0 || decl.max_msg_bytes == 0 ||
        decl.overflow < DFTU_OVERFLOW_BLOCK ||
        decl.overflow > DFTU_OVERFLOW_DROP_OLDEST) {
        return host_fail(ctx, DFTU_E_INVALID,
                         dstd::fmt<"channel '{}': credits and max_msg_bytes "
                                   "must be above zero and overflow one of "
                                   "Block, Fail or DropOldest">(decl.name));
    }
    for (const ChannelDecl& seen : bh.ports().channels) {
        if (seen.name == decl.name) {
            return host_fail(
                ctx, DFTU_E_INVALID,
                dstd::fmt<"channel '{}' is declared twice by plugin '{}'">(
                    decl.name, decl.declarer));
        }
    }
    *out = dstd::hash::fnv1a_hash(decl.name);
    bh.ports().channels.push_back(std::move(decl));
    return DFTU_OK;
}

dftu_status build_channel_send(::dftu_ctx* ctx, void* h, std::uint64_t,
                               const void*, std::uint32_t, ::dftu_port_status*,
                               ::dftu_task**) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::channel_send");
}

dftu_status build_channel_recv(::dftu_ctx* ctx, void* h, std::uint64_t, void*,
                               std::uint32_t, std::uint32_t*,
                               ::dftu_port_status*, ::dftu_task**) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::channel_recv");
}

dftu_status build_channel_recv_exact(::dftu_ctx* ctx, void* h, std::uint64_t,
                                     void*, std::uint32_t, std::uint32_t*,
                                     ::dftu_port_status*, ::dftu_task**) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::channel_recv_exact");
}

dftu_status build_channel_try_send(::dftu_ctx* ctx, void* h, std::uint64_t,
                                   const void*, std::uint32_t,
                                   ::dftu_port_status*) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::channel_try_send");
}

dftu_status build_channel_close(::dftu_ctx* ctx, void* h, std::uint64_t) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::channel_close");
}

dftu_status build_latest_set(::dftu_ctx* ctx, void* h, const char*, const void*,
                             std::uint32_t, std::uint64_t*) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::latest_set");
}

dftu_status build_latest_get(::dftu_ctx* ctx, void* h, const char*, void*,
                             std::uint32_t, std::uint32_t*, std::uint64_t*,
                             ::dftu_port_status*) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::latest_get");
}

dftu_status build_phase_declare(::dftu_ctx* ctx, void* h, const char* name,
                                ::dftu_merge_op op, ::dftu_type type,
                                std::uint64_t generation,
                                std::uint64_t* out_generation) {
    BuildHost& bh = self_of(h);
    if (name == nullptr || out_generation == nullptr) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "phase_declare: NULL name or out_generation");
    }
    bool is_own = false;
    auto full = qualify_port_name(bh.declared_name(), name, &is_own);
    if (!full) return host_fail(ctx, full.error());
    if (!is_own) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"phase result '{}' belongs to another plugin; only its "
                      "owner declares generations">(*full));
    }
    if (!phase_merge_accepts(op, type)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"phase result '{}': merge {} does not take value type "
                      "{}">(*full, op, type));
    }
    std::uint64_t next = 1;
    for (const PhaseDecl& seen : bh.ports().phases) {
        if (seen.name != *full) continue;
        next = seen.generation + 1;
        if (seen.op != op || seen.type != type) {
            return host_fail(
                ctx, DFTU_E_INVALID,
                dstd::fmt<"phase result '{}': every generation keeps one "
                          "merge and one value type">(*full));
        }
    }
    if (generation != 0 && generation != next) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"phase result '{}': generation {} would skip {}; the "
                      "host assigns generations in order from 1 and the next "
                      "is {}">(*full, generation, next, next));
    }
    bh.ports().phases.push_back({std::move(*full), next, op, type});
    *out_generation = next;
    return DFTU_OK;
}

dftu_status build_phase_contribute(::dftu_ctx* ctx, void* h, const char*,
                                   std::uint64_t, ::dftu_merge_op, ::dftu_type,
                                   const void*, std::uint32_t,
                                   ::dftu_port_status*) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::phase_contribute");
}

dftu_status build_phase_seal(::dftu_ctx* ctx, void* h, const char*,
                             std::uint64_t) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::phase_seal");
}

dftu_status build_phase_result(::dftu_ctx* ctx, void* h, const char*,
                               std::uint64_t, void*, std::uint32_t,
                               std::uint32_t*, ::dftu_port_status*) {
    return denied(ctx, self_of(h), DFTU_SVC_PORTS "::phase_result");
}

const ::dftu_svc_ports g_build_ports = {
    sizeof(::dftu_svc_ports), build_port_key,         build_port_publish,
    build_port_consume,       build_port_try_publish, build_port_try_consume,
    build_channel_open,       build_channel_send,     build_channel_recv,
    build_channel_try_send,   build_channel_close,    build_latest_set,
    build_latest_get,         build_phase_declare,    build_phase_contribute,
    build_phase_seal,         build_phase_result,     build_channel_recv_exact};

dftu_status build_register_provider(::dftu_ctx* ctx, void* h, const char* name,
                                    const ::dftu_source_vt* vt, void* self) {
    BuildHost& bh = self_of(h);
    const dftu_status status = detail::register_plugin_provider(
        ctx, bh.declared_name(), name, vt, self, bh.library(), bh.memory());
    if (status >= 0) bh.registered_providers().emplace_back(name);
    return status;
}

const ::dftu_svc_providers g_build_providers = {sizeof(::dftu_svc_providers),
                                                build_register_provider};

dftu_status build_register_format(::dftu_ctx* ctx, void* h, const char* name,
                                  const ::dftu_file_format_vt* vt, void* self) {
    BuildHost& bh = self_of(h);
    const dftu_status status = detail::register_plugin_file_format(
        ctx, bh.declared_name(), name, vt, self, bh.library(), bh.memory());
    if (status >= 0) bh.registered_file_formats().emplace_back(name);
    return status;
}

dftu_status build_register_sink(::dftu_ctx* ctx, void* h, const char* name,
                                const ::dftu_sink_vt* vt, void* self) {
    BuildHost& bh = self_of(h);
    const dftu_status status = detail::register_plugin_sink(
        ctx, bh.declared_name(), name, vt, self, bh.library());
    if (status >= 0) bh.registered_sinks().emplace_back(name);
    return status;
}

const ::dftu_svc_file_formats g_build_file_formats = {
    sizeof(::dftu_svc_file_formats), build_register_format,
    build_register_sink};

dftu_status build_register_node(::dftu_ctx* ctx, void* h, const char* name,
                                const ::dftu_node_vt* vt, void* self) {
    BuildHost& bh = self_of(h);
    const dftu_status status = detail::register_plugin_node(
        ctx, name, vt, self, bh.library(), bh.memory());
    if (status >= 0) bh.registered_nodes().emplace_back(name);
    return status;
}

const ::dftu_svc_nodes g_build_nodes = {sizeof(::dftu_svc_nodes),
                                        build_register_node};

dftu_status build_register_index(::dftu_ctx* ctx, void* h, const char* name,
                                 const ::dftu_index_extension* vt, void* self) {
    BuildHost& bh = self_of(h);
    const dftu_status status = detail::register_plugin_index_extension(
        ctx, name, vt, self, bh.library());
    if (status >= 0) bh.registered_index_extensions().emplace_back(name);
    return status;
}

const ::dftu_svc_index g_build_index = {sizeof(::dftu_svc_index),
                                        build_register_index};

dftu_status build_task_job(::dftu_ctx* ctx, void* h, const ::dftu_job_desc*,
                           ::dftu_job**) {
    return denied(ctx, self_of(h), DFTU_SVC_TASK "::job");
}

dftu_status build_task_sleep(::dftu_ctx* ctx, void* h, std::uint64_t,
                             ::dftu_job**) {
    return denied(ctx, self_of(h), DFTU_SVC_TASK "::sleep");
}

dftu_status build_task_then(::dftu_ctx* ctx, void* h, ::dftu_job*, ::dftu_job*,
                            ::dftu_job**) {
    return denied(ctx, self_of(h), DFTU_SVC_TASK "::then");
}

dftu_status build_task_all(::dftu_ctx* ctx, void* h, ::dftu_job* const*,
                           std::uint32_t, ::dftu_job**) {
    return denied(ctx, self_of(h), DFTU_SVC_TASK "::all");
}

dftu_status build_task_any(::dftu_ctx* ctx, void* h, ::dftu_job* const*,
                           std::uint32_t, ::dftu_job**) {
    return denied(ctx, self_of(h), DFTU_SVC_TASK "::any");
}

dftu_status build_task_map(::dftu_ctx* ctx, void* h, ::dftu_job*, std::uint32_t,
                           ::dftu_job**) {
    return denied(ctx, self_of(h), DFTU_SVC_TASK "::map");
}

dftu_status build_task_start(::dftu_ctx* ctx, void* h, ::dftu_job*, const void*,
                             void*, ::dftu_task**) {
    return denied(ctx, self_of(h), DFTU_SVC_TASK "::start");
}

dftu_status build_task_spawn(::dftu_ctx* ctx, void* h,
                             dftu_status (*)(::dftu_ctx*, void*), void*,
                             ::dftu_job_where, ::dftu_task**) {
    return denied(ctx, self_of(h), DFTU_SVC_TASK "::spawn");
}

dftu_status build_task_drive(::dftu_ctx* ctx, void* h, ::dftu_step_fn, void*,
                             void (*)(void*), ::dftu_task**) {
    return denied(ctx, self_of(h), DFTU_SVC_TASK "::drive");
}

dftu_status build_task_status(::dftu_ctx* ctx, void* h, const ::dftu_task*) {
    return denied(ctx, self_of(h), DFTU_SVC_TASK "::status");
}

void build_task_discard(void* h, ::dftu_task*) {
    self_of(h).deny(DFTU_SVC_TASK "::discard");
}

// run uses no host state, so a plugin may keep it past the factory: a node's
// cursor pulls its input with it long after the build host is gone.
const ::dftu_svc_task g_build_task = {
    sizeof(::dftu_svc_task), build_task_job,      build_task_sleep,
    build_task_then,         build_task_all,      build_task_any,
    build_task_map,          build_task_start,    build_task_spawn,
    build_task_drive,        build_task_status,   build_task_discard,
    detail::task_run,        detail::task_test,   detail::task_cancel,
    detail::task_retain,     detail::task_release};

const void* build_get_service(void* h, const char* ext_id) {
    if (!ext_id) return nullptr;
    if (dstd::StringView{ext_id} == DFTU_SVC_OPS) return &g_build_ops;
    if (dstd::StringView{ext_id} == DFTU_SVC_AGG) return &g_build_agg;
    if (dstd::StringView{ext_id} == DFTU_SVC_PORTS) return &g_build_ports;
    if (dstd::StringView{ext_id} == DFTU_SVC_PROVIDERS)
        return &g_build_providers;
    if (dstd::StringView{ext_id} == DFTU_SVC_FILE_FORMATS)
        return &g_build_file_formats;
    if (dstd::StringView{ext_id} == DFTU_SVC_NODES) return &g_build_nodes;
    if (dstd::StringView{ext_id} == DFTU_SVC_INDEX) return &g_build_index;
    if (dstd::StringView{ext_id} == DFTU_SVC_TASK) return &g_build_task;
    self_of(h).deny(ext_id);
    return nullptr;
}

const char* build_resolve(void* h, dftu_str, std::uint32_t* out_size) {
    if (out_size) *out_size = 0;
    self_of(h).deny("resolve");
    return nullptr;
}

dftu_str build_intern(void* h, const char*, std::uint32_t) {
    self_of(h).deny("intern");
    return DFTU_STR_NONE;
}

void build_log(void* h, std::uint8_t level, const char* s, std::uint32_t n) {
    const char* name = self_of(h).plugin_name().c_str();
    const char* msg = s ? s : "";
    switch (level) {
        case DFTU_LOG_ERROR:
            DFTRACER_UTILS_LOG_ERROR("[plugin:{}] {}", name,
                                     dstd::StringView(msg, n));
            break;
        case DFTU_LOG_WARN:
            DFTRACER_UTILS_LOG_WARN("[plugin:{}] {}", name,
                                    dstd::StringView(msg, n));
            break;
        case DFTU_LOG_DEBUG:
            DFTRACER_UTILS_LOG_DEBUG("[plugin:{}] {}", name,
                                     dstd::StringView(msg, n));
            break;
        case DFTU_LOG_TRACE:
            DFTRACER_UTILS_LOG_TRACE("[plugin:{}] {}", name,
                                     dstd::StringView(msg, n));
            break;
        default:
            DFTRACER_UTILS_LOG_INFO("[plugin:{}] {}", name,
                                    dstd::StringView(msg, n));
            break;
    }
}

}  // namespace

BuildHost::BuildHost(dstd::String plugin_name,
                     dstd::SharedPtr<PluginLibrary> library)
    : library_(std::move(library)),
      plugin_name_(plugin_name.empty() ? "plugin" : std::move(plugin_name)),
      memory_(dstd::make_shared<PluginMemory>(plugin_name_)) {
    host_.struct_size = sizeof(dftu_plugin_host);
    host_.h = this;
    host_.get_service = build_get_service;
    host_.resolve = build_resolve;
    host_.intern = build_intern;
    host_.log = build_log;
}

void BuildHost::deny(const char* what) {
    if (!denied_.empty()) return;
    denied_ = what ? what : "(null)";
    DFTRACER_UTILS_LOG_ERROR(
        "[plugin:{}] '{}' is not available at load; the factory host offers "
        "registration only",
        plugin_name_, denied_);
}

}  // namespace dftracer::utils::plugins::host
