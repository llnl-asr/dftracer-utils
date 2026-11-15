// SPDX-License-Identifier: MIT
/* A real C consumer of the dftu_svc_fs ABI. C has no coroutines, so the C++
 * harness runs each task; the scenario starts an op, has the harness run it and
 * checks the status, the message and the result. */
#include <dftracer/utils/plugins/abi.h>
#include <plugins/abi_c_consumers.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MSG_CAP 256

typedef struct {
    dftu_ctx* ctx;
    const dftu_plugin_host* host;
    const dftu_svc_fs* fs;
    const dftu_test_harness* hn;
    char msg[MSG_CAP];
} scn;

/* Takes the task by address: C leaves the order of argument evaluation open,
 * so a plain `t` could be read before the op call sets it. */
static dftu_status finish(scn* s, dftu_status st, dftu_task** t) {
    s->msg[0] = 0;
    if (st < 0) return st;
    return s->hn->run(*t, s->msg, MSG_CAP);
}

/* Checks the status and, for a failure, that the message names the op. */
static int expect_status(scn* s, const char* what, dftu_status got,
                         dftu_status want, const char* in_message) {
    s->hn->expect(what, got == want);
    if (in_message && got == want && got != DFTU_OK)
        s->hn->expect(what, strstr(s->msg, in_message) != NULL);
    return got == want;
}

/* Checks that the op failed, with any error status. */
static void expect_failed(scn* s, const char* what, dftu_status got,
                          const char* in_message) {
    s->hn->expect(what, got < 0);
    if (in_message && got < 0)
        s->hn->expect(what, strstr(s->msg, in_message) != NULL);
}

static int has_name(const dftu_svc_fs* fs, const dftu_path_list* list,
                    const char* suffix, dftu_file_kind kind) {
    const size_t suffix_len = strlen(suffix);
    for (uint64_t i = 0; i < fs->list_count(list); ++i) {
        dftu_path_entry e;
        if (fs->list_entry(NULL, list, i, &e) < 0) return 0;
        if (e.path_len >= suffix_len &&
            strcmp(e.path + e.path_len - suffix_len, suffix) == 0 &&
            (kind < 0 || e.kind == kind))
            return 1;
    }
    return 0;
}

