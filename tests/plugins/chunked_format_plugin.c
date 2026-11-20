// SPDX-License-Identifier: MIT
/* Test-only plugin for the chunk_token callback of the file format service
 * (dftu_file_format_vt) and the index a format's file gets. It registers two
 * formats:
 *
 *   <plugin>.rows   a file of rows, CHUNK_ROWS rows to a chunk, with a token
 *                   per chunk: the byte offset of the chunk's first row;
 *   <plugin>.plain  the same rows, whose chunks have no token (chunk_token is
 *                   NULL), so its file is one chunk to the index.
 *
 * Its columns are id (int64), name (string) and args.v (int64, 10 * id).
 * A file is one row per line, either `<int64 id> <text name>` or the JSON line
 * {"id":<int64>,"name":"<text>"}, after an optional first line of CHK1 (rows)
 * or PLN1 (plain). A file with no magic line whose first line is a JSON row is
 * claimed by .rows, so one file can change owner between the built-in JSON
 * reader and this format. Frames carry FRAME_ROWS rows, so a chunk is several
 * frames. A filter is voted DFTU_PUSHED_NO: the engine applies it.
 *
 * Compiled twice: as `chunked_format`, and as `chunked_alt` (CHUNKED_FORMAT_ALT
 * defined), whose rows format also claims CHK1 files, so a test can change a
 * file's owner by loading the other build.
 *
 * The rows format has an explain hook (the filters and chunk rows of a scan)
 * and counts `chunks` (scans opened) and `rows` (rows framed); the plain format
 * has neither, so a test sees a format with no hook beside one with it.
 *
 * chunked_format_scan_log() reports the chunks scan() opened since the last
 * chunked_format_reset(), in order, so a test can see which chunks a scan read,
 * and chunked_format_open_count() the files open() was called on.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CHUNKED_FORMAT_ALT
#define PLUGIN_NAME "chunked_alt"
#else
#define PLUGIN_NAME "chunked_format"
#endif

#define CHUNK_ROWS 4
#define FRAME_ROWS 3
#define NAME_BYTES 64
#define SCAN_LOG_CAP 256

struct File {
    int64_t* ids;
    int32_t* offsets;   /* n_rows + 1 into names */
    char* names;
    int64_t* row_start; /* byte offset of each row's line */
    int64_t n_rows;
    int64_t chunks;
};

static dftu_plugin plugin;
static int64_t scan_log[SCAN_LOG_CAP];
static int64_t scan_count = 0;
static int64_t open_count = 0;

static void file_close(void* handle) {
    struct File* file = (struct File*)handle;
    if (!file) return;
    free(file->ids);
    free(file->offsets);
    free(file->names);
    free(file->row_start);
    free(file);
}

static char* read_all(const char* path, int64_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    *out_size = (int64_t)got;
    return buf;
}

/* Parses one row of `line` into `id` and `name` (NAME_BYTES room). */
static int parse_row(const char* line, long long* id, char* name) {
    if (line[0] == '{') {
        return sscanf(line, "{\"id\":%lld,\"name\":\"%63[^\"]\"}", id, name) ==
               2;
    }
    return sscanf(line, "%lld %63s", id, name) == 2;
}

static dftu_status file_open(dftu_ctx* ctx, void* unused_self, const char* path,
                             void** out) {
    (void)unused_self;
    int64_t size = 0;
    char* buf = path ? read_all(path, &size) : NULL;
    if (!buf)
        return dftu_fail(ctx, DFTU_E_IO,
                         "chunked_format: cannot read the file");
    ++open_count;

    struct File* file = (struct File*)calloc(1, sizeof(*file));
    const size_t room = (size_t)size + 1;
    if (file) {
        file->ids = (int64_t*)malloc(sizeof(int64_t) * room);
        file->offsets = (int32_t*)malloc(sizeof(int32_t) * (room + 1));
        file->names = (char*)malloc(room);
        file->row_start = (int64_t*)malloc(sizeof(int64_t) * room);
    }
    if (!file || !file->ids || !file->offsets || !file->names ||
        !file->row_start) {
        file_close(file);
        free(buf);
        return dftu_fail(ctx, DFTU_E_IO, "chunked_format: out of memory");
    }

    int64_t names_len = 0;
    int64_t pos = 0;
    while (pos < size) {
        const int64_t start = pos;
        while (pos < size && buf[pos] != '\n') ++pos;
        buf[pos] = '\0';
        char* line = buf + start;
        if (pos < size) ++pos;
        if (*line == '\0') continue;
        if (strncmp(line, "CHK1", 4) == 0 || strncmp(line, "PLN1", 4) == 0)
            continue;
        long long id = 0;
        char name[NAME_BYTES];
        if (!parse_row(line, &id, name)) {
            file_close(file);
            free(buf);
            return dftu_fail(ctx, DFTU_E_INVALID,
                             "chunked_format: a row is malformed");
        }
        const size_t len = strlen(name);
        file->ids[file->n_rows] = (int64_t)id;
        file->row_start[file->n_rows] = start;
        file->offsets[file->n_rows] = (int32_t)names_len;
        memcpy(file->names + names_len, name, len);
        names_len += (int64_t)len;
        file->offsets[file->n_rows + 1] = (int32_t)names_len;
        ++file->n_rows;
    }
    free(buf);
    file->chunks =
        file->n_rows == 0 ? 1 : (file->n_rows + CHUNK_ROWS - 1) / CHUNK_ROWS;
    *out = file;
    return DFTU_OK;
}

