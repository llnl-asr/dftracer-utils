// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/plugin_abi.h>
#include <dftracer/utils/plugins/host/port_hub.h>
#include <dftracer/utils/plugins/task_abi.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dftracer::utils::plugins::host {
namespace {

PluginFold& fold_of(void* h) { return *static_cast<PluginFold*>(h); }

dstd::Vector<std::byte> copy_bytes(const void* data, std::uint32_t len) {
    const auto* p = static_cast<const std::byte*>(data);
    return dstd::Vector<std::byte>(p, p + len);
}

// The hub of the scan, or the failure of a call that needs one.
PortHub* hub_of(::dftu_ctx* ctx, PluginFold& fold, const char* what,
                dftu_status* failed) {
    PortHub* hub = fold.port_hub();
    if (hub == nullptr) {
        *failed =
            host_fail(ctx, DFTU_E_UNSUPPORTED,
                      dstd::fmt<"{}: this host has no port hub for the scan">(
                          dstd::StringView(what)));
    }
    return hub;
}

// The name the plugin passed, qualified and matched to its slots once.
PortNames::Entry* named(::dftu_ctx* ctx, PluginFold& fold, const char* name,
                        dftu_status* failed) {
    auto entry =
        fold.port_names()->resolve(name != nullptr ? name : dstd::StringView());
    if (!entry) {
        *failed = host_fail(ctx, entry.error());
        return nullptr;
    }
    return *entry;
}

std::uint64_t host_port_key(void*, const char* cap_id) {
    return cap_id ? dstd::hash::fnv1a_hash(cap_id) : 0;
}

void host_port_publish(void* h, std::uint64_t key, const void* data,
                       std::uint32_t len) {
    fold_of(h).port_publish(key, data, len);
}

const void* host_port_consume(void* h, std::uint64_t key,
                              std::uint32_t* out_size) {
    return fold_of(h).port_consume(key, out_size);
}

dftu_status host_try_publish(::dftu_ctx* ctx, void* h, std::uint64_t key,
                             const void* data, std::uint32_t len,
                             ::dftu_port_status* out_status) {
    if (out_status == nullptr || (data == nullptr && len != 0)) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "try_publish: NULL out_status or data");
    }
    PluginFold& fold = fold_of(h);
    if (!fold.has_port_bus()) {
        *out_status = DFTU_PORT_CLOSED;
        return DFTU_OK;
    }
    if (fold.port_bus_bytes() + len >
        common::constants::plugins::PORT_BATCH_BYTES) {
        *out_status = DFTU_PORT_FULL;
        return DFTU_OK;
    }
    fold.port_publish(key, data, len);
    *out_status = DFTU_PORT_OK;
    return DFTU_OK;
}

dftu_status host_try_consume(::dftu_ctx* ctx, void* h, std::uint64_t key,
                             void* buf, std::uint32_t cap,
                             std::uint32_t* out_len,
                             ::dftu_port_status* out_status) {
    if (out_status == nullptr || out_len == nullptr ||
        (buf == nullptr && cap != 0)) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "try_consume: NULL out_status, out_len or buf");
    }
    std::uint32_t size = 0;
    const void* data = fold_of(h).port_consume(key, &size);
    *out_len = size;
    if (data == nullptr) {
        *out_status = DFTU_PORT_NOT_READY;
    } else if (size > cap) {
        *out_status = DFTU_PORT_TOO_BIG;
    } else {
        if (size != 0) std::memcpy(buf, data, size);
        *out_status = DFTU_PORT_OK;
    }
    return DFTU_OK;
}

dftu_status host_channel_open(::dftu_ctx* ctx, void* h,
                              const ::dftu_channel_desc* desc,
                              std::uint64_t* out) {
    if (desc == nullptr || out == nullptr || desc->name == nullptr) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "channel_open: NULL descriptor, name or out");
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
    PluginFold& fold = fold_of(h);
    dftu_status failed = DFTU_OK;
    PortHub* hub = hub_of(ctx, fold, "channel_open", &failed);
    if (hub == nullptr) return failed;
    PortNames::Entry* entry = named(ctx, fold, desc->name, &failed);
    if (entry == nullptr) return failed;
    const dstd::String& full = entry->full;
    const dstd::Optional<std::uint64_t> handle = hub->find_channel(full);
    if (!handle) {
        return host_fail(
            ctx, DFTU_E_NOT_FOUND,
            dstd::fmt<"channel '{}' was not declared in a plugin factory">(
                full));
    }
    *out = *handle;
    return DFTU_OK;
}

PortChannel* channel_of(::dftu_ctx* ctx, PluginFold& fold, std::uint64_t handle,
                        const char* what, dftu_status* failed) {
    PortHub* hub = hub_of(ctx, fold, what, failed);
    if (hub == nullptr) return nullptr;
    PortChannel* channel = hub->channel(handle);
    if (channel == nullptr) {
        *failed = host_fail(ctx, DFTU_E_NOT_FOUND,
                            dstd::fmt<"{}: no channel has this handle">(
                                dstd::StringView(what)));
    }
    return channel;
}

