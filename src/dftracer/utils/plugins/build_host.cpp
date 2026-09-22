#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/build_host.h>
#include <dftracer/utils/plugins/fold_adapter/ext.h>
#include <dftracer/utils/plugins/state_registry.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace dftracer::utils::plugins {
namespace {

BuildHost& self_of(void* h) { return *static_cast<BuildHost*>(h); }

::dftu_error denied_error() {
    return ::dftu_error{dftracer::utils::CORE_DOMAIN.id,
                        static_cast<std::int32_t>(DFTU_COND_UNSUPPORTED),
                        DFTU_COND_UNSUPPORTED,
                        "not available at load: the factory host offers "
                        "registration only"};
}

::dftu_result_series build_ops_run(void* h, const char*,
                                   const ::dftu_series* const*, std::uint32_t,
                                   const ::dftu_op_arg*) {
    self_of(h).deny(DFTU_SVC_OPS "::run");
    return ::dftu_result_series{0, {.err = denied_error()}};
}

::dftu_result_scalar build_ops_run_aggregate(void* h, const char*,
                                             const ::dftu_series* const*,
                                             std::uint32_t,
                                             const ::dftu_op_arg*) {
    self_of(h).deny(DFTU_SVC_OPS "::run_aggregate");
    return ::dftu_result_scalar{0, {.err = denied_error()}};
}

::dftu_result_frame build_ops_run_frame(void* h, const char*,
                                        const ::dftu_dataframe* const*,
                                        std::uint32_t, const ::dftu_op_arg*) {
    self_of(h).deny(DFTU_SVC_OPS "::run_frame");
    return ::dftu_result_frame{0, {.err = denied_error()}};
}

// Registering an op means checking the name is free, so find() is part of the
// registration surface rather than an escape from it.
const ::dftu_op_desc* build_ops_find(void*, const char* name) {
    return ::dftu_op_find(name);
}

int build_ops_register(void* h, const ::dftu_op_desc* desc) {
    int rc = detail::register_plugin_op(desc);
    if (rc == 0) self_of(h).registered_ops().emplace_back(desc->name);
    return rc;
}

::dftu_result_lazyframe build_ops_run_lazy(void* h, const char*,
                                           const ::dftu_lazyframe* const*,
                                           std::uint32_t,
                                           const ::dftu_op_arg*) {
    self_of(h).deny(DFTU_SVC_OPS "::run_lazy");
    return ::dftu_result_lazyframe{0, {.err = denied_error()}};
}

const ::dftu_svc_ops g_build_ops = {
    build_ops_run,  build_ops_run_aggregate, build_ops_run_frame,
    build_ops_find, build_ops_register,      build_ops_run_lazy};

::dftu_agg* build_agg_new(void* h, const char*, const char* const*,
                          std::uint32_t, const ::dftu_agg_col*, std::uint32_t) {
    self_of(h).deny(DFTU_SVC_AGG "::agg_new");
    return nullptr;
}

void build_agg_accumulate(void* h, ::dftu_agg*, const ::dftu_dataframe*) {
    self_of(h).deny(DFTU_SVC_AGG "::agg_accumulate");
}

::dftu_dataframe* build_agg_result(void* h, const char*) {
    self_of(h).deny(DFTU_SVC_AGG "::agg_result");
    return nullptr;
}

int build_register_state(void* h, const ::dftu_state_desc* desc, void* self) {
    BuildHost& bh = self_of(h);
    return register_state_into(bh.states(), desc, self, bh.plugin_name());
}

const ::dftu_svc_agg g_build_agg = {build_agg_new, build_agg_accumulate,
                                    build_agg_result, build_register_state};

// A port key is a pure hash of the name, so wiring a port up is available at
// load; moving bytes through one needs a batch that does not exist yet.
std::uint64_t build_port_key(void*, const char* name) {
    return name ? dftracer::utils::hash::fnv1a_hash(name) : 0;
}

void build_port_publish(void* h, std::uint64_t, const void*, std::uint32_t) {
    self_of(h).deny(DFTU_SVC_PORTS "::publish");
}

const void* build_port_consume(void* h, std::uint64_t, std::uint32_t* out_len) {
    self_of(h).deny(DFTU_SVC_PORTS "::consume");
    if (out_len) *out_len = 0;
    return nullptr;
}

const ::dftu_svc_ports g_build_ports = {build_port_key, build_port_publish,
                                        build_port_consume};

int build_register_provider(void* h, const char* name,
                            const ::dftu_source_vt* vt, void* self) {
    BuildHost& bh = self_of(h);
    int rc = detail::register_plugin_provider(name, vt, self);
    if (rc == 0) bh.registered_providers().emplace_back(name);
    return rc;
}

const ::dftu_svc_providers g_build_providers = {build_register_provider};

const void* build_get_service(void* h, const char* ext_id) {
    if (!ext_id) return nullptr;
    if (std::string_view{ext_id} == DFTU_SVC_OPS) return &g_build_ops;
    if (std::string_view{ext_id} == DFTU_SVC_AGG) return &g_build_agg;
    if (std::string_view{ext_id} == DFTU_SVC_PORTS) return &g_build_ports;
    if (std::string_view{ext_id} == DFTU_SVC_PROVIDERS)
        return &g_build_providers;
    self_of(h).deny(ext_id);
    return nullptr;
}

const char* build_resolve(void* h, dftu_str, std::uint32_t* out_len) {
    if (out_len) *out_len = 0;
    self_of(h).deny("resolve");
    return nullptr;
}

dftu_str build_intern(void* h, const char*, std::uint32_t) {
    self_of(h).deny("intern");
    return DFTU_STR_NONE;
}

void build_log(void* h, std::uint8_t level, const char* s, std::uint32_t n) {
    const char* name = self_of(h).plugin_name().c_str();
    const int len = static_cast<int>(n);
    const char* msg = s ? s : "";
    switch (level) {
        case DFTU_LOG_ERROR:
            DFTRACER_UTILS_LOG_ERROR("[plugin:%s] %.*s", name, len, msg);
            break;
        case DFTU_LOG_WARN:
            DFTRACER_UTILS_LOG_WARN("[plugin:%s] %.*s", name, len, msg);
            break;
        case DFTU_LOG_DEBUG:
            DFTRACER_UTILS_LOG_DEBUG("[plugin:%s] %.*s", name, len, msg);
            break;
        case DFTU_LOG_TRACE:
            DFTRACER_UTILS_LOG_TRACE("[plugin:%s] %.*s", name, len, msg);
            break;
        default:
            DFTRACER_UTILS_LOG_INFO("[plugin:%s] %.*s", name, len, msg);
            break;
    }
}

}  // namespace

BuildHost::BuildHost(std::string plugin_name)
    : plugin_name_(plugin_name.empty() ? "plugin" : std::move(plugin_name)) {
    host_.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
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
        "[plugin:%s] '%s' is not available at load; the factory host offers "
        "registration only",
        plugin_name_.c_str(), denied_.c_str());
}

}  // namespace dftracer::utils::plugins
