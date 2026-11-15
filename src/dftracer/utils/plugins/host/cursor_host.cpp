// SPDX-License-Identifier: MIT
#include <dftracer/utils/plugins/host/cursor_host.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>

#include <cstring>

namespace dftracer::utils::plugins::host {

namespace {

CursorHost& cursor_of(void* h) { return *static_cast<CursorHost*>(h); }

// The services that belong to one scan (task, fileio, agg, result, ports, duql,
// trace) are not here: a cursor has no fold to keep their state in.
const void* cursor_get_service(void*, const char* ext_id) {
    if (!ext_id) return nullptr;
    if (std::strcmp(ext_id, DFTU_SVC_MEMORY) == 0)
        return detail::cursor_memory_ext_vtable();
    return detail::stateless_service(ext_id);
}

const char* cursor_resolve(void*, dftu_str, std::uint32_t* out_size) {
    if (out_size) *out_size = 0;
    return nullptr;
}

dftu_str cursor_intern(void*, const char*, std::uint32_t) {
    return DFTU_STR_NONE;
}

void cursor_log(void* h, std::uint8_t level, const char* s, std::uint32_t n) {
    detail::log_plugin_line(cursor_of(h).memory().name(), level, s, n);
}

}  // namespace

CursorHost::CursorHost(dstd::SharedPtr<PluginMemory> memory,
                       dstd::StringView fallback_name,
                       std::uint64_t memory_budget)
    : memory_(std::move(memory)), memory_budget_(memory_budget) {
    if (!memory_) memory_ = dstd::make_shared<PluginMemory>(fallback_name);
    host_.struct_size = sizeof(dftu_plugin_host);
    host_.h = this;
    host_.get_service = cursor_get_service;
    host_.resolve = cursor_resolve;
    host_.intern = cursor_intern;
    host_.log = cursor_log;
}

CursorHost::~CursorHost() { memory_->release_tagged(this); }

}  // namespace dftracer::utils::plugins::host
