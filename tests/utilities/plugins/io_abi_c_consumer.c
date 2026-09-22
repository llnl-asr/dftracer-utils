/* A real C consumer of the dftu_io ABI. Each call returns a dftu_task; since
 * C has no coroutines, the C++ harness co_awaits them in sequence (the fd from
 * open flows into write/pread/close). This proves even the async I/O vtable is
 * C-callable. */
#include <dftracer/utils/plugins/abi.h>
#include <fcntl.h>
#include <stdint.h>

static const dftu_io* io_of(const dftu_plugin_host* h) {
    return (const dftu_io*)h->get_service(h->h, DFTU_SVC_IO);
}

dftu_task* dftu_test_io_open(const dftu_plugin_host* h, const char* path,
                             int* fd) {
    const dftu_io* io = io_of(h);
    if (!io) {
        *fd = -1;
        return NULL;
    }
    return io->open(h->h, path, O_CREAT | O_RDWR | O_TRUNC, 0644, fd);
}

dftu_task* dftu_test_io_write(const dftu_plugin_host* h, int fd,
                              const void* buf, uint64_t len, int64_t* n) {
    return io_of(h)->write(h->h, fd, buf, len, n);
}

dftu_task* dftu_test_io_pread(const dftu_plugin_host* h, int fd, void* buf,
                              uint64_t len, uint64_t off, int64_t* n) {
    return io_of(h)->pread(h->h, fd, buf, len, off, n);
}

dftu_task* dftu_test_io_close(const dftu_plugin_host* h, int fd, int* rc) {
    return io_of(h)->close(h->h, fd, rc);
}
