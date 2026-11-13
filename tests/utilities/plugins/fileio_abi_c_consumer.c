// SPDX-License-Identifier: MIT
/* A real C consumer of the dftu_svc_fileio ABI: the parallel writer, the line
 * and chunk readers, the gzip line writer, whole-file gzip and the file
 * helpers. Like io, the async calls return tasks the C++ harness sequences with
 * co_await. */
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <utilities/plugins/abi_c_consumers.h>

static const dftu_svc_fileio* fio_of(const dftu_plugin_host* h) {
    return (const dftu_svc_fileio*)h->get_service(h->h, DFTU_SVC_FILEIO);
}

dftu_status dftu_test_writer_create(dftu_ctx* ctx, const dftu_plugin_host* h,
                                    const char* path, dftu_writer** out) {
    return fio_of(h)->writer_create(ctx, h->h, path, 1, 0, out);
}

dftu_status dftu_test_writer_open(dftu_ctx* ctx, const dftu_plugin_host* h,
                                  dftu_writer* w, dftu_task** t) {
    return fio_of(h)->writer_open(ctx, h->h, w, t);
}

dftu_status dftu_test_writer_chunk(dftu_ctx* ctx, const dftu_plugin_host* h,
                                   dftu_writer* w, const void* data,
                                   uint64_t len, dftu_task** t) {
    return fio_of(h)->writer_chunk(ctx, h->h, w, 0, data, len, t);
}

dftu_status dftu_test_writer_close(dftu_ctx* ctx, const dftu_plugin_host* h,
                                   dftu_writer* w, dftu_task** t) {
    return fio_of(h)->writer_close(ctx, h->h, w, t);
}

dftu_status dftu_test_merge_shards(dftu_ctx* ctx, const dftu_plugin_host* h,
                                   const char* target, const char* a,
                                   const char* b, dftu_task** t) {
    const char* shards[2];
    shards[0] = a;
    shards[1] = b;
    return fio_of(h)->merge_shards(ctx, h->h, target, shards, 2, t);
}

dftu_status dftu_test_lines_open(dftu_ctx* ctx, const dftu_plugin_host* h,
                                 const char* path, uint64_t start_line,
                                 uint64_t end_line, dftu_line_reader** out) {
    return fio_of(h)->lines_open(ctx, h->h, path, start_line, end_line, out);
}

dftu_status dftu_test_lines_next(dftu_ctx* ctx, const dftu_plugin_host* h,
                                 dftu_line_reader* r, uint32_t max_lines,
                                 dftu_line_batch* out, dftu_task** t) {
    return fio_of(h)->lines_next(ctx, h->h, r, max_lines, out, t);
}

void dftu_test_lines_close(const dftu_plugin_host* h, dftu_line_reader* r) {
    fio_of(h)->lines_close(h->h, r);
}

dftu_status dftu_test_chunks_open(dftu_ctx* ctx, const dftu_plugin_host* h,
                                  const char* path, uint64_t chunk_size,
                                  dftu_chunk_reader** out) {
    return fio_of(h)->chunks_open(ctx, h->h, path, chunk_size, out);
}

dftu_status dftu_test_chunks_next(dftu_ctx* ctx, const dftu_plugin_host* h,
                                  dftu_chunk_reader* r, dftu_bytes* out,
                                  dftu_task** t) {
    return fio_of(h)->chunks_next(ctx, h->h, r, out, t);
}

void dftu_test_chunks_close(const dftu_plugin_host* h, dftu_chunk_reader* r) {
    fio_of(h)->chunks_close(h->h, r);
}

dftu_status dftu_test_gz_open(dftu_ctx* ctx, const dftu_plugin_host* h,
                              const char* path, uint64_t member_size,
                              dftu_gz_writer** out, dftu_task** t) {
    dftu_gz_options opts;
    opts.struct_size = (uint32_t)sizeof(opts);
    opts.level = 0;
    opts.member_size = member_size;
    opts.workers = 0;
    return fio_of(h)->gz_open(ctx, h->h, path, &opts, out, t);
}

dftu_status dftu_test_gz_append(dftu_ctx* ctx, const dftu_plugin_host* h,
                                dftu_gz_writer* w, const void* data,
                                uint64_t len) {
    return fio_of(h)->gz_append(ctx, h->h, w, data, len);
}

dftu_status dftu_test_gz_close(dftu_ctx* ctx, const dftu_plugin_host* h,
                               dftu_gz_writer* w, dftu_gz_summary* out,
                               dftu_task** t) {
    return fio_of(h)->gz_close(ctx, h->h, w, out, t);
}

dftu_status dftu_test_compress(dftu_ctx* ctx, const dftu_plugin_host* h,
                               const char* in_path, const char* out_path,
                               uint64_t* out_size, dftu_task** t) {
    return fio_of(h)->compress(ctx, h->h, in_path, out_path, 0, 0, out_size, t);
}

dftu_status dftu_test_decompress(dftu_ctx* ctx, const dftu_plugin_host* h,
                                 const char* in_path, const char* out_path,
                                 uint64_t* out_size, dftu_task** t) {
    return fio_of(h)->decompress(ctx, h->h, in_path, out_path, out_size, t);
}

dftu_status dftu_test_logical_path(dftu_ctx* ctx, const dftu_plugin_host* h,
                                   const char* path, char* buf, uint64_t cap) {
    dftu_bytes out;
    uint64_t i;
    dftu_status st = fio_of(h)->logical_path(ctx, h->h, path, &out);
    if (st < 0) return st;
    for (i = 0; i < out.len && i + 1 < cap; ++i)
        buf[i] = ((const char*)out.data)[i];
    buf[i] = '\0';
    if (out.free_fn) out.free_fn((void*)(uintptr_t)out.data, out.ud);
    return DFTU_OK;
}

dftu_status dftu_test_mtime(dftu_ctx* ctx, const dftu_plugin_host* h,
                            const char* path, int64_t* out) {
    return fio_of(h)->mtime(ctx, h->h, path, out);
}

dftu_status dftu_test_size(dftu_ctx* ctx, const dftu_plugin_host* h,
                           const char* path, uint64_t* out) {
    return fio_of(h)->size(ctx, h->h, path, out);
}

dftu_status dftu_test_sampled_hash(dftu_ctx* ctx, const dftu_plugin_host* h,
                                   const char* path, uint64_t* out) {
    return fio_of(h)->sampled_hash(ctx, h->h, path, out);
}
