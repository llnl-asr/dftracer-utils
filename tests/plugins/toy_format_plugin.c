// SPDX-License-Identifier: MIT
/* Test-only plugin exercising the file format service (dftu.svc.file_formats,
 * dftu_file_format_vt). It registers one format, "toy_format.rows":
 *
 *   TOY1
 *   <int64 id> <text name>
 *   ...
 *
 * Two chunks (the first half of the rows, then the rest), a projection that
 * keeps only the columns the caller asked for, and `id > literal` absorbed at
 * scan time and voted DFTU_PUSHED_EXACT.
 *
 * Two failure modes, so the test can drive an error out of each callback whose
 * message must reach the caller:
 *   - a first line of FAIL fails open();
 *   - a row named SCANFAIL fails scan() of the chunk that holds it.
 *
 * The factory also probes the registration name gate (host-reserved namespace
 * and a duplicate name) and exports toy_format_gate_ok() so the test can read
 * whether every probe behaved as documented.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOY_MAGIC "TOY1"
#define TOY_FAIL_MAGIC "FAIL"
#define TOY_VOTE_MAGIC "TOYV"
#define TOY_SCAN_FAIL_NAME "SCANFAIL"
#define TOY_ROWS_PER_FRAME 64
#define TOY_NAME_BYTES (TOY_ROWS_PER_FRAME * 32)

struct ToyFile {
    int64_t* ids;
    int32_t* offsets; /* n_rows + 1 into names */
    char* names;
    int64_t n_rows;
    int64_t chunks;
    int vary_votes; /* chunk 1 votes its filters differently from chunk 0 */
};

static int gate_ok = 0;
/* Counters the test reads through toy_format_stats: cursors made and destroyed,
 * files opened and closed, format states destroyed. */
static int64_t stat_made = 0;
static int64_t stat_killed = 0;
static int64_t stat_opened = 0;
static int64_t stat_closed = 0;
static int64_t stat_destroyed = 0;
static dftu_plugin plugin;

static void toy_close(void* file);

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

/* The next non-empty line of `buf` from `*pos`, NUL terminated in place. */
static char* next_line(char* buf, int64_t size, int64_t* pos) {
    while (*pos < size) {
        const int64_t start = *pos;
        while (*pos < size && buf[*pos] != '\n') ++*pos;
        int64_t end = *pos;
        if (*pos < size) ++*pos;
        if (end > start && buf[end - 1] == '\r') --end;
        buf[end] = '\0';
        if (end > start) return buf + start;
    }
    return NULL;
}

static dftu_status toy_open(dftu_ctx* ctx, void* unused_self, const char* path,
                            void** out) {
    (void)unused_self;
    int64_t size = 0;
    char* buf = path ? read_all(path, &size) : NULL;
    if (!buf)
        return dftu_fail(ctx, DFTU_E_IO, "toy_format: cannot read the file");

    int64_t pos = 0;
    const char* magic = next_line(buf, size, &pos);
    const size_t toy_len = strlen(TOY_MAGIC);
    const size_t vote_len = strlen(TOY_VOTE_MAGIC);
    const int is_toy = magic && strncmp(magic, TOY_MAGIC, toy_len) == 0;
    const int is_vote = magic && strncmp(magic, TOY_VOTE_MAGIC, vote_len) == 0;
    if (!is_toy && !is_vote) {
        const int fail = magic && strncmp(magic, TOY_FAIL_MAGIC,
                                          strlen(TOY_FAIL_MAGIC)) == 0;
        free(buf);
        if (fail)
            return dftu_fail(ctx, DFTU_E_IO,
                             "toy_format: the file's first line says FAIL");
        return dftu_fail(ctx, DFTU_E_INVALID, "toy_format: no TOY1 magic");
    }

    struct ToyFile* file = (struct ToyFile*)calloc(1, sizeof(*file));
    if (!file) {
        free(buf);
        return dftu_fail(ctx, DFTU_E_IO, "toy_format: out of memory");
    }
    const size_t room = (size_t)size + 1;
    file->ids = (int64_t*)malloc(sizeof(int64_t) * room);
    file->offsets = (int32_t*)malloc(sizeof(int32_t) * (room + 1));
    file->names = (char*)malloc(room);
    if (!file->ids || !file->offsets || !file->names) {
        toy_close(file);
        free(buf);
        return dftu_fail(ctx, DFTU_E_IO, "toy_format: out of memory");
    }

    int64_t names_len = 0;
    const char* line;
    while ((line = next_line(buf, size, &pos)) != NULL) {
        char* end = NULL;
        const long long id = strtoll(line, &end, 10);
        if (end == line || (*end != ' ' && *end != '\t')) {
            toy_close(file);
            free(buf);
            return dftu_fail(
                ctx, DFTU_E_INVALID,
                "toy_format: a row does not start with an integer");
        }
        while (*end == ' ' || *end == '\t') ++end;
        const size_t name_len = strlen(end);
        file->ids[file->n_rows] = (int64_t)id;
        file->offsets[file->n_rows] = (int32_t)names_len;
        memcpy(file->names + names_len, end, name_len);
        names_len += (int64_t)name_len;
        file->offsets[file->n_rows + 1] = (int32_t)names_len;
        ++file->n_rows;
    }
    free(buf);
    file->chunks = file->n_rows == 0 ? 1 : 2;
    file->vary_votes = is_vote;
    ++stat_opened;
    *out = file;
    return DFTU_OK;
}