static dftu_status file_schema(dftu_ctx* ctx, void* unused_file,
                               dftu_schema* out) {
    (void)unused_file;
    if (dftu_schema_add_field(out, "id", DFTU_TYPE_INT64, 0, 0, NULL, 0, 0, 0) <
            0 ||
        dftu_schema_add_field(out, "name", DFTU_TYPE_STRING, 0, 0, NULL, 0, 0,
                              0) < 0 ||
        dftu_schema_add_field(out, "args.v", DFTU_TYPE_INT64, 0, 0, NULL, 0, 0,
                              0) < 0)
        return dftu_fail(ctx, DFTU_E_INVALID,
                         "chunked_format: cannot declare its schema");
    return DFTU_OK;
}

static dftu_status file_chunks(dftu_ctx* unused_ctx, void* handle,
                               uint64_t* out) {
    (void)unused_ctx;
    *out = (uint64_t)((struct File*)handle)->chunks;
    return DFTU_OK;
}

/* The byte offset of the chunk's first row: the same for the same bytes and
 * rising with the chunk. */
static dftu_status file_chunk_token(dftu_ctx* ctx, void* handle, uint64_t chunk,
                                    uint64_t* token) {
    const struct File* file = (const struct File*)handle;
    if ((int64_t)chunk >= file->chunks)
        return dftu_fail(ctx, DFTU_E_INVALID, "chunked_format: no such chunk");
    *token = file->n_rows == 0
                 ? 0
                 : (uint64_t)file->row_start[(int64_t)chunk * CHUNK_ROWS];
    return DFTU_OK;
}

struct Cursor {
    const struct File* file; /* borrowed: the file outlives its cursors */
    int64_t pos;
    int64_t end;
    int n_columns;
    int columns[3]; /* 0 = id, 1 = name, 2 = args.v */
};

static dftu_status cursor_next(dftu_ctx* ctx, void* self, int64_t max_rows,
                               dftu_dataframe** out, dftu_task** unused_task) {
    (void)unused_task;
    struct Cursor* cursor = (struct Cursor*)self;
    const struct File* file = cursor->file;

    int64_t ids[FRAME_ROWS];
    int64_t values[FRAME_ROWS];
    int32_t offsets[FRAME_ROWS + 1];
    char names[FRAME_ROWS * NAME_BYTES];
    const char* column_names[3];
    dftu_series* columns[3];

    int64_t limit = max_rows > 0 ? max_rows : cursor->end - cursor->pos;
    if (limit > FRAME_ROWS) limit = FRAME_ROWS;

    int64_t n = 0;
    int64_t names_len = 0;
    while (cursor->pos < cursor->end && n < limit) {
        const int64_t i = cursor->pos++;
        const int32_t from = file->offsets[i];
        const int32_t len = file->offsets[i + 1] - from;
        ids[n] = file->ids[i];
        values[n] = file->ids[i] * 10;
        offsets[n] = (int32_t)names_len;
        memcpy(names + names_len, file->names + from, (size_t)len);
        names_len += len;
        offsets[n + 1] = (int32_t)names_len;
        ++n;
    }
    if (n == 0) {
        *out = NULL;
        return DFTU_END;
    }
    dftu_counter(ctx, "rows", n);

    int n_columns = 0;
    for (int c = 0; c < cursor->n_columns; ++c) {
        if (cursor->columns[c] == 0) {
            column_names[n_columns] = "id";
            columns[n_columns++] =
                dftu_series_new_flat(DFTU_TYPE_INT64, ids, n, NULL);
        } else if (cursor->columns[c] == 1) {
            column_names[n_columns] = "name";
            columns[n_columns++] = dftu_series_new_string(
                DFTU_TYPE_STRING, offsets, names, n, NULL);
        } else {
            column_names[n_columns] = "args.v";
            columns[n_columns++] =
                dftu_series_new_flat(DFTU_TYPE_INT64, values, n, NULL);
        }
    }
    *out = dftu_dataframe_new(column_names, columns, n_columns);
    if (!*out)
        return dftu_fail(ctx, DFTU_E_INTERNAL, "chunked_format: no frame");
    return DFTU_OK;
}

