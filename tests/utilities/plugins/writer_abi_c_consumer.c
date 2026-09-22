/* A real C consumer of the dftu_svc_writer ABI: create a parallel writer, then
 * open/chunk/close it. Like io, the calls return tasks the C++ harness
 * sequences with co_await. */
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>

static const dftu_svc_writer* wr_of(const dftu_plugin_host* h) {
    return (const dftu_svc_writer*)h->get_service(h->h, DFTU_SVC_WRITER);
}

dftu_writer* dftu_test_writer_create(const dftu_plugin_host* h,
                                     const char* path) {
    const dftu_svc_writer* w = wr_of(h);
    return w ? w->writer_create(h->h, path, 1, 0) : NULL;
}

dftu_task* dftu_test_writer_open(const dftu_plugin_host* h, dftu_writer* w) {
    return wr_of(h)->writer_open(h->h, w);
}

dftu_task* dftu_test_writer_chunk(const dftu_plugin_host* h, dftu_writer* w,
                                  const void* data, uint64_t len) {
    return wr_of(h)->writer_chunk(h->h, w, 0, data, len);
}

dftu_task* dftu_test_writer_close(const dftu_plugin_host* h, dftu_writer* w) {
    return wr_of(h)->writer_close(h->h, w);
}
