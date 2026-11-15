// SPDX-License-Identifier: MIT
/* A real C consumer of the dftu_svc_file_formats ABI: builds a format and a
 * sink vtable, registers both through the service table exactly as a C plugin
 * would, and checks that the loader sees the same vtable and `self` back. The
 * registry here is a local table of this file's own, so nothing reaches the
 * host: the point is that the header, the function pointer types and the
 * struct layout compile and work from C.
 */
#include <dftracer/utils/plugins/abi.h>
#include <plugins/abi_c_consumers.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MSG_CAP 256

typedef struct {
    const dftu_test_harness* hn;
    const dftu_file_format_vt* got_vt;
    void* got_self;
    const dftu_sink_vt* got_sink_vt;
    void* got_sink_self;
    char name[64];
} reg;

static dftu_status reg_format(dftu_ctx* unused_ctx, void* self,
                              const char* name, const dftu_file_format_vt* vt,
                              void* format_self) {
    (void)unused_ctx;
    reg* r = (reg*)self;
    r->got_vt = vt;
    r->got_self = format_self;
    if (name) {
        strncpy(r->name, name, sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
    }
    return DFTU_OK;
}

static dftu_status reg_sink(dftu_ctx* unused_ctx, void* self,
                            const char* unused_name, const dftu_sink_vt* vt,
                            void* sink_self) {
    (void)unused_ctx;
    (void)unused_name;
    reg* r = (reg*)self;
    r->got_sink_vt = vt;
    r->got_sink_self = sink_self;
    return DFTU_OK;
}

static int32_t cf_probe(void* unused_self, const uint8_t* head, uint64_t n,
                        const char* unused_path) {
    (void)unused_self;
    (void)unused_path;
    return (n >= 2 && head[0] == 'C' && head[1] == 'F') ? 70 : 0;
}

static dftu_status cf_open(dftu_ctx* unused_ctx, void* self,
                           const char* unused_path, void** file) {
    (void)unused_ctx;
    (void)unused_path;
    *file = self;
    return DFTU_OK;
}

static dftu_status cf_schema(dftu_ctx* unused_ctx, void* unused_file,
                             dftu_schema* out) {
    (void)unused_ctx;
    (void)unused_file;
    return dftu_schema_add_field(out, "x", DFTU_TYPE_INT64, 0, 0, NULL, 0, 0,
                                 0) < 0
               ? DFTU_E_INVALID
               : DFTU_OK;
}

static dftu_status cf_chunks(dftu_ctx* unused_ctx, void* unused_file,
                             uint64_t* count) {
    (void)unused_ctx;
    (void)unused_file;
    *count = 1;
    return DFTU_OK;
}

static dftu_status cf_chunk_stats(dftu_ctx* unused_ctx, void* unused_file,
                                  uint64_t unused_chunk,
                                  dftu_chunk_stats* out) {
    (void)unused_ctx;
    (void)unused_file;
    (void)unused_chunk;
    out->rows = 0;
    return DFTU_OK;
}

static dftu_status cf_next(dftu_ctx* unused_ctx, void* unused_self,
                           int64_t unused_max_rows, dftu_dataframe** out,
                           dftu_task** unused_task) {
    (void)unused_ctx;
    (void)unused_self;
    (void)unused_max_rows;
    (void)unused_task;
    *out = NULL;
    return DFTU_END;
}

static void cf_cursor_destroy(void* unused_self) { (void)unused_self; }

static const dftu_cursor_vt CF_CURSOR_VT = {
    .struct_size = sizeof(dftu_cursor_vt),
    .next = cf_next,
    .destroy = cf_cursor_destroy};

static dftu_status cf_scan(dftu_ctx* unused_ctx, void* self,
                           uint64_t unused_chunk,
                           const dftu_plugin_host* unused_host,
                           const dftu_scan_request* req, int32_t* out_pushed,
                           void** cursor_self,
                           const dftu_cursor_vt** cursor_vt) {
    (void)unused_ctx;
    (void)unused_chunk;
    (void)unused_host;
    /* The request is the provider one, reused rather than copied. */
    if (out_pushed && req->n_filters > 0) out_pushed[0] = DFTU_PUSHED_EXACT;
    *cursor_self = self;
    *cursor_vt = &CF_CURSOR_VT;
    return DFTU_OK;
}

static void cf_close(void* unused_file) { (void)unused_file; }
static void cf_destroy(void* unused_self) { (void)unused_self; }

static dftu_status cf_bind(dftu_ctx* unused_ctx, void* unused_self,
                           const char* const* names, const int32_t* types,
                           uint32_t n_columns, const dftu_value* options,
                           void** sink) {
    (void)unused_ctx;
    (void)unused_self;
    if (n_columns > 0 && (!names || !types)) return DFTU_E_INVALID;
    (void)options;
    *sink = (void*)(uintptr_t)names;
    return DFTU_OK;
}

static dftu_status cf_write(dftu_ctx* unused_ctx, void* unused_sink,
                            const dftu_dataframe* batch) {
    (void)unused_ctx;
    (void)unused_sink;
    return batch ? DFTU_OK : DFTU_E_INVALID;
}

static dftu_status cf_finalize(dftu_ctx* unused_ctx, void* unused_sink) {
    (void)unused_ctx;
    (void)unused_sink;
    return DFTU_OK;
}
static void cf_abort(void* unused_sink) { (void)unused_sink; }
static void cf_sink_destroy(void* unused_self) { (void)unused_self; }

void dftu_test_file_format_ops(dftu_ctx* ctx, const dftu_test_harness* hn) {
    /* The service id is the string a plugin passes to get_service. */
    hn->expect("the file formats service id",
               strcmp(DFTU_SVC_FILE_FORMATS, "dftu.svc.file_formats") == 0);

    static const char* const extensions[] = {"cf", NULL};
    const dftu_file_format_vt format = {
        .struct_size = sizeof(dftu_file_format_vt),
        .extensions = extensions,
        .probe = cf_probe,
        .open = cf_open,
        .schema = cf_schema,
        .chunks = cf_chunks,
        .chunk_stats = cf_chunk_stats,
        .scan = cf_scan,
        .close = cf_close,
        .destroy = cf_destroy};
    const dftu_sink_vt sink = {.struct_size = sizeof(dftu_sink_vt),
                               .bind = cf_bind,
                               .write = cf_write,
                               .finalize = cf_finalize,
                               .abort = cf_abort,
                               .destroy = cf_sink_destroy};

    reg r;
    memset(&r, 0, sizeof(r));
    r.hn = hn;

    const dftu_svc_file_formats service = {sizeof(dftu_svc_file_formats),
                                           reg_format, reg_sink};
    int self = 0;
    hn->expect("a format registers",
               service.register_format(ctx, &r, "cf.format", &format, &self) ==
                   DFTU_OK);
    hn->expect("the vtable arrives unchanged", r.got_vt == &format);
    hn->expect("the format self arrives unchanged", r.got_self == &self);
    hn->expect("the name arrives unchanged", strcmp(r.name, "cf.format") == 0);

    int sink_self = 0;
    hn->expect("a sink registers",
               service.register_sink(ctx, &r, "cf.sink", &sink, &sink_self) ==
                   DFTU_OK);
    hn->expect("the sink vtable arrives unchanged", r.got_sink_vt == &sink);
    hn->expect("the sink self arrives unchanged",
               r.got_sink_self == &sink_self);

    /* The callbacks keep their documented shapes when called from C. */
    const uint8_t head[2] = {'C', 'F'};
    hn->expect("probe scores its magic",
               format.probe(&self, head, 2, "x.cf") == 70);
    hn->expect("probe refuses another file",
               format.probe(&self, (const uint8_t*)"zz", 2, "x") == 0);

    void* file = NULL;
    hn->expect(
        "open gives a file",
        format.open(ctx, &self, "x.cf", &file) == DFTU_OK && file == &self);
    uint64_t chunks = 0;
    hn->expect("chunks gives a count",
               format.chunks(ctx, file, &chunks) == DFTU_OK && chunks == 1);
    dftu_chunk_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    hn->expect(
        "chunk_stats writes the row count",
        format.chunk_stats(ctx, file, 0, &stats) == DFTU_OK && stats.rows == 0);
    format.close(file);

    hn->expect("the format's extensions are readable",
               extensions[0] != NULL && extensions[1] == NULL);

    void* sink_handle = NULL;
    const char* names[1] = {"x"};
    const int32_t types[1] = {DFTU_TYPE_INT64};
    hn->expect("bind accepts its columns",
               sink.bind(ctx, &sink_self, names, types, 1, NULL,
                         &sink_handle) == DFTU_OK);
    hn->expect("write takes a borrowed frame",
               sink.write(ctx, sink_handle, NULL) == DFTU_E_INVALID);
    hn->expect("finalize finishes the output",
               sink.finalize(ctx, sink_handle) == DFTU_OK);
    sink.destroy(&sink_self);
}
