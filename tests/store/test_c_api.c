// SPDX-License-Identifier: MIT
#include <dftracer/utils/store/c_api.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <testing_utilities.h>
#include <unity.h>

static test_environment_handle_t g_env = NULL;
static int g_dir_count = 0;

void setUp(void) {
    g_env = test_environment_create();
    TEST_ASSERT_NOT_NULL(g_env);
}

void tearDown(void) {
    test_environment_destroy(g_env);
    g_env = NULL;
}

static const char* const FILE_PATH[] = {"/traces/a.pfw"};
static const dftu_store_scope FILE_SCOPE = {DFTU_STORE_SCOPE_FILE, FILE_PATH,
                                            1};
static const dftu_store_scope ROOT_SCOPE = {DFTU_STORE_SCOPE_ROOT, NULL, 0};

static void check_ok(dftu_status status) {
    TEST_ASSERT_EQUAL_INT(DFTU_OK, status);
}

static dftu_store_t* open_store(void) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/store_%d", test_environment_get_dir(g_env),
             ++g_dir_count);
    dftu_store_t* store = NULL;
    check_ok(dftu_store_open(dir, &store, NULL));
    TEST_ASSERT_NOT_NULL(store);
    return store;
}

static dftu_store_ns_t* make_ns(dftu_store_t* store, const char* name,
                                int fold) {
    dftu_store_ns_t* ns = NULL;
    check_ok(dftu_store_ns(store, name, 1, fold, 0, 0, &ns, NULL));
    return ns;
}

static void put_str(dftu_store_batch_t* b, const char* key, const char* value) {
    check_ok(dftu_store_batch_put(b, (const uint8_t*)key, strlen(key),
                                  (const uint8_t*)value, strlen(value), NULL));
}

static dftu_store_batch_t* new_batch(dftu_store_t* store,
                                     const dftu_store_ns_t* ns,
                                     const dftu_store_scope* scope) {
    dftu_store_batch_t* b = NULL;
    check_ok(dftu_store_batch(store, ns, scope, 0, NULL, 0, &b, NULL));
    return b;
}

static void commit_pairs(dftu_store_t* store, const dftu_store_ns_t* ns,
                         const dftu_store_scope* scope,
                         const char* const* pairs, int count) {
    dftu_store_batch_t* b = new_batch(store, ns, scope);
    for (int i = 0; i < count; i += 2) put_str(b, pairs[i], pairs[i + 1]);
    check_ok(dftu_store_batch_commit(b, NULL));
}

static dftu_store_snapshot_t* take_snapshot(dftu_store_t* store) {
    check_ok(dftu_store_flush(store, NULL));
    dftu_store_snapshot_t* snap = NULL;
    check_ok(dftu_store_snapshot(store, &snap, NULL));
    return snap;
}

static void expect_value(dftu_store_snapshot_t* snap, const dftu_store_ns_t* ns,
                         const dftu_store_scope* scope, const char* key,
                         const char* expected) {
    dftu_store_value_t* v = NULL;
    check_ok(dftu_store_snapshot_get(snap, ns, scope, (const uint8_t*)key,
                                     strlen(key), &v, NULL));
    if (!expected) {
        TEST_ASSERT_NULL(v);
        return;
    }
    TEST_ASSERT_NOT_NULL(v);
    dftu_store_bytes bytes = dftu_store_value_bytes(v);
    TEST_ASSERT_EQUAL_UINT64(strlen(expected), bytes.size);
    TEST_ASSERT_EQUAL_MEMORY(expected, bytes.data, bytes.size);
    dftu_store_value_free(v);
}

static void test_free_null(void) {
    dftu_store_free(NULL);
    dftu_store_ns_free(NULL);
    dftu_store_batch_free(NULL);
    dftu_store_snapshot_free(NULL);
    dftu_store_value_free(NULL);
    dftu_store_value_list_free(NULL);
    dftu_store_cursor_free(NULL);
}

static void check_invalid(dftu_status status) {
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, status);
}