static void cursor_destroy(void* self) { free(self); }

static const dftu_cursor_vt CURSOR_VT = {.struct_size = sizeof(dftu_cursor_vt),
                                         .next = cursor_next,
                                         .destroy = cursor_destroy};

static dftu_status file_scan(dftu_ctx* ctx, void* handle, uint64_t chunk,
                             const dftu_plugin_host* unused_host,
                             const dftu_scan_request* req,
                             int32_t* unused_pushed, void** out_cursor_self,
                             const dftu_cursor_vt** out_vt) {
    (void)unused_host;
    (void)unused_pushed;
    const struct File* file = (const struct File*)handle;
    if ((int64_t)chunk >= file->chunks)
        return dftu_fail(ctx, DFTU_E_INVALID, "chunked_format: no such chunk");

    struct Cursor* cursor = (struct Cursor*)calloc(1, sizeof(*cursor));
    if (!cursor)
        return dftu_fail(ctx, DFTU_E_IO, "chunked_format: out of memory");
    cursor->file = file;
    cursor->pos = (int64_t)chunk * CHUNK_ROWS;
    cursor->end = cursor->pos + CHUNK_ROWS;
    if (cursor->end > file->n_rows) cursor->end = file->n_rows;
    cursor->n_columns = req->n_projection <= 0 ? 3 : req->n_projection;
    if (cursor->n_columns > 3) {
        free(cursor);
        return dftu_fail(ctx, DFTU_E_INVALID,
                         "chunked_format: it has only three columns");
    }
    for (int c = 0; c < cursor->n_columns; ++c) {
        const char* want = req->n_projection <= 0 ? (c == 0   ? "id"
                                                     : c == 1 ? "name"
                                                              : "args.v")
                                                  : req->projection[c];
        if (strcmp(want, "id") == 0) {
            cursor->columns[c] = 0;
        } else if (strcmp(want, "name") == 0) {
            cursor->columns[c] = 1;
        } else if (strcmp(want, "args.v") == 0) {
            cursor->columns[c] = 2;
        } else {
            free(cursor);
            return dftu_fail(
                ctx, DFTU_E_INVALID,
                "chunked_format: no column other than id, name and args.v");
        }
    }
    if (scan_count < SCAN_LOG_CAP) scan_log[scan_count] = (int64_t)chunk;
    ++scan_count;
    dftu_counter(ctx, "chunks", 1);
    *out_cursor_self = cursor;
    *out_vt = &CURSOR_VT;
    return DFTU_OK;
}

static int starts_with(const uint8_t* head, uint64_t n, const char* text) {
    const size_t len = strlen(text);
    return n >= len && memcmp(head, text, len) == 0;
}

/* A first line that is a JSON row: {"id":. */
static int is_json_rows(const uint8_t* head, uint64_t n) {
    return starts_with(head, n, "{\"id\":");
}

static int32_t rows_probe(void* unused_self, const uint8_t* head, uint64_t n,
                          const char* unused_path) {
    (void)unused_self;
    (void)unused_path;
    if (starts_with(head, n, "CHK1")) return 90;
    return is_json_rows(head, n) ? 80 : 0;
}

static int32_t plain_probe(void* unused_self, const uint8_t* head, uint64_t n,
                           const char* unused_path) {
    (void)unused_self;
    (void)unused_path;
    return starts_with(head, n, "PLN1") ? 90 : 0;
}

static void format_destroy(void* unused_self) { (void)unused_self; }