dftu_status host_channel_send(::dftu_ctx* ctx, void* h, std::uint64_t handle,
                              const void* data, std::uint32_t len,
                              ::dftu_port_status* out_status,
                              ::dftu_task** out_task) {
    if (out_status == nullptr || out_task == nullptr ||
        (data == nullptr && len != 0)) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "channel_send: NULL out_status, out_task or data");
    }
    dftu_status failed = DFTU_OK;
    PortChannel* channel =
        channel_of(ctx, fold_of(h), handle, "channel_send", &failed);
    if (channel == nullptr) return failed;
    if (channel->overflow() == DFTU_OVERFLOW_BLOCK && in_batch_callback()) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"channel_send: a blocking send on channel '{}' was "
                      "started from on_batch or transform; start it from a "
                      "task and declare DFTU_CHANNEL_F_TASK_END">(
                channel->name()));
    }
    // The queue owns the bytes of a message, since the receiver runs later.
    *out_task = plugins::make_abi_task(
        copy_bytes(data, len),
        [channel, out_status](plugins::AbiTask* task) {
            return channel->send_now(task->fast->payload, out_status);
        },
        [channel, out_status](plugins::AbiTask* task) {
            return channel->send(std::move(task->fast->payload), out_status);
        });
    return DFTU_OK;
}

dftu_status channel_recv_with(::dftu_ctx* ctx, void* h, std::uint64_t handle,
                              void* buf, std::uint32_t cap,
                              std::uint32_t* out_len,
                              ::dftu_port_status* out_status,
                              ::dftu_task** out_task, bool exact) {
    if (out_status == nullptr || out_len == nullptr || out_task == nullptr ||
        (buf == nullptr && cap != 0)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            "channel_recv: NULL out_status, out_len, out_task or buf");
    }
    dftu_status failed = DFTU_OK;
    PortChannel* channel =
        channel_of(ctx, fold_of(h), handle, "channel_recv", &failed);
    if (channel == nullptr) return failed;
    *out_task = plugins::make_abi_task(
        dstd::Vector<std::byte>{},
        [channel, buf, cap, out_len, out_status, exact](plugins::AbiTask*) {
            return channel->recv_now(buf, cap, out_len, out_status, exact);
        },
        [channel, buf, cap, out_len, out_status, exact](plugins::AbiTask*) {
            return channel->recv(buf, cap, out_len, out_status, exact);
        });
    return DFTU_OK;
}

dftu_status host_channel_recv(::dftu_ctx* ctx, void* h, std::uint64_t handle,
                              void* buf, std::uint32_t cap,
                              std::uint32_t* out_len,
                              ::dftu_port_status* out_status,
                              ::dftu_task** out_task) {
    return channel_recv_with(ctx, h, handle, buf, cap, out_len, out_status,
                             out_task, false);
}

dftu_status host_channel_recv_exact(::dftu_ctx* ctx, void* h,
                                    std::uint64_t handle, void* buf,
                                    std::uint32_t cap, std::uint32_t* out_len,
                                    ::dftu_port_status* out_status,
                                    ::dftu_task** out_task) {
    return channel_recv_with(ctx, h, handle, buf, cap, out_len, out_status,
                             out_task, true);
}

dftu_status host_channel_try_send(::dftu_ctx* ctx, void* h,
                                  std::uint64_t handle, const void* data,
                                  std::uint32_t len,
                                  ::dftu_port_status* out_status) {
    if (out_status == nullptr || (data == nullptr && len != 0)) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "channel_try_send: NULL out_status or data");
    }
    dftu_status failed = DFTU_OK;
    PortChannel* channel =
        channel_of(ctx, fold_of(h), handle, "channel_try_send", &failed);
    if (channel == nullptr) return failed;
    *out_status = channel->try_send(copy_bytes(data, len));
    return DFTU_OK;
}

dftu_status host_channel_close(::dftu_ctx* ctx, void* h, std::uint64_t handle) {
    dftu_status failed = DFTU_OK;
    PortChannel* channel =
        channel_of(ctx, fold_of(h), handle, "channel_close", &failed);
    if (channel == nullptr) return failed;
    channel->close();
    return DFTU_OK;
}

dftu_status host_latest_set(::dftu_ctx* ctx, void* h, const char* name,
                            const void* data, std::uint32_t len,
                            std::uint64_t* out_generation) {
    if (out_generation == nullptr || (data == nullptr && len != 0)) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "latest_set: NULL out_generation or data");
    }
    if (len > common::constants::plugins::PORT_VALUE_MAX_BYTES) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "latest_set: the value is over the port value limit");
    }
    PluginFold& fold = fold_of(h);
    dftu_status failed = DFTU_OK;
    if (hub_of(ctx, fold, "latest_set", &failed) == nullptr) return failed;
    PortNames::Entry* entry = named(ctx, fold, name, &failed);
    if (entry == nullptr) return failed;
    *out_generation = fold.port_names()->latest(entry)->set(data, len);
    return DFTU_OK;
}

