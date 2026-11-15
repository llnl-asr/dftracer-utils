// SPDX-License-Identifier: MIT
/* dftu.svc.store from real C: write and commit, read back through a snapshot,
 * refuse a namespace name, and refuse a batch whose sources report a changed
 * file. */
#include <dftracer/utils/plugins/abi.h>
#include <plugins/abi_c_consumers.h>
#include <stdint.h>
#include <string.h>

static const dftu_svc_store* store_of(dftu_ctx* ctx,
                                      const dftu_plugin_host* host,
                                      dftu_store_report* report) {
    const dftu_svc_store* e =
        (const dftu_svc_store*)host->get_service(host->h, DFTU_SVC_STORE);
    report->has_service = e != NULL;
    if (e == NULL) dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no store service");
    return e;
}

static void copy_value(const dftu_store_bytes bytes, char* out, uint64_t cap) {
    uint64_t n = bytes.size < cap - 1 ? bytes.size : cap - 1;
    if (bytes.data != NULL && n > 0) memcpy(out, bytes.data, (size_t)n);
    out[n] = '\0';
}

static void take_message(dftu_ctx* ctx, const dftu_test_harness* hn,
                         dftu_store_report* report) {
    hn->ctx_message(ctx, report->message, sizeof(report->message));
}

/* One copy of scope `path` in namespace `ns` at key `key`: put `value` and
 * commit. */
void dftu_test_store_write_at(dftu_ctx* ctx, const dftu_plugin_host* host,
                              const dftu_test_harness* hn, const char* ns,
                              const char* path, const char* key,
                              const char* value,
                              const dftu_store_source_stat* sources,
                              dftu_store_report* report) {
    memset(report, 0, sizeof(*report));
    const dftu_svc_store* e = store_of(ctx, host, report);
    if (e == NULL) return;

    dftu_store_ns_t* n = NULL;
    dftu_store_batch_t* b = NULL;
    const char* paths[1] = {path};
    dftu_store_scope scope = {DFTU_STORE_SCOPE_FILE, paths, 1};

    report->status = e->ns(ctx, host->h, ns, 1, DFTU_STORE_FOLD_NONE, 0, 0, &n);
    if (report->status < 0) {
        take_message(ctx, hn, report);
        return;
    }
    report->namespace_registered = 1;
    if (e->batch(ctx, host->h, n, &scope, 7, sources, sources ? 1 : 0, &b) <
        0) {
        take_message(ctx, hn, report);
        e->ns_free(n);
        return;
    }
    if (e->put(ctx, b, (const uint8_t*)key, strlen(key), (const uint8_t*)value,
               strlen(value)) < 0) {
        take_message(ctx, hn, report);
        e->batch_free(b);
        e->ns_free(n);
        return;
    }
    if (e->commit(ctx, host->h, b) < 0) {
        take_message(ctx, hn, report);
        e->ns_free(n);
        return;
    }
    /* No read-back here: a snapshot can be up to a second old, so the test
     * reads the value back through a later fold. */
    report->committed = 1;
    e->ns_free(n);
}

/* One copy of scope `path` in namespace `ns` at key "k": put `value` and
 * commit. */
void dftu_test_store_write(dftu_ctx* ctx, const dftu_plugin_host* host,
                           const dftu_test_harness* hn, const char* ns,
                           const char* path, const char* value,
                           const dftu_store_source_stat* sources,
                           dftu_store_report* report) {
    dftu_test_store_write_at(ctx, host, hn, ns, path, "k", value, sources,
                             report);
}

/* Register `ns` and read key "k" of scope `path` into report->value. */
void dftu_test_store_read(dftu_ctx* ctx, const dftu_plugin_host* host,
                          const dftu_test_harness* hn, const char* ns,
                          const char* path, dftu_store_report* report) {
    memset(report, 0, sizeof(*report));
    const dftu_svc_store* e = store_of(ctx, host, report);
    if (e == NULL) return;

    dftu_store_ns_t* n = NULL;
    dftu_store_snapshot_t* s = NULL;
    dftu_store_value_t* v = NULL;
    const char* paths[1] = {path};
    dftu_store_scope scope = {DFTU_STORE_SCOPE_FILE, paths, 1};

    report->status = e->ns(ctx, host->h, ns, 1, DFTU_STORE_FOLD_NONE, 0, 0, &n);
    if (report->status < 0) {
        take_message(ctx, hn, report);
        return;
    }
    report->namespace_registered = 1;
    if (e->snapshot(ctx, host->h, &s) < 0) {
        take_message(ctx, hn, report);
        e->ns_free(n);
        return;
    }
    if (e->get(ctx, s, n, &scope, (const uint8_t*)"k", 1, &v) < 0) {
        take_message(ctx, hn, report);
    } else if (v != NULL) {
        report->found = 1;
        copy_value(e->value_bytes(v), report->value, sizeof(report->value));
        e->value_free(v);
    }
    e->snapshot_free(s);
    e->ns_free(n);
}