static void test_null_arguments(void) {
    dftu_store_t* opened = NULL;
    dftu_error err = DFTU_ERROR_INIT;
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, dftu_store_open(NULL, &opened, &err));
    TEST_ASSERT_NULL(opened);
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, err.status);
    TEST_ASSERT_NOT_NULL(err.message);
    dftu_error_release(&err);

    check_invalid(dftu_store_flush(NULL, NULL));
    check_invalid(dftu_store_close(NULL, NULL));
    check_invalid(dftu_store_batch_commit(NULL, NULL));
    check_invalid(dftu_store_batch_put(NULL, NULL, 0, NULL, 0, NULL));
    dftu_store_pack_report report;
    check_invalid(dftu_store_pack(NULL, NULL, &report, NULL));
    dftu_store_snapshot_t* snap_out = NULL;
    check_invalid(dftu_store_snapshot(NULL, &snap_out, NULL));
    dftu_store_ns_t* ns_out = NULL;
    check_invalid(dftu_store_ns(NULL, "n", 0, 0, 0, 0, &ns_out, NULL));
    check_invalid(dftu_store_cursor_next(NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, dftu_store_cursor_key(NULL).size);
    TEST_ASSERT_EQUAL_UINT64(0, dftu_store_value_bytes(NULL).size);

    dftu_store_t* store = open_store();
    check_invalid(dftu_store_ns(store, NULL, 0, 0, 0, 0, &ns_out, NULL));
    check_invalid(dftu_store_ns(store, "", 0, 0, 0, 0, &ns_out, NULL));
    check_invalid(dftu_store_ns(store, "n", 0, 99, 0, 0, &ns_out, NULL));
    check_invalid(dftu_store_ns(store, "n", 0, 0, 0, 0, NULL, NULL));
    TEST_ASSERT_NULL(ns_out);
    dftu_store_ns_t* ns =
        make_ns(store, "dftu.test.null", DFTU_STORE_FOLD_NONE);
    dftu_store_batch_t* nb = NULL;
    check_invalid(dftu_store_batch(store, ns, NULL, 0, NULL, 0, &nb, NULL));
    dftu_store_scope bad = {DFTU_STORE_SCOPE_FILE, NULL, 0};
    check_invalid(dftu_store_batch(store, ns, &bad, 0, NULL, 0, &nb, NULL));
    TEST_ASSERT_NULL(nb);
    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

static void test_error_is_caller_owned(void) {
    dftu_error err = DFTU_ERROR_INIT;
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, dftu_store_flush(NULL, &err));
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, err.status);
    TEST_ASSERT_NOT_NULL(err.message);
    TEST_ASSERT_NOT_NULL(err.release);
    const char* first = err.message;
    TEST_ASSERT_TRUE(strlen(first) > 0);

    /* A second failure releases the first message and fills it again. */
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, dftu_store_close(NULL, &err));
    TEST_ASSERT_NOT_NULL(err.message);
    dftu_error_release(&err);
    TEST_ASSERT_NULL(err.message);

    /* A success leaves a filled err as it was. */
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, dftu_store_flush(NULL, &err));
    char saved[256];
    strncpy(saved, err.message, sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';
    dftu_store_t* store = open_store();
    TEST_ASSERT_EQUAL_INT(DFTU_OK, dftu_store_flush(store, &err));
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, err.status);
    TEST_ASSERT_EQUAL_STRING(saved, err.message);
    dftu_error_release(&err);
    dftu_store_free(store);
}

static void test_pack_options_struct_size(void) {
    dftu_store_t* store = open_store();
    dftu_store_pack_options options;
    dftu_store_pack_options_init(&options);
    TEST_ASSERT_EQUAL_UINT32(sizeof(options), options.struct_size);
    dftu_store_pack_report report;

    /* An older caller whose struct ends after the first field. */
    options.struct_size =
        (uint32_t)(offsetof(dftu_store_pack_options, small_file_bytes) +
                   sizeof(options.small_file_bytes));
    check_ok(dftu_store_pack(store, &options, &report, NULL));

    /* A newer caller. */
    options.struct_size = (uint32_t)sizeof(options) + 64;
    check_ok(dftu_store_pack(store, &options, &report, NULL));

    options.struct_size = 0;
    dftu_error err = DFTU_ERROR_INIT;
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID,
                          dftu_store_pack(store, &options, &report, &err));
    TEST_ASSERT_NOT_NULL(err.message);
    dftu_error_release(&err);
    dftu_store_free(store);
}

