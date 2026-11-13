// SPDX-License-Identifier: MIT
/* A real C consumer of the dftu_io ABI. Each call returns a dftu_task; since
 * C has no coroutines, the C++ harness co_awaits them in sequence (the fd from
 * open flows into write/pread/close). This proves even the async I/O vtable is
 * C-callable. */
#include <dftracer/utils/plugins/abi.h>
#include <fcntl.h>
#include <stdint.h>
#include <utilities/plugins/abi_c_consumers.h>

static const dftu_io* io_of(const dftu_plugin_host* h) {
    return (const dftu_io*)h->get_service(h->h, DFTU_SVC_IO);
}

dftu_status dftu_test_io_open(dftu_ctx* ctx, const dftu_plugin_host* h,
                              const char* path, int* fd, dftu_task** t) {
    const dftu_io* io = io_of(h);
    if (!io) {
        *fd = -1;
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no io service");
    }
    return io->open(ctx, h->h, path, O_CREAT | O_RDWR | O_TRUNC, 0644, fd, t);
}

/* Opens read-only, so a missing path fails the task with DFTU_E_NOT_FOUND. */
dftu_status dftu_test_io_open_existing(dftu_ctx* ctx, const dftu_plugin_host* h,
                                       const char* path, int* fd,
                                       dftu_task** t) {
    return io_of(h)->open(ctx, h->h, path, O_RDONLY, 0, fd, t);
}

dftu_status dftu_test_io_write(dftu_ctx* ctx, const dftu_plugin_host* h, int fd,
                               const void* buf, uint64_t len, int64_t* n,
                               dftu_task** t) {
    return io_of(h)->write(ctx, h->h, fd, buf, len, n, t);
}

dftu_status dftu_test_io_pread(dftu_ctx* ctx, const dftu_plugin_host* h, int fd,
                               void* buf, uint64_t len, uint64_t off,
                               int64_t* n, dftu_task** t) {
    return io_of(h)->pread(ctx, h->h, fd, buf, len, off, n, t);
}

dftu_status dftu_test_io_close(dftu_ctx* ctx, const dftu_plugin_host* h, int fd,
                               dftu_task** t) {
    return io_of(h)->close(ctx, h->h, fd, t);
}