/* Register `ns` only, to see which names the host refuses and why. */
void dftu_test_store_probe(dftu_ctx* ctx, const dftu_plugin_host* host,
                           const dftu_test_harness* hn, const char* ns,
                           dftu_store_report* report) {
    memset(report, 0, sizeof(*report));
    const dftu_svc_store* e = store_of(ctx, host, report);
    if (e == NULL) return;
    dftu_store_ns_t* n = NULL;
    report->status = e->ns(ctx, host->h, ns, 1, DFTU_STORE_FOLD_NONE, 0, 0, &n);
    if (report->status < 0) {
        take_message(ctx, hn, report);
        return;
    }
    report->namespace_registered = 1;
    e->ns_free(n);
}

/* Take a namespace handle from host `a` and use it with host `b`: the host
 * must refuse it, since a namespace handle belongs to the store that made it.
 */
void dftu_test_store_cross(dftu_ctx* ctx, const dftu_plugin_host* a,
                           const dftu_plugin_host* b,
                           const dftu_test_harness* hn, const char* ns,
                           const char* path, dftu_store_report* report) {
    memset(report, 0, sizeof(*report));
    const dftu_svc_store* ea = store_of(ctx, a, report);
    if (ea == NULL) return;
    const dftu_svc_store* eb =
        (const dftu_svc_store*)b->get_service(b->h, DFTU_SVC_STORE);
    if (eb == NULL) {
        dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no store service");
        return;
    }
    dftu_store_ns_t* n = NULL;
    const char* paths[1] = {path};
    dftu_store_scope scope = {DFTU_STORE_SCOPE_FILE, paths, 1};
    if (ea->ns(ctx, a->h, ns, 1, DFTU_STORE_FOLD_NONE, 0, 0, &n) < 0) {
        take_message(ctx, hn, report);
        return;
    }
    report->namespace_registered = 1;
    dftu_store_batch_t* batch = NULL;
    report->status = eb->batch(ctx, b->h, n, &scope, 0, NULL, 0, &batch);
    if (report->status < 0) {
        take_message(ctx, hn, report);
    } else {
        eb->batch_free(batch);
    }
    dftu_store_snapshot_t* s = NULL;
    if (eb->snapshot(ctx, b->h, &s) >= 0) {
        dftu_store_value_t* v = NULL;
        const dftu_status read =
            eb->get(ctx, s, n, &scope, (const uint8_t*)"k", 1, &v);
        if (read < 0) {
            report->refused_read = 1;
            take_message(ctx, hn, report);
        }
        if (v != NULL) eb->value_free(v);
        eb->snapshot_free(s);
    }
    ea->ns_free(n);
}

static void null_checked(dftu_ctx* ctx, const dftu_test_harness* hn,
                         dftu_store_report* report, dftu_status status) {
    if (status == DFTU_E_INVALID) {
        report->null_refused++;
    } else {
        report->status = status;
    }
    take_message(ctx, hn, report);
}

/* Every call that takes a namespace, batch, snapshot or cursor handle, with a
 * NULL one: each must refuse it instead of reaching the store C ABI. */
void dftu_test_store_nulls(dftu_ctx* ctx, const dftu_plugin_host* host,
                           const dftu_test_harness* hn, const char* ns,
                           const char* path, dftu_store_report* report) {
    memset(report, 0, sizeof(*report));
    const dftu_svc_store* e = store_of(ctx, host, report);
    if (e == NULL) return;
    const uint8_t key[] = "k";
    const uint8_t value[] = "v";
    const char* paths[1] = {path};
    dftu_store_scope scope = {DFTU_STORE_SCOPE_FILE, paths, 1};
    dftu_store_ns_t* n = NULL;
    dftu_store_snapshot_t* snap = NULL;
    dftu_store_value_t* v = NULL;
    dftu_store_value_list_t* list = NULL;
    dftu_store_cursor_t* cursor = NULL;
    dftu_store_bytes bytes = {NULL, 0};

    null_checked(ctx, hn, report,
                 e->batch(ctx, host->h, NULL, &scope, 0, NULL, 0, NULL));
    null_checked(ctx, hn, report, e->put(ctx, NULL, key, 1, value, 1));
    null_checked(ctx, hn, report, e->fold(ctx, NULL, key, 1, value, 1));
    null_checked(ctx, hn, report, e->erase(ctx, NULL, key, 1));
    null_checked(ctx, hn, report, e->erase_range(ctx, NULL, key, 1, key, 1));
    null_checked(ctx, hn, report, e->commit(ctx, host->h, NULL));
    null_checked(ctx, hn, report, e->get(ctx, NULL, NULL, &scope, key, 1, &v));
    null_checked(
        ctx, hn, report,
        e->scan_range(ctx, NULL, NULL, &scope, key, 1, key, 1, &cursor));
    null_checked(ctx, hn, report, e->cursor_next(ctx, NULL));
    null_checked(ctx, hn, report, e->cursor_value(ctx, NULL, &bytes));

    /* The same with a real snapshot and a NULL namespace. */
    if (e->ns(ctx, host->h, ns, 1, DFTU_STORE_FOLD_NONE, 0, 0, &n) >= 0 &&
        e->snapshot(ctx, host->h, &snap) >= 0) {
        null_checked(ctx, hn, report,
                     e->get(ctx, snap, NULL, &scope, key, 1, &v));
        null_checked(ctx, hn, report,
                     e->multi_get(ctx, snap, NULL, &scope, NULL, 0, &list));
        null_checked(ctx, hn, report,
                     e->scan_prefix(ctx, snap, NULL, &scope, NULL, 0, &cursor));
        e->snapshot_free(snap);
    }
    if (n != NULL) e->ns_free(n);
}