dftu_status host_latest_get(::dftu_ctx* ctx, void* h, const char* name,
                            void* buf, std::uint32_t cap,
                            std::uint32_t* out_len,
                            std::uint64_t* out_generation,
                            ::dftu_port_status* out_status) {
    if (out_len == nullptr || out_generation == nullptr ||
        out_status == nullptr || (buf == nullptr && cap != 0)) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "latest_get: NULL out_len, out_generation, "
                         "out_status or buf");
    }
    PluginFold& fold = fold_of(h);
    dftu_status failed = DFTU_OK;
    if (hub_of(ctx, fold, "latest_get", &failed) == nullptr) return failed;
    PortNames::Entry* entry = named(ctx, fold, name, &failed);
    if (entry == nullptr) return failed;
    *out_status = fold.port_names()->latest(entry)->get(buf, cap, out_len,
                                                        out_generation);
    return DFTU_OK;
}

dftu_status undeclared_phase(::dftu_ctx* ctx, const PortNames::Entry* entry,
                             std::uint64_t generation) {
    return host_fail(
        ctx, DFTU_E_NOT_FOUND,
        dstd::fmt<"phase result '{}' generation {} was not declared">(
            entry->full, generation));
}

dftu_status host_phase_declare(::dftu_ctx* ctx, void*, const char*,
                               ::dftu_merge_op, ::dftu_type, std::uint64_t,
                               std::uint64_t*) {
    return host_fail(ctx, DFTU_E_UNSUPPORTED,
                     "phase_declare: generations are declared in the plugin "
                     "factory, before the plan is settled");
}

dftu_status host_phase_contribute(::dftu_ctx* ctx, void* h, const char* name,
                                  std::uint64_t generation, ::dftu_merge_op op,
                                  ::dftu_type type, const void* data,
                                  std::uint32_t len,
                                  ::dftu_port_status* out_status) {
    if (out_status == nullptr || (data == nullptr && len != 0)) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "phase_contribute: NULL out_status or data");
    }
    PluginFold& fold = fold_of(h);
    dftu_status failed = DFTU_OK;
    if (hub_of(ctx, fold, "phase_contribute", &failed) == nullptr)
        return failed;
    PortNames::Entry* entry = named(ctx, fold, name, &failed);
    if (entry == nullptr) return failed;
    if (entry->phase == nullptr)
        return undeclared_phase(ctx, entry, generation);
    auto merged = entry->phase->contribute(generation, op, type, data, len);
    if (!merged) return host_fail(ctx, merged.error());
    *out_status = *merged;
    return DFTU_OK;
}

dftu_status host_phase_seal(::dftu_ctx* ctx, void* h, const char* name,
                            std::uint64_t generation) {
    PluginFold& fold = fold_of(h);
    dftu_status failed = DFTU_OK;
    if (hub_of(ctx, fold, "phase_seal", &failed) == nullptr) return failed;
    PortNames::Entry* entry = named(ctx, fold, name, &failed);
    if (entry == nullptr) return failed;
    if (entry->phase == nullptr)
        return undeclared_phase(ctx, entry, generation);
    auto sealed = entry->phase->seal(generation);
    if (!sealed) return host_fail(ctx, sealed.error());
    return DFTU_OK;
}

dftu_status host_phase_result(::dftu_ctx* ctx, void* h, const char* name,
                              std::uint64_t generation, void* buf,
                              std::uint32_t cap, std::uint32_t* out_len,
                              ::dftu_port_status* out_status) {
    if (out_len == nullptr || out_status == nullptr ||
        (buf == nullptr && cap != 0)) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "phase_result: NULL out_len, out_status or buf");
    }
    PluginFold& fold = fold_of(h);
    dftu_status failed = DFTU_OK;
    if (hub_of(ctx, fold, "phase_result", &failed) == nullptr) return failed;
    PortNames::Entry* entry = named(ctx, fold, name, &failed);
    if (entry == nullptr) return failed;
    if (entry->phase == nullptr)
        return undeclared_phase(ctx, entry, generation);
    auto got = entry->phase->result(generation, buf, cap, out_len);
    if (!got) return host_fail(ctx, got.error());
    *out_status = *got;
    return DFTU_OK;
}

const ::dftu_svc_ports g_ports = {
    sizeof(::dftu_svc_ports), host_port_key,      host_port_publish,
    host_port_consume,        host_try_publish,   host_try_consume,
    host_channel_open,        host_channel_send,  host_channel_recv,
    host_channel_try_send,    host_channel_close, host_latest_set,
    host_latest_get,          host_phase_declare, host_phase_contribute,
    host_phase_seal,          host_phase_result,  host_channel_recv_exact};

}  // namespace

const void* detail::ports_ext_vtable() { return &g_ports; }

}  // namespace dftracer::utils::plugins::host
