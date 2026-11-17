// SPDX-License-Identifier: MIT
/* A real C consumer of the dftu.svc.ports ABI: this translation unit is
 * compiled by the C compiler and sees only <dftracer/utils/plugins/abi.h>, so
 * it proves the channel, latest and phase members are callable from C. Each
 * function is one service call; the C++ test sequences them. */
#include <dftracer/utils/plugins/abi.h>
#include <plugins/abi_c_consumers.h>
#include <stdint.h>
#include <string.h>

static const dftu_svc_ports* ports_of(const dftu_plugin_host* h) {
    return (const dftu_svc_ports*)h->get_service(h->h, DFTU_SVC_PORTS);
}

dftu_status dftu_test_ports_open(dftu_ctx* ctx, const dftu_plugin_host* h,
                                 const char* name, uint32_t field_mask,
                                 uint32_t credits, dftu_overflow overflow,
                                 uint32_t max_msg_bytes, uint64_t* out) {
    dftu_channel_desc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d);
    d.field_mask = field_mask;
    d.name = name;
    d.credits = credits;
    d.overflow = overflow;
    d.max_msg_bytes = max_msg_bytes;
    return ports_of(h)->channel_open(ctx, h->h, &d, out);
}

dftu_status dftu_test_ports_send(dftu_ctx* ctx, const dftu_plugin_host* h,
                                 uint64_t ch, const void* data, uint32_t len,
                                 dftu_port_status* st, dftu_task** t) {
    return ports_of(h)->channel_send(ctx, h->h, ch, data, len, st, t);
}

dftu_status dftu_test_ports_recv(dftu_ctx* ctx, const dftu_plugin_host* h,
                                 uint64_t ch, void* buf, uint32_t cap,
                                 uint32_t* len, dftu_port_status* st,
                                 dftu_task** t) {
    return ports_of(h)->channel_recv(ctx, h->h, ch, buf, cap, len, st, t);
}

dftu_status dftu_test_ports_try_send(dftu_ctx* ctx, const dftu_plugin_host* h,
                                     uint64_t ch, const void* data,
                                     uint32_t len, dftu_port_status* st) {
    return ports_of(h)->channel_try_send(ctx, h->h, ch, data, len, st);
}

dftu_status dftu_test_ports_close(dftu_ctx* ctx, const dftu_plugin_host* h,
                                  uint64_t ch) {
    return ports_of(h)->channel_close(ctx, h->h, ch);
}

dftu_status dftu_test_ports_latest_set(dftu_ctx* ctx, const dftu_plugin_host* h,
                                       const char* name, const void* data,
                                       uint32_t len, uint64_t* gen) {
    return ports_of(h)->latest_set(ctx, h->h, name, data, len, gen);
}

dftu_status dftu_test_ports_latest_get(dftu_ctx* ctx, const dftu_plugin_host* h,
                                       const char* name, void* buf,
                                       uint32_t cap, uint32_t* len,
                                       uint64_t* gen, dftu_port_status* st) {
    return ports_of(h)->latest_get(ctx, h->h, name, buf, cap, len, gen, st);
}

dftu_status dftu_test_ports_phase_declare(dftu_ctx* ctx,
                                          const dftu_plugin_host* h,
                                          const char* name, dftu_merge_op op,
                                          dftu_type type, uint64_t want,
                                          uint64_t* got) {
    return ports_of(h)->phase_declare(ctx, h->h, name, op, type, want, got);
}

dftu_status dftu_test_ports_phase_contribute(dftu_ctx* ctx,
                                             const dftu_plugin_host* h,
                                             const char* name, uint64_t gen,
                                             dftu_merge_op op, dftu_type type,
                                             const void* data, uint32_t len,
                                             dftu_port_status* st) {
    return ports_of(h)->phase_contribute(ctx, h->h, name, gen, op, type, data,
                                         len, st);
}

dftu_status dftu_test_ports_phase_seal(dftu_ctx* ctx, const dftu_plugin_host* h,
                                       const char* name, uint64_t gen) {
    return ports_of(h)->phase_seal(ctx, h->h, name, gen);
}

dftu_status dftu_test_ports_phase_result(dftu_ctx* ctx,
                                         const dftu_plugin_host* h,
                                         const char* name, uint64_t gen,
                                         void* buf, uint32_t cap, uint32_t* len,
                                         dftu_port_status* st) {
    return ports_of(h)->phase_result(ctx, h->h, name, gen, buf, cap, len, st);
}

dftu_status dftu_test_ports_try_publish(dftu_ctx* ctx,
                                        const dftu_plugin_host* h, uint64_t key,
                                        const void* data, uint32_t len,
                                        dftu_port_status* st) {
    return ports_of(h)->try_publish(ctx, h->h, key, data, len, st);
}

dftu_status dftu_test_ports_try_consume(dftu_ctx* ctx,
                                        const dftu_plugin_host* h, uint64_t key,
                                        void* buf, uint32_t cap, uint32_t* len,
                                        dftu_port_status* st) {
    return ports_of(h)->try_consume(ctx, h->h, key, buf, cap, len, st);
}

uint64_t dftu_test_ports_key(const dftu_plugin_host* h, const char* name) {
    return ports_of(h)->port_key(h->h, name);
}