static void test_huge_counts_are_invalid(void) {
    dftu_store_t* store = open_store();
    dftu_store_ns_t* ns =
        make_ns(store, "dftu.test.huge", DFTU_STORE_FOLD_NONE);
    const char* path = "a.pfw";
    dftu_store_scope huge_scope = {DFTU_STORE_SCOPE_FILES, &path, UINT64_MAX};
    dftu_store_batch_t* nb = NULL;
    check_invalid(
        dftu_store_batch(store, ns, &huge_scope, 0, NULL, 0, &nb, NULL));

    dftu_store_source_stat stat = {0, 0, 0};
    check_invalid(dftu_store_batch(store, ns, &FILE_SCOPE, 0, &stat, UINT64_MAX,
                                   &nb, NULL));

    dftu_store_snapshot_t* snap = take_snapshot(store);
    dftu_store_bytes key = {(const uint8_t*)"k", 1};
    dftu_store_value_list_t* list = NULL;
    check_invalid(dftu_store_snapshot_multi_get(snap, ns, &FILE_SCOPE, &key,
                                                UINT64_MAX, &list, NULL));
    TEST_ASSERT_NULL(list);

    dftu_store_snapshot_free(snap);
    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

static void test_put_commit_and_read_back(void) {
    dftu_store_t* store = open_store();
    dftu_store_ns_t* ns = make_ns(store, "dftu.test.rw", DFTU_STORE_FOLD_NONE);
    const char* rows[] = {"k1", "v1", "k2", "value two"};
    commit_pairs(store, ns, &FILE_SCOPE, rows, 4);

    dftu_store_snapshot_t* snap = take_snapshot(store);
    expect_value(snap, ns, &FILE_SCOPE, "k1", "v1");
    expect_value(snap, ns, &FILE_SCOPE, "k2", "value two");
    expect_value(snap, ns, &FILE_SCOPE, "missing", NULL);
    expect_value(snap, ns, &ROOT_SCOPE, "k1", NULL);

    dftu_store_bytes keys[] = {{(const uint8_t*)"k2", 2},
                               {(const uint8_t*)"nope", 4},
                               {(const uint8_t*)"k1", 2}};
    dftu_store_value_list_t* list = NULL;
    check_ok(dftu_store_snapshot_multi_get(snap, ns, &FILE_SCOPE, keys, 3,
                                           &list, NULL));
    TEST_ASSERT_EQUAL_UINT64(3, dftu_store_value_list_count(list));
    dftu_store_bytes out;
    TEST_ASSERT_EQUAL_INT32(1, dftu_store_value_list_at(list, 0, &out));
    TEST_ASSERT_EQUAL_UINT64(9, out.size);
    TEST_ASSERT_EQUAL_INT32(0, dftu_store_value_list_at(list, 1, &out));
    TEST_ASSERT_EQUAL_INT32(1, dftu_store_value_list_at(list, 2, &out));
    TEST_ASSERT_EQUAL_MEMORY("v1", out.data, 2);
    TEST_ASSERT_EQUAL_INT32(-1, dftu_store_value_list_at(list, 3, &out));
    dftu_store_value_list_free(list);

    dftu_store_snapshot_free(snap);
    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

static void test_discarded_batch_writes_nothing(void) {
    dftu_store_t* store = open_store();
    dftu_store_ns_t* ns =
        make_ns(store, "dftu.test.discard", DFTU_STORE_FOLD_NONE);
    dftu_store_batch_t* b = new_batch(store, ns, &FILE_SCOPE);
    put_str(b, "k", "v");
    dftu_store_batch_free(b);
    dftu_store_snapshot_t* snap = take_snapshot(store);
    expect_value(snap, ns, &FILE_SCOPE, "k", NULL);
    dftu_store_snapshot_free(snap);
    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

static void test_erase_hides_a_key(void) {
    dftu_store_t* store = open_store();
    dftu_store_ns_t* ns =
        make_ns(store, "dftu.test.erase", DFTU_STORE_FOLD_NONE);
    const char* rows[] = {"a", "1", "b", "2", "c", "3", "d", "4"};
    commit_pairs(store, ns, &FILE_SCOPE, rows, 8);
    check_ok(dftu_store_flush(store, NULL));

    dftu_store_batch_t* b = new_batch(store, ns, &FILE_SCOPE);
    check_ok(dftu_store_batch_erase(b, (const uint8_t*)"a", 1, NULL));
    check_ok(dftu_store_batch_erase_range(b, (const uint8_t*)"c", 1,
                                          (const uint8_t*)"d", 1, NULL));
    check_ok(dftu_store_batch_commit(b, NULL));

    dftu_store_snapshot_t* snap = take_snapshot(store);
    expect_value(snap, ns, &FILE_SCOPE, "a", NULL);
    expect_value(snap, ns, &FILE_SCOPE, "b", "2");
    expect_value(snap, ns, &FILE_SCOPE, "c", NULL);
    expect_value(snap, ns, &FILE_SCOPE, "d", "4");
    dftu_store_snapshot_free(snap);
    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

static void test_scan_prefix_and_range_in_key_order(void) {
    dftu_store_t* store = open_store();
    dftu_store_ns_t* ns =
        make_ns(store, "dftu.test.scan", DFTU_STORE_FOLD_NONE);
    const char* rows[] = {"b2", "B2", "a1", "A1", "b1", "B1", "c1", "C1"};
    commit_pairs(store, ns, &FILE_SCOPE, rows, 8);
    dftu_store_snapshot_t* snap = take_snapshot(store);

    dftu_store_cursor_t* cur = NULL;
    check_ok(dftu_store_snapshot_scan_prefix(
        snap, ns, &FILE_SCOPE, (const uint8_t*)"b", 1, &cur, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, dftu_store_cursor_key(cur).size);
    TEST_ASSERT_EQUAL_INT(DFTU_OK, dftu_store_cursor_next(cur, NULL));
    TEST_ASSERT_EQUAL_MEMORY("b1", dftu_store_cursor_key(cur).data, 2);
    dftu_store_bytes value;
    check_ok(dftu_store_cursor_value(cur, &value, NULL));
    TEST_ASSERT_EQUAL_MEMORY("B1", value.data, 2);
    TEST_ASSERT_EQUAL_INT(DFTU_OK, dftu_store_cursor_next(cur, NULL));
    TEST_ASSERT_EQUAL_MEMORY("b2", dftu_store_cursor_key(cur).data, 2);
    TEST_ASSERT_EQUAL_INT(DFTU_END, dftu_store_cursor_next(cur, NULL));
    TEST_ASSERT_EQUAL_INT(DFTU_END, dftu_store_cursor_next(cur, NULL));
    check_invalid(dftu_store_cursor_value(cur, &value, NULL));
    dftu_store_cursor_free(cur);

    check_ok(dftu_store_snapshot_scan_range(
        snap, ns, &FILE_SCOPE, (const uint8_t*)"a1", 2, (const uint8_t*)"b2", 2,
        &cur, NULL));
    const char* expected[] = {"a1", "b1"};
    int seen = 0;
    while (dftu_store_cursor_next(cur, NULL) == DFTU_OK) {
        TEST_ASSERT_TRUE(seen < 2);
        TEST_ASSERT_EQUAL_MEMORY(expected[seen],
                                 dftu_store_cursor_key(cur).data, 2);
        seen++;
    }
    TEST_ASSERT_EQUAL_INT(2, seen);
    dftu_store_cursor_free(cur);

    dftu_store_snapshot_free(snap);
    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

static void test_fold_namespace_sums(void) {
    dftu_store_t* store = open_store();
    dftu_store_ns_t* ns =
        make_ns(store, "dftu.test.fold", DFTU_STORE_FOLD_SUM_I64);
    int64_t one = 1;
    int64_t two = 2;
    dftu_store_batch_t* b = new_batch(store, ns, &ROOT_SCOPE);
    check_ok(dftu_store_batch_fold(b, (const uint8_t*)"n", 1,
                                   (const uint8_t*)&one, sizeof(one), NULL));
    check_ok(dftu_store_batch_fold(b, (const uint8_t*)"n", 1,
                                   (const uint8_t*)&two, sizeof(two), NULL));
    check_ok(dftu_store_batch_commit(b, NULL));

    dftu_store_snapshot_t* snap = take_snapshot(store);
    dftu_store_value_t* r = NULL;
    check_ok(dftu_store_snapshot_get(snap, ns, &ROOT_SCOPE, (const uint8_t*)"n",
                                     1, &r, NULL));
    dftu_store_bytes bytes = dftu_store_value_bytes(r);
    TEST_ASSERT_EQUAL_UINT64(8, bytes.size);
    int64_t sum;
    memcpy(&sum, bytes.data, sizeof(sum));
    TEST_ASSERT_EQUAL_INT64(3, sum);
    dftu_store_value_free(r);

    b = new_batch(store, ns, &ROOT_SCOPE);
    put_str(b, "n", "x");
    dftu_error err = DFTU_ERROR_INIT;
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, dftu_store_batch_commit(b, &err));
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, err.status);
    dftu_error_release(&err);

    dftu_store_snapshot_free(snap);
    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

static void test_building_batch_publish_and_copy_info(void) {
    dftu_store_t* store = open_store();
    dftu_store_ns_t* ns =
        make_ns(store, "dftu.test.publish", DFTU_STORE_FOLD_NONE);
    dftu_store_snapshot_t* snap = take_snapshot(store);
    dftu_store_copy absent;
    check_ok(dftu_store_snapshot_copy_info(snap, ns, &FILE_SCOPE, NULL, 0,
                                           &absent, NULL));
    TEST_ASSERT_EQUAL_INT(DFTU_STORE_STATE_ABSENT, absent.state);
    dftu_store_snapshot_free(snap);

    dftu_store_batch_t* br = NULL;
    check_ok(dftu_store_building_batch(store, ns, &FILE_SCOPE, &br, NULL));
    put_str(br, "k", "built");
    check_ok(dftu_store_batch_commit(br, NULL));

    snap = take_snapshot(store);
    expect_value(snap, ns, &FILE_SCOPE, "k", NULL);
    dftu_store_snapshot_free(snap);

    dftu_store_source_stat source = {11, 22, 33};
    dftu_store_publish_target targets[1];
    memset(targets, 0, sizeof(targets));
    targets[0].ns = ns;
    targets[0].scope = &FILE_SCOPE;
    targets[0].params_hash = 77;
    targets[0].sources = &source;
    targets[0].source_count = 1;
    targets[0].change = DFTU_STORE_PUBLISH_READY;
    check_ok(dftu_store_publish(store, targets, 1, NULL));

    snap = take_snapshot(store);
    expect_value(snap, ns, &FILE_SCOPE, "k", "built");
    dftu_store_source_stat got[2];
    memset(got, 0, sizeof(got));
    dftu_store_copy info;
    check_ok(dftu_store_snapshot_copy_info(snap, ns, &FILE_SCOPE, got, 2, &info,
                                           NULL));
    TEST_ASSERT_EQUAL_INT(DFTU_STORE_STATE_READY, info.state);
    TEST_ASSERT_EQUAL_UINT64(77, info.params_hash);
    TEST_ASSERT_EQUAL_UINT64(1, info.source_count);
    TEST_ASSERT_EQUAL_UINT64(11, got[0].mtime);
    TEST_ASSERT_EQUAL_UINT64(22, got[0].size);
    TEST_ASSERT_EQUAL_UINT64(33, got[0].hash);
    dftu_store_snapshot_free(snap);

    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

static void test_refresh(void) {
    check_invalid(dftu_store_refresh(NULL, NULL));
    dftu_store_t* store = open_store();
    check_ok(dftu_store_refresh(store, NULL));
    dftu_store_free(store);
}

static void test_fail_and_drop(void) {
    dftu_store_t* store = open_store();
    dftu_store_ns_t* ns =
        make_ns(store, "dftu.test.faildrop", DFTU_STORE_FOLD_NONE);
    check_ok(dftu_store_fail(store, ns, &FILE_SCOPE, 5, NULL));
    dftu_store_snapshot_t* snap = take_snapshot(store);
    dftu_store_copy r;
    check_ok(dftu_store_snapshot_copy_info(snap, ns, &FILE_SCOPE, NULL, 0, &r,
                                           NULL));
    TEST_ASSERT_EQUAL_INT(DFTU_STORE_STATE_FAILED, r.state);
    TEST_ASSERT_EQUAL_UINT64(5, r.params_hash);
    dftu_store_snapshot_free(snap);

    const char* rows[] = {"k", "v"};
    commit_pairs(store, ns, &FILE_SCOPE, rows, 2);
    check_ok(dftu_store_drop(store, ns, &FILE_SCOPE, NULL));
    snap = take_snapshot(store);
    expect_value(snap, ns, &FILE_SCOPE, "k", NULL);
    dftu_store_snapshot_free(snap);

    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

static void test_pack_keeps_the_data(void) {
    dftu_store_t* store = open_store();
    dftu_store_ns_t* ns =
        make_ns(store, "dftu.test.pack", DFTU_STORE_FOLD_NONE);
    const char* first[] = {"p1", "one"};
    const char* second[] = {"p2", "two"};
    commit_pairs(store, ns, &FILE_SCOPE, first, 2);
    check_ok(dftu_store_close(store, NULL));
    const char* other_path[] = {"/traces/b.pfw"};
    dftu_store_scope other = {DFTU_STORE_SCOPE_FILE, other_path, 1};
    commit_pairs(store, ns, &other, second, 2);
    check_ok(dftu_store_close(store, NULL));

    dftu_store_pack_options options;
    dftu_store_pack_options_init(&options);
    TEST_ASSERT_TRUE(options.small_file_bytes > 0);
    options.small_file_bytes = (uint64_t)1 << 30;
    options.keep_seconds = 0;
    dftu_store_pack_report r;
    check_ok(dftu_store_pack(store, &options, &r, NULL));
    TEST_ASSERT_TRUE(r.files_packed >= 2);
    TEST_ASSERT_TRUE(r.copies_written >= 2);

    dftu_store_snapshot_t* snap = take_snapshot(store);
    expect_value(snap, ns, &FILE_SCOPE, "p1", "one");
    expect_value(snap, ns, &other, "p2", "two");
    dftu_store_snapshot_free(snap);

    check_ok(dftu_store_pack(store, NULL, &r, NULL));
    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

static void test_files_scope(void) {
    dftu_store_t* store = open_store();
    dftu_store_ns_t* ns =
        make_ns(store, "dftu.test.files", DFTU_STORE_FOLD_NONE);
    const char* paths[] = {"/traces/b.pfw", "/traces/a.pfw", "/traces/a.pfw"};
    dftu_store_scope files = {DFTU_STORE_SCOPE_FILES, paths, 3};
    const char* rows[] = {"k", "joint"};
    commit_pairs(store, ns, &files, rows, 2);
    dftu_store_snapshot_t* snap = take_snapshot(store);
    const char* reordered[] = {"/traces/a.pfw", "/traces/b.pfw"};
    dftu_store_scope same = {DFTU_STORE_SCOPE_FILES, reordered, 2};
    expect_value(snap, ns, &same, "k", "joint");
    expect_value(snap, ns, &FILE_SCOPE, "k", NULL);
    dftu_store_scope none = {DFTU_STORE_SCOPE_FILES, NULL, 0};
    dftu_store_value_t* r = NULL;
    check_invalid(dftu_store_snapshot_get(snap, ns, &none, (const uint8_t*)"k",
                                          1, &r, NULL));
    TEST_ASSERT_NULL(r);
    dftu_store_snapshot_free(snap);
    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_free_null);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_error_is_caller_owned);
    RUN_TEST(test_pack_options_struct_size);
    RUN_TEST(test_huge_counts_are_invalid);
    RUN_TEST(test_put_commit_and_read_back);
    RUN_TEST(test_discarded_batch_writes_nothing);
    RUN_TEST(test_erase_hides_a_key);
    RUN_TEST(test_scan_prefix_and_range_in_key_order);
    RUN_TEST(test_fold_namespace_sums);
    RUN_TEST(test_building_batch_publish_and_copy_info);
    RUN_TEST(test_refresh);
    RUN_TEST(test_fail_and_drop);
    RUN_TEST(test_pack_keeps_the_data);
    RUN_TEST(test_files_scope);
    return UNITY_END();
}