void dftu_test_fs_ops(dftu_ctx* ctx, const dftu_plugin_host* h,
                      const dftu_test_harness* hn, const char* dir) {
    scn sc = {
        ctx, h, (const dftu_svc_fs*)h->get_service(h->h, DFTU_SVC_FS), hn, {0}};
    scn* s = &sc;
    const dftu_svc_fs* fs = s->fs;
    dftu_task* t = NULL;
    char root[512], sub[512], a[512], b[512], missing[512], through[512],
        in_missing_dir[512];
    snprintf(root, sizeof root, "%s/fs_root", dir);
    snprintf(sub, sizeof sub, "%s/fs_root/sub", dir);
    snprintf(a, sizeof a, "%s/fs_root/a.txt", dir);
    snprintf(b, sizeof b, "%s/fs_root/sub/b.txt", dir);
    snprintf(missing, sizeof missing, "%s/fs_root/missing", dir);
    snprintf(through, sizeof through, "%s/fs_root/a.txt/x", dir);
    snprintf(in_missing_dir, sizeof in_missing_dir, "%s/fs_root/nodir/x", dir);
    if (!fs) {
        hn->expect("the fs service", 0);
        return;
    }

    int32_t flag = -1;
    expect_status(
        s, "create_directories",
        finish(s, fs->create_directories(ctx, h->h, sub, &flag, &t), &t),
        DFTU_OK, NULL);
    hn->expect("create_directories made them", flag == 1);
    expect_status(
        s, "create_directories again",
        finish(s, fs->create_directories(ctx, h->h, sub, &flag, &t), &t),
        DFTU_OK, NULL);
    hn->expect("create_directories found them", flag == 0);

    const char text[] = "hello world";
    expect_status(s, "write_file",
                  finish(s, fs->write_file(ctx, h->h, a, text, 11, &t), &t),
                  DFTU_OK, NULL);
    expect_status(
        s, "write_file into a missing directory",
        finish(s, fs->write_file(ctx, h->h, in_missing_dir, text, 11, &t), &t),
        DFTU_E_NOT_FOUND, "write_file");
    expect_status(s, "write_file with a NULL path",
                  fs->write_file(ctx, h->h, NULL, text, 11, &t), DFTU_E_INVALID,
                  NULL);
    expect_status(s, "write_file b",
                  finish(s, fs->write_file(ctx, h->h, b, "bb", 2, &t), &t),
                  DFTU_OK, NULL);

    dftu_bytes bytes;
    memset(&bytes, 0, sizeof bytes);
    expect_status(s, "read_file",
                  finish(s, fs->read_file(ctx, h->h, a, &bytes, &t), &t),
                  DFTU_OK, NULL);
    hn->expect("read_file bytes", bytes.len == 11 &&
                                      memcmp(bytes.data, text, 11) == 0 &&
                                      bytes.free_fn != NULL);
    if (bytes.free_fn) bytes.free_fn((void*)(uintptr_t)bytes.data, bytes.ud);
    expect_status(s, "read_file of a missing file",
                  finish(s, fs->read_file(ctx, h->h, missing, &bytes, &t), &t),
                  DFTU_E_NOT_FOUND, "read_file");
    hn->expect("a failed read_file leaves no bytes to free",
               bytes.free_fn == NULL);

    char buf[16] = {0};
    int64_t n = -1;
    expect_status(s, "read_at",
                  finish(s, fs->read_at(ctx, h->h, a, 6, buf, 5, &n, &t), &t),
                  DFTU_OK, NULL);
    hn->expect("read_at bytes", n == 5 && memcmp(buf, "world", 5) == 0);
    expect_status(s, "read_at past the end",
                  finish(s, fs->read_at(ctx, h->h, a, 100, buf, 5, &n, &t), &t),
                  DFTU_OK, NULL);
    hn->expect("read_at past the end gives 0", n == 0);
    expect_status(
        s, "read_at of a missing file",
        finish(s, fs->read_at(ctx, h->h, missing, 0, buf, 5, &n, &t), &t),
        DFTU_E_NOT_FOUND, "read_at");

    expect_status(
        s, "write_at",
        finish(s, fs->write_at(ctx, h->h, a, 6, "WORLD", 5, &n, &t), &t),
        DFTU_OK, NULL);
    hn->expect("write_at count", n == 5);
    memset(buf, 0, sizeof buf);
    expect_status(s, "read back",
                  finish(s, fs->read_at(ctx, h->h, a, 0, buf, 11, &n, &t), &t),
                  DFTU_OK, NULL);
    hn->expect("write_at changed the file",
               n == 11 && memcmp(buf, "hello WORLD", 11) == 0);
    expect_status(
        s, "write_at in a missing directory",
        finish(s, fs->write_at(ctx, h->h, in_missing_dir, 0, "x", 1, &n, &t),
               &t),
        DFTU_E_NOT_FOUND, "write_at");

    expect_status(s, "exists",
                  finish(s, fs->exists(ctx, h->h, a, &flag, &t), &t), DFTU_OK,
                  NULL);
    hn->expect("exists of a file", flag == 1);
    expect_status(s, "exists of a missing path",
                  finish(s, fs->exists(ctx, h->h, missing, &flag, &t), &t),
                  DFTU_OK, NULL);
    hn->expect("exists of a missing path", flag == 0);
    expect_status(s, "exists with a NULL path",
                  fs->exists(ctx, h->h, NULL, &flag, &t), DFTU_E_INVALID, NULL);
    expect_failed(
        s, "create_directories below a file",
        finish(s, fs->create_directories(ctx, h->h, through, &flag, &t), &t),
        "create_directories");

    dftu_path_list* list = NULL;
    expect_status(s, "list_dir",
                  finish(s, fs->list_dir(ctx, h->h, root, &list, &t), &t),
                  DFTU_OK, NULL);
    hn->expect("list_dir lists a file and a directory",
               list != NULL && fs->list_count(list) == 2 &&
                   has_name(fs, list, "a.txt", DFTU_FILE_FILE) &&
                   has_name(fs, list, "sub", DFTU_FILE_DIRECTORY));
    dftu_path_entry entry;
    expect_status(s, "list_entry past the end",
                  list ? fs->list_entry(ctx, list, 2, &entry) : DFTU_E_INVALID,
                  DFTU_E_INVALID, NULL);
    fs->list_free(list);
    list = NULL;
    expect_status(s, "list_dir of a missing directory",
                  finish(s, fs->list_dir(ctx, h->h, missing, &list, &t), &t),
                  DFTU_E_NOT_FOUND, "list_dir");
    hn->expect("a failed list_dir leaves no list", list == NULL);

    const char* const patterns[] = {"*.txt"};
    dftu_find_options opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = sizeof opts;
    opts.patterns = patterns;
    opts.pattern_count = 1;
    opts.recursive = 1;
    opts.with_stat = 1;
    expect_status(
        s, "find_all",
        finish(s, fs->find_all(ctx, h->h, root, &opts, &list, &t), &t), DFTU_OK,
        NULL);
    hn->expect("find_all finds both files with their sizes",
               list != NULL && fs->list_count(list) == 2 &&
                   has_name(fs, list, "a.txt", DFTU_FILE_FILE) &&
                   has_name(fs, list, "sub/b.txt", DFTU_FILE_FILE));
    if (list && fs->list_count(list) == 2) {
        dftu_path_entry first;
        fs->list_entry(ctx, list, 0, &first);
        hn->expect("find_all sorts by path and stats",
                   first.path_len > 5 && first.size == 11);
    }
    fs->list_free(list);
    list = NULL;
    opts.recursive = 0;
    expect_status(
        s, "find_all not recursive",
        finish(s, fs->find_all(ctx, h->h, root, &opts, &list, &t), &t), DFTU_OK,
        NULL);
    hn->expect("find_all not recursive finds one file",
               list != NULL && fs->list_count(list) == 1);
    fs->list_free(list);
    list = NULL;
    expect_status(
        s, "find_all of a missing root",
        finish(s, fs->find_all(ctx, h->h, missing, &opts, &list, &t), &t),
        DFTU_E_NOT_FOUND, "find_all");

    const char* const paths[3] = {a, missing, sub};
    dftu_fs_stat stats[3];
    dftu_status statuses[3] = {DFTU_OK, DFTU_OK, DFTU_OK};
    memset(stats, 0, sizeof stats);
    expect_status(
        s, "stat_batch",
        finish(s, fs->stat_batch(ctx, h->h, paths, 3, stats, statuses, &t), &t),
        DFTU_OK, NULL);
    hn->expect("stat_batch of a file", statuses[0] == DFTU_OK &&
                                           stats[0].size == 11 &&
                                           stats[0].kind == DFTU_FILE_FILE);
    hn->expect("stat_batch of a missing path fails only its slot",
               statuses[1] == DFTU_E_NOT_FOUND);
    hn->expect("stat_batch of a directory",
               statuses[2] == DFTU_OK && stats[2].kind == DFTU_FILE_DIRECTORY);
    expect_status(s, "stat_batch with a NULL path array",
                  fs->stat_batch(ctx, h->h, NULL, 3, stats, statuses, &t),
                  DFTU_E_INVALID, NULL);
    expect_status(
        s, "stat_batch with a count the heap cannot hold",
        fs->stat_batch(ctx, h->h, paths, UINT64_MAX / 2, stats, statuses, &t),
        DFTU_E_NO_MEMORY, NULL);
    hn->ctx_message(ctx, s->msg, MSG_CAP);
    hn->expect("the refused stat_batch names the allocation",
               strstr(s->msg, "cannot allocate") != NULL);

    expect_failed(s, "remove of a directory that has entries",
                  finish(s, fs->remove(ctx, h->h, sub, &flag, &t), &t),
                  "remove");
    expect_status(s, "remove",
                  finish(s, fs->remove(ctx, h->h, b, &flag, &t), &t), DFTU_OK,
                  NULL);
    hn->expect("remove removed it", flag == 1);
    expect_status(s, "remove of a missing path",
                  finish(s, fs->remove(ctx, h->h, b, &flag, &t), &t), DFTU_OK,
                  NULL);
    hn->expect("remove of a missing path gives 0", flag == 0);
    expect_status(s, "remove with a NULL path",
                  fs->remove(ctx, h->h, NULL, &flag, &t), DFTU_E_INVALID, NULL);
    fs->remove(ctx, h->h, sub, &flag, &t);
    hn->run(t, sc.msg, MSG_CAP);
    fs->remove(ctx, h->h, a, &flag, &t);
    hn->run(t, sc.msg, MSG_CAP);
    fs->remove(ctx, h->h, root, &flag, &t);
    hn->run(t, sc.msg, MSG_CAP);
}
