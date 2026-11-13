// SPDX-License-Identifier: MIT
/* A real C consumer of the dftu_svc_writer ABI: create a parallel writer, then
 * open/chunk/close it. Like io, the calls return tasks the C++ harness
 * sequences with co_await. */
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <utilities/plugins/abi_c_consumers.h>

static const dftu_svc_writer* wr_of(const dftu_plugin_host* h) {
    return (const dftu_svc_writer*)h->get_service(h->h, DFTU_SVC_WRITER);
}

dftu_status dftu_test_writer_create(dftu_ctx* ctx, const dftu_plugin_host* h,
                                    const char* path, dftu_writer** out) {
    return wr_of(h)->writer_create(ctx, h->h, path, 1, 0, out);
}

dftu_status dftu_test_writer_open(dftu_ctx* ctx, const dftu_plugin_host* h,
                                  dftu_writer* w, dftu_task** t) {
    return wr_of(h)->writer_open(ctx, h->h, w, t);
}

dftu_status dftu_test_writer_chunk(dftu_ctx* ctx, const dftu_plugin_host* h,
                                   dftu_writer* w, const void* data,
                                   uint64_t len, dftu_task** t) {
    return wr_of(h)->writer_chunk(ctx, h->h, w, 0, data, len, t);
}

dftu_status dftu_test_writer_close(dftu_ctx* ctx, const dftu_plugin_host* h,
                                   dftu_writer* w, dftu_task** t) {
    return wr_of(h)->writer_close(ctx, h->h, w, t);
}