static dftu_status toy_schema(dftu_ctx* ctx, void* unused_file,
                              dftu_schema* out) {
    (void)unused_file;
    if (dftu_schema_add_field(out, "id", DFTU_TYPE_INT64, 0, 0, NULL, 0, 0, 0) <
            0 ||
        dftu_schema_add_field(out, "name", DFTU_TYPE_STRING, 0, 0, NULL, 0, 0,
                              0) < 0)
        return dftu_fail(ctx, DFTU_E_INVALID,
                         "toy_format: cannot declare its schema");
    return DFTU_OK;
}

static dftu_status toy_chunks(dftu_ctx* unused_ctx, void* handle,
                              uint64_t* out) {
    (void)unused_ctx;
    *out = (uint64_t)((struct ToyFile*)handle)->chunks;
    return DFTU_OK;
}

static dftu_status toy_chunk_stats(dftu_ctx* unused_ctx, void* handle,
                                   uint64_t chunk, dftu_chunk_stats* out) {
    (void)unused_ctx;
    const struct ToyFile* file = (const struct ToyFile*)handle;
    const int64_t half = (file->n_rows + 1) / 2;
    if (chunk == 0)
        out->rows = half;
    else if (chunk == 1)
        out->rows = file->n_rows - half;
    else
        out->rows = -1;
    return DFTU_OK;
}

struct ToyCursor {
    const struct ToyFile* file; /* borrowed: the file outlives its cursors */
    int64_t pos;
    int64_t end;
    int has_filter;
    int64_t threshold;
    int n_columns;
    int columns[2]; /* 0 = id, 1 = name */
};