static dftu_status file_explain(dftu_ctx* unused_ctx, void* unused_handle,
                                const dftu_scan_request* req,
                                const dftu_explain_out* out) {
    char text[32];
    (void)unused_ctx;
    (void)unused_handle;
    snprintf(text, sizeof(text), "%d", (int)req->n_filters);
    out->add(out->self, "filters_offered", 15, text, strlen(text));
    snprintf(text, sizeof(text), "%d", CHUNK_ROWS);
    out->add(out->self, "chunk_rows", 10, text, strlen(text));
    return DFTU_OK;
}

static const dftu_file_format_vt ROWS_VT = {
    .struct_size = sizeof(dftu_file_format_vt),
    .probe = rows_probe,
    .open = file_open,
    .schema = file_schema,
    .chunks = file_chunks,
    .scan = file_scan,
    .close = file_close,
    .destroy = format_destroy,
    .chunk_token = file_chunk_token,
    .explain = file_explain};

static const dftu_file_format_vt PLAIN_VT = {
    .struct_size = sizeof(dftu_file_format_vt),
    .probe = plain_probe,
    .open = file_open,
    .schema = file_schema,
    .chunks = file_chunks,
    .scan = file_scan,
    .close = file_close,
    .destroy = format_destroy,
    .chunk_token = NULL};

static dftu_status make_slice(dftu_ctx* unused_ctx, void* unused_self,
                              void** slice) {
    (void)unused_ctx;
    (void)unused_self;
    *slice = NULL;
    return DFTU_OK;
}
static dftu_status on_batch(dftu_ctx* unused_ctx, void* unused_self,
                            const dftu_dataframe* unused_batch,
                            const dftu_plugin_host* unused_host) {
    (void)unused_ctx;
    (void)unused_self;
    (void)unused_batch;
    (void)unused_host;
    return DFTU_OK;
}
static dftu_status merge(dftu_ctx* unused_ctx, void* unused_a, void* unused_b) {
    (void)unused_ctx;
    (void)unused_a;
    (void)unused_b;
    return DFTU_OK;
}
static void destroy_slice(void* unused_self) { (void)unused_self; }
static void destroy(void* unused_self) { (void)unused_self; }

DFTU_PLUGIN_EXPORT void chunked_format_reset(void);
DFTU_PLUGIN_EXPORT int64_t chunked_format_scan_log(int64_t* out, int64_t cap);
DFTU_PLUGIN_EXPORT int64_t chunked_format_open_count(void);

DFTU_PLUGIN_EXPORT void chunked_format_reset(void) {
    scan_count = 0;
    open_count = 0;
}

/* The files open() was called on since the reset. */
DFTU_PLUGIN_EXPORT int64_t chunked_format_open_count(void) {
    return open_count;
}

/* The chunks scan() opened since the reset, in order; returns how many. */
DFTU_PLUGIN_EXPORT int64_t chunked_format_scan_log(int64_t* out, int64_t cap) {
    int64_t n = scan_count < SCAN_LOG_CAP ? scan_count : SCAN_LOG_CAP;
    if (n > cap) n = cap;
    for (int64_t i = 0; i < n; ++i) out[i] = scan_log[i];
    return n;
}

#ifdef CHUNKED_FORMAT_V2
#define PLUGIN_VERSION "2.0"
#else
#define PLUGIN_VERSION "1.0"
#endif

DFTU_PLUGIN_INFO(PLUGIN_NAME, PLUGIN_VERSION)

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* unused_config,
                                               dftu_plugin** out) {
    (void)unused_config;
    const dftu_svc_file_formats* formats =
        (const dftu_svc_file_formats*)h->get_service(h->h,
                                                     DFTU_SVC_FILE_FORMATS);
    if (!formats || !formats->register_format)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no file format registry");

    dftu_status registered = formats->register_format(
        ctx, h->h, PLUGIN_NAME ".rows", &ROWS_VT, NULL);
    if (registered >= 0)
        registered = formats->register_format(ctx, h->h, PLUGIN_NAME ".plain",
                                              &PLAIN_VT, NULL);

    memset(&plugin, 0, sizeof(plugin));
    plugin.struct_size = sizeof(plugin);
    plugin.make_slice = make_slice;
    plugin.on_batch = on_batch;
    plugin.merge = merge;
    plugin.destroy_slice = destroy_slice;
    plugin.destroy = destroy;
    *out = &plugin;
    return registered;
}