/* Build a batch over `ns` and hand it back, so the test can refuse its commit
 * after changing what the fold knows. */
dftu_status dftu_test_store_make_batch(dftu_ctx* ctx,
                                       const dftu_plugin_host* host,
                                       const dftu_test_harness* hn,
                                       const char* ns, const char* path,
                                       dftu_store_batch_t** out,
                                       dftu_store_report* report) {
    memset(report, 0, sizeof(*report));
    *out = NULL;
    const dftu_svc_store* e = store_of(ctx, host, report);
    if (e == NULL) return DFTU_E_UNSUPPORTED;
    const char* paths[1] = {path};
    dftu_store_scope scope = {DFTU_STORE_SCOPE_FILE, paths, 1};
    dftu_store_ns_t* n = NULL;
    report->status = e->ns(ctx, host->h, ns, 1, DFTU_STORE_FOLD_NONE, 0, 0, &n);
    if (report->status < 0) {
        take_message(ctx, hn, report);
        return report->status;
    }
    report->namespace_registered = 1;
    report->status = e->batch(ctx, host->h, n, &scope, 0, NULL, 0, out);
    e->ns_free(n);
    if (report->status < 0) take_message(ctx, hn, report);
    return report->status;
}

/* Commit a batch the host made; the host consumes it whether it commits or
 * refuses, so the test leaks nothing in either case. */
void dftu_test_store_commit(dftu_ctx* ctx, const dftu_plugin_host* host,
                            const dftu_test_harness* hn,
                            dftu_store_batch_t* batch,
                            dftu_store_report* report) {
    memset(report, 0, sizeof(*report));
    const dftu_svc_store* e = store_of(ctx, host, report);
    if (e == NULL) return;
    report->status = e->commit(ctx, host->h, batch);
    if (report->status < 0) {
        take_message(ctx, hn, report);
    } else {
        report->committed = 1;
    }
}

/* Hand back the namespace handle of `ns` itself, for a handle whose fold dies
 * before the test uses it. */
void dftu_test_store_probe_ns(dftu_ctx* ctx, const dftu_plugin_host* host,
                              const dftu_test_harness* hn, const char* ns,
                              dftu_store_ns_t** out,
                              dftu_store_report* report) {
    memset(report, 0, sizeof(*report));
    *out = NULL;
    const dftu_svc_store* e = store_of(ctx, host, report);
    if (e == NULL) return;
    report->status =
        e->ns(ctx, host->h, ns, 1, DFTU_STORE_FOLD_NONE, 0, 0, out);
    if (report->status < 0) {
        take_message(ctx, hn, report);
        return;
    }
    report->namespace_registered = 1;
}

/* Use a namespace handle the test holds with `host`: a batch over `path`. */
void dftu_test_store_use(dftu_ctx* ctx, const dftu_plugin_host* host,
                         const dftu_test_harness* hn, dftu_store_ns_t* n,
                         const char* path, dftu_store_report* report) {
    memset(report, 0, sizeof(*report));
    const dftu_svc_store* e = store_of(ctx, host, report);
    if (e == NULL) return;
    const char* paths[1] = {path};
    dftu_store_scope scope = {DFTU_STORE_SCOPE_FILE, paths, 1};
    dftu_store_batch_t* b = NULL;
    report->status = e->batch(ctx, host->h, n, &scope, 0, NULL, 0, &b);
    if (report->status < 0) {
        take_message(ctx, hn, report);
        return;
    }
    report->namespace_registered = 1;
    e->batch_free(b);
}