static dftu_status toy_next(dftu_ctx* ctx, void* self, int64_t max_rows,
                            dftu_dataframe** out, dftu_task** unused_task) {
    (void)unused_task;
    struct ToyCursor* cursor = (struct ToyCursor*)self;
    const struct ToyFile* file = cursor->file;

    int64_t ids[TOY_ROWS_PER_FRAME];
    int32_t offsets[TOY_ROWS_PER_FRAME + 1];
    char names[TOY_NAME_BYTES];
    const char* column_names[2];
    dftu_series* columns[2];

    int64_t limit = max_rows > 0 ? max_rows : cursor->end - cursor->pos;
    if (limit > TOY_ROWS_PER_FRAME) limit = TOY_ROWS_PER_FRAME;

    int64_t n = 0;
    int64_t names_len = 0;
    while (cursor->pos < cursor->end && n < limit) {
        const int64_t i = cursor->pos++;
        const int32_t from = file->offsets[i];
        const int32_t len = file->offsets[i + 1] - from;
        if (len == (int32_t)strlen(TOY_SCAN_FAIL_NAME) &&
            memcmp(file->names + from, TOY_SCAN_FAIL_NAME, (size_t)len) == 0)
            return dftu_fail(ctx, DFTU_E_IO,
                             "toy_format: a row's name says SCANFAIL");
        if (cursor->has_filter && file->ids[i] <= cursor->threshold) continue;
        ids[n] = file->ids[i];
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

    int n_columns = 0;
    for (int c = 0; c < cursor->n_columns; ++c) {
        if (cursor->columns[c] == 0) {
            column_names[n_columns] = "id";
            columns[n_columns++] =
                dftu_series_new_flat(DFTU_TYPE_INT64, ids, n, NULL);
        } else {
            column_names[n_columns] = "name";
            columns[n_columns++] = dftu_series_new_string(
                DFTU_TYPE_STRING, offsets, names, n, NULL);
        }
    }
    *out = dftu_dataframe_new(column_names, columns, n_columns);
    if (!*out) return dftu_fail(ctx, DFTU_E_INTERNAL, "toy_format: no frame");
    return DFTU_OK;
}

static void toy_cursor_destroy(void* self) {
    ++stat_killed;
    free(self);
}

static const dftu_cursor_vt TOY_CURSOR_VT = {
    .struct_size = sizeof(dftu_cursor_vt),
    .next = toy_next,
    .destroy = toy_cursor_destroy};

static dftu_status toy_scan(dftu_ctx* ctx, void* handle, uint64_t chunk,
                            const dftu_plugin_host* unused_host,
                            const dftu_scan_request* req, int32_t* out_pushed,
                            void** out_cursor_self,
                            const dftu_cursor_vt** out_vt) {
    (void)unused_host;
    const struct ToyFile* file = (const struct ToyFile*)handle;
    if ((int64_t)chunk >= file->chunks)
        return dftu_fail(ctx, DFTU_E_INVALID, "toy_format: no such chunk");

    struct ToyCursor* cursor = (struct ToyCursor*)calloc(1, sizeof(*cursor));
    if (!cursor) return dftu_fail(ctx, DFTU_E_IO, "toy_format: out of memory");
    cursor->file = file;
    const int64_t half = (file->n_rows + 1) / 2;
    cursor->pos = chunk == 0 ? 0 : half;
    cursor->end = chunk == 0 ? half : file->n_rows;
    cursor->n_columns = req->n_projection <= 0 ? 2 : req->n_projection;
    if (cursor->n_columns > 2) {
        free(cursor);
        return dftu_fail(ctx, DFTU_E_INVALID,
                         "toy_format: it has only two columns");
    }
    for (int c = 0; c < cursor->n_columns; ++c) {
        const char* want = req->n_projection <= 0 ? (c == 0 ? "id" : "name")
                                                  : req->projection[c];
        if (strcmp(want, "id") == 0) {
            cursor->columns[c] = 0;
        } else if (strcmp(want, "name") == 0) {
            cursor->columns[c] = 1;
        } else {
            free(cursor);
            return dftu_fail(ctx, DFTU_E_INVALID,
                             "toy_format: no column other than id and name");
        }
    }

    /* One filter kind: `id > <int literal>`, column 0 in the declared order. */
    if (req->n_filters > 1) {
        free(cursor);
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED,
                         "toy_format: at most one filter");
    }
    for (int32_t f = 0; f < req->n_filters; ++f) {
        int32_t column = -1;
        int32_t cmp = 0;
        dftu_scalar rhs;
        memset(&rhs, 0, sizeof(rhs));
        if (!dftu_expr_as_col_cmp(req->filters[f], &column, &cmp, &rhs) ||
            column != 0 || cmp != DFTU_CMP_GT ||
            rhs.kind != DFTU_SCALAR_TAG_I64) {
            free(cursor);
            return dftu_fail(ctx, DFTU_E_UNSUPPORTED,
                             "toy_format: only `id > <int>` is absorbed");
        }
        cursor->has_filter = 1;
        cursor->threshold = rhs.value.i;
        /* A TOYV file's chunk 1 disagrees with chunk 0 on purpose. */
        out_pushed[f] = (file->vary_votes && chunk == 1) ? DFTU_PUSHED_NO
                                                         : DFTU_PUSHED_EXACT;
    }

    ++stat_made;
    *out_cursor_self = cursor;
    *out_vt = &TOY_CURSOR_VT;
    return DFTU_OK;
}

static void toy_close(void* handle) {
    struct ToyFile* file = (struct ToyFile*)handle;
    if (!file) return;
    ++stat_closed;
    free(file->ids);
    free(file->offsets);
    free(file->names);
    free(file);
}

static void toy_destroy(void* self) {
    (void)self;
    ++stat_destroyed;
}

/* 90 on its own magic. FAIL is claimed too, so open() reports it rather than
 * the engine reporting that no format claims the file. TOYV is claimed so the
 * test can drive a chunk that votes its filters differently. */
static int32_t toy_probe(void* unused_self, const uint8_t* head, uint64_t n,
                         const char* unused_path) {
    (void)unused_self;
    (void)unused_path;
    static const char* const magics[] = {TOY_MAGIC, TOY_FAIL_MAGIC,
                                         TOY_VOTE_MAGIC};
    for (size_t i = 0; i < sizeof(magics) / sizeof(magics[0]); ++i) {
        const size_t len = strlen(magics[i]);
        if (n >= len && memcmp(head, magics[i], len) == 0) return 90;
    }
    return 0;
}

static const dftu_file_format_vt TOY_FORMAT_VT = {
    .struct_size = sizeof(dftu_file_format_vt),
    .extensions = NULL,
    .probe = toy_probe,
    .open = toy_open,
    .schema = toy_schema,
    .chunks = toy_chunks,
    .chunk_stats = toy_chunk_stats,
    .scan = toy_scan,
    .close = toy_close,
    .destroy = toy_destroy};

/* 10 on the same magic toy_probe scores 90 on, and its open() is a trap: the
 * engine must pick the higher score. */
static int32_t toy_low_probe(void* unused_self, const uint8_t* head, uint64_t n,
                             const char* unused_path) {
    return toy_probe(unused_self, head, n, unused_path) == 0 ? 0 : 10;
}

static dftu_status toy_low_open(dftu_ctx* ctx, void* unused_self,
                                const char* unused_path, void** unused_file) {
    (void)unused_self;
    (void)unused_path;
    (void)unused_file;
    return dftu_fail(ctx, DFTU_E_INTERNAL,
                     "toy_format.low: it must never be chosen");
}

static const dftu_file_format_vt TOY_LOW_VT = {
    .struct_size = sizeof(dftu_file_format_vt),
    .extensions = NULL,
    .probe = toy_low_probe,
    .open = toy_low_open,
    .schema = toy_schema,
    .chunks = toy_chunks,
    .chunk_stats = toy_chunk_stats,
    .scan = toy_scan,
    .close = toy_close,
    .destroy = toy_destroy};

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

DFTU_PLUGIN_EXPORT int toy_format_gate_ok(void);
DFTU_PLUGIN_EXPORT void toy_format_stats(int64_t* out);

DFTU_PLUGIN_EXPORT int toy_format_gate_ok(void) { return gate_ok; }

/* {made, killed, opened, closed, destroyed}: what the test counts to see that
 * every cursor, file and format state is released exactly once. */
DFTU_PLUGIN_EXPORT void toy_format_stats(int64_t* out) {
    out[0] = stat_made;
    out[1] = stat_killed;
    out[2] = stat_opened;
    out[3] = stat_closed;
    out[4] = stat_destroyed;
}

DFTU_PLUGIN_INFO("toy_format", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* unused_config,
                                               dftu_plugin** out) {
    (void)unused_config;
    const dftu_svc_file_formats* formats =
        (const dftu_svc_file_formats*)h->get_service(h->h,
                                                     DFTU_SVC_FILE_FORMATS);
    if (!formats || !formats->register_format || !formats->register_sink)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no file format registry");

    const dftu_status registered = formats->register_format(
        ctx, h->h, "toy_format.rows", &TOY_FORMAT_VT, NULL);
    /* A second format on the same file that scores lower: the engine must pick
     * the 90 above and never call its open(). */
    const dftu_status low = formats->register_format(
        ctx, h->h, "toy_format.low", &TOY_LOW_VT, NULL);
    /* Host-reserved namespace and a duplicate name must both be refused. */
    const int reserved = formats->register_format(ctx, h->h, "dftu.reserved",
                                                  &TOY_FORMAT_VT, NULL) < 0;
    const int duplicate = formats->register_format(ctx, h->h, "toy_format.rows",
                                                   &TOY_FORMAT_VT, NULL) < 0;
    gate_ok = registered >= 0 && low >= 0 && reserved && duplicate;

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
