// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/trace/index/abi.h>
#include <dftracer/utils/trace/views/abi.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <unity.h>

static test_environment_handle_t g_env = NULL;

void setUp(void) {
    g_env = test_environment_create();
    TEST_ASSERT_NOT_NULL(g_env);
}

void tearDown(void) {
    test_environment_destroy(g_env);
    g_env = NULL;
}

static void check_ok(dftu_status status) {
    TEST_ASSERT_EQUAL_INT(DFTU_OK, status);
}

static void check_invalid(dftu_status status) {
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, status);
}

static dftu_indexer* open_with(const char* const* paths, uint64_t n,
                               const dftu_indexer_options* options) {
    dftu_indexer* ix = NULL;
    check_ok(dftu_indexer_open(paths, n, options, &ix, NULL));
    TEST_ASSERT_NOT_NULL(ix);
    return ix;
}

static dftu_indexer* open_one(const char* path) {
    const char* paths[] = {path};
    return open_with(paths, 1, NULL);
}

static dftu_indexer_report build_ok(dftu_indexer* ix) {
    dftu_indexer_report report;
    check_ok(dftu_indexer_build(ix, &report, NULL));
    return report;
}

static dftu_indexer_report status_ok(dftu_indexer* ix) {
    dftu_indexer_report report;
    check_ok(dftu_indexer_status(ix, &report, NULL));
    return report;
}

static char* manifest_ok(dftu_indexer* ix) {
    char* json = NULL;
    check_ok(dftu_indexer_manifest(ix, &json, NULL));
    TEST_ASSERT_NOT_NULL(json);
    return json;
}

static dftu_indexer_file_list* files_ok(dftu_indexer* ix) {
    dftu_indexer_file_list* list = NULL;
    check_ok(dftu_indexer_files(ix, &list, NULL));
    return list;
}

static uint64_t file_size(const char* path) {
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(path, &st));
    return (uint64_t)st.st_size;
}

static void test_open_with_default_options(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 10);
    dftu_indexer* ix = open_one(trace);
    dftu_indexer_free(ix);
    free(trace);
}

static void test_open_invalid_arguments(void) {
    const char* one[] = {"x.pfw.gz"};
    const char* with_null[] = {NULL};
    dftu_indexer* ix = NULL;
    dftu_error err = DFTU_ERROR_INIT;
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID,
                          dftu_indexer_open(NULL, 1, NULL, &ix, &err));
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, err.status);
    dftu_error_release(&err);
    check_invalid(dftu_indexer_open(one, 0, NULL, &ix, NULL));
    check_invalid(dftu_indexer_open(with_null, 1, NULL, &ix, NULL));
    check_invalid(dftu_indexer_open(one, 1, NULL, NULL, NULL));
    TEST_ASSERT_NULL(ix);
}

static void test_error_is_caller_owned(void) {
    dftu_error err = DFTU_ERROR_INIT;
    dftu_indexer_report report;
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID,
                          dftu_indexer_build(NULL, &report, &err));
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, err.status);
    TEST_ASSERT_NOT_NULL(err.message);
    TEST_ASSERT_NOT_NULL(err.release);

    /* A success leaves a filled err as it was. */
    char saved[256];
    strncpy(saved, err.message, sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';
    char* trace = test_environment_create_dft_gzip_file(g_env, 10);
    dftu_indexer* ix = open_one(trace);
    TEST_ASSERT_EQUAL_INT(DFTU_OK, dftu_indexer_status(ix, &report, &err));
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, err.status);
    TEST_ASSERT_EQUAL_STRING(saved, err.message);

    /* A second failure replaces the message. */
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID,
                          dftu_indexer_drop_extension(ix, NULL, &report, &err));
    TEST_ASSERT_NOT_NULL(err.message);
    dftu_error_release(&err);
    TEST_ASSERT_NULL(err.message);

    dftu_indexer_free(ix);
    free(trace);
}

static void test_options_struct_size(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 10);
    const char* paths[] = {trace};
    dftu_indexer_options opts;
    dftu_indexer_options_init(&opts);
    TEST_ASSERT_EQUAL_UINT32(sizeof(opts), opts.struct_size);

    /* An older caller whose struct ends after the first field. */
    opts.struct_size = (uint32_t)(offsetof(dftu_indexer_options, index_dir) +
                                  sizeof(opts.index_dir));
    dftu_indexer_free(open_with(paths, 1, &opts));

    /* A newer caller. */
    opts.struct_size = (uint32_t)sizeof(opts) + 64;
    dftu_indexer_free(open_with(paths, 1, &opts));

    dftu_indexer* ix = NULL;
    opts.struct_size = 0;
    dftu_error err = DFTU_ERROR_INIT;
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID,
                          dftu_indexer_open(paths, 1, &opts, &ix, &err));
    TEST_ASSERT_NOT_NULL(err.message);
    dftu_error_release(&err);
    TEST_ASSERT_NULL(ix);
    free(trace);
}

static void test_free_null(void) {
    dftu_indexer_free(NULL);
    dftu_indexer_file_list_free(NULL);
}

static void test_build_then_status(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 10);
    dftu_indexer* ix = open_one(trace);

    TEST_ASSERT_EQUAL_UINT64(1, build_ok(ix).indexed);

    dftu_indexer_report st = status_ok(ix);
    TEST_ASSERT_EQUAL_UINT64(1, st.total);
    TEST_ASSERT_EQUAL_UINT64(1, st.ready);
    TEST_ASSERT_EQUAL_UINT64(0, st.needs_work);
    TEST_ASSERT_EQUAL_UINT64(0, st.indexed);

    dftu_indexer_free(ix);
    free(trace);
}

static void test_build_failure_names_the_trace(void) {
    if (geteuid() == 0) TEST_IGNORE_MESSAGE("root reads every file");
    char* trace = test_environment_create_dft_gzip_file(g_env, 10);
    dftu_indexer* ix = open_one(trace);
    TEST_ASSERT_EQUAL_INT(0, chmod(trace, 0));

    dftu_indexer_report report;
    dftu_error err = DFTU_ERROR_INIT;
    TEST_ASSERT_TRUE(dftu_indexer_build(ix, &report, &err) < 0);
    TEST_ASSERT_NOT_NULL(err.message);
    TEST_ASSERT_NOT_NULL(strstr(err.message, trace));
    dftu_error_release(&err);

    TEST_ASSERT_EQUAL_INT(0, chmod(trace, 0644));
    dftu_indexer_free(ix);
    free(trace);
}

static void test_file_list(void) {
    char* a = test_environment_create_dft_gzip_file(g_env, 10);
    char* b = test_environment_create_dft_gzip_file(g_env, 20);
    const char* paths[] = {a, b};
    dftu_indexer* ix = open_with(paths, 2, NULL);
    build_ok(ix);

    dftu_indexer_file_list* list = files_ok(ix);
    dftu_indexer_free(ix);  // the list outlives its indexer

    TEST_ASSERT_EQUAL_UINT64(2, dftu_indexer_file_list_count(list));
    for (uint64_t i = 0; i < 2; ++i) {
        dftu_indexer_file f;
        check_ok(dftu_indexer_file_list_get(list, i, &f, NULL));
        TEST_ASSERT_EQUAL_UINT64(file_size(f.path), f.size_bytes);
        TEST_ASSERT_TRUE(f.size_bytes > 0);
        TEST_ASSERT_TRUE(strlen(f.index_path) > 0);
    }
    dftu_indexer_file out;
    check_invalid(dftu_indexer_file_list_get(list, 2, &out, NULL));

    dftu_indexer_file_list_free(list);
    free(a);
    free(b);
}

static char* cut_copy(const char* src, double frac) {
    FILE* in = fopen(src, "rb");
    TEST_ASSERT_NOT_NULL(in);
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    long keep = (long)((double)size * frac);
    char* bytes = malloc((size_t)keep);
    TEST_ASSERT_EQUAL_size_t((size_t)keep, fread(bytes, 1, (size_t)keep, in));
    fclose(in);
    size_t len = strlen(src) + 8;
    char* dst = malloc(len);
    snprintf(dst, len, "%s.cut.gz", src);
    FILE* out = fopen(dst, "wb");
    TEST_ASSERT_NOT_NULL(out);
    fwrite(bytes, 1, (size_t)keep, out);
    fclose(out);
    free(bytes);
    return dst;
}

static void test_truncated_count(void) {
    char* full = test_environment_create_dft_gzip_file(g_env, 2000);
    char* cut = cut_copy(full, 0.5);
    const char* paths[] = {full, cut};
    dftu_indexer* ix = open_with(paths, 2, NULL);
    TEST_ASSERT_EQUAL_UINT64(1, build_ok(ix).truncated);

    dftu_indexer_file_list* list = files_ok(ix);
    for (uint64_t i = 0; i < dftu_indexer_file_list_count(list); ++i) {
        dftu_indexer_file f;
        check_ok(dftu_indexer_file_list_get(list, i, &f, NULL));
        TEST_ASSERT_EQUAL_INT(strcmp(f.path, cut) == 0, f.truncated);
    }
    dftu_indexer_file_list_free(list);
    dftu_indexer_free(ix);
    free(full);
    free(cut);
}

static void test_build_then_count_through_view(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 25);
    dftu_indexer* ix = open_one(trace);
    build_ok(ix);
    dftu_indexer_free(ix);

    const char* paths[] = {trace};
    dftu_view* v = dftu_view_from_files(paths, NULL, 1);
    TEST_ASSERT_NOT_NULL(v);
    dftu_dataframe* df = dftu_view_collect(v, NULL);
    TEST_ASSERT_NOT_NULL(df);
    TEST_ASSERT_EQUAL_INT64(25, dftu_dataframe_num_rows(df));
    dftu_dataframe_free(df);
    dftu_view_free(v);
    free(trace);
}

static void test_manifest_and_explain_json(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 25);
    dftu_indexer* ix = open_one(trace);
    build_ok(ix);

    char* m = manifest_ok(ix);
    TEST_ASSERT_NOT_NULL(strstr(m, "\"name\":\"counts\""));
    TEST_ASSERT_NOT_NULL(strstr(m, "\"current\":true"));
    dftu_indexer_string_free(m);

    char* e = NULL;
    check_ok(dftu_indexer_explain(ix, "name == \"absent\"", &e, NULL));
    TEST_ASSERT_NOT_NULL(strstr(e, "\"may_match\":false"));
    dftu_indexer_string_free(e);

    char* none = NULL;
    check_invalid(dftu_indexer_explain(ix, "name ==", &none, NULL));
    TEST_ASSERT_NULL(none);
    check_invalid(dftu_indexer_explain(ix, NULL, &none, NULL));
    check_invalid(dftu_indexer_manifest(NULL, &none, NULL));
    TEST_ASSERT_NULL(none);
    dftu_indexer_string_free(NULL);

    dftu_indexer_free(ix);
    free(trace);
}

static void test_drop_then_build_extension(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 25);
    dftu_indexer* ix = open_one(trace);
    build_ok(ix);

    dftu_indexer_report report;
    check_invalid(dftu_indexer_drop_extension(ix, "nope", &report, NULL));
    check_invalid(dftu_indexer_drop_extension(ix, NULL, &report, NULL));

    check_ok(dftu_indexer_drop_extension(ix, "counts", &report, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, report.needs_work);
    char* m = manifest_ok(ix);
    TEST_ASSERT_NULL(strstr(m, "\"name\":\"counts\""));
    dftu_indexer_string_free(m);

    TEST_ASSERT_EQUAL_UINT64(0, build_ok(ix).needs_work);

    check_ok(dftu_indexer_rebuild_extension(ix, "bloom", &report, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, report.ready);
    dftu_indexer_free(ix);

    const char* only[] = {"zonemap"};
    dftu_indexer_options opts;
    dftu_indexer_options_init(&opts);
    opts.extensions = only;
    opts.extension_count = 1;
    const char* paths[] = {trace};
    dftu_indexer_free(open_with(paths, 1, &opts));
    const char* bad[] = {"dft.stats"};
    opts.extensions = bad;
    dftu_indexer* none = NULL;
    check_invalid(dftu_indexer_open(paths, 1, &opts, &none, NULL));
    free(trace);
}

static void test_schema_option_and_file_field(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 25);
    const char* paths[] = {trace};
    dftu_indexer_options opts;
    dftu_indexer_options_init(&opts);
    TEST_ASSERT_NULL(opts.schema);
    opts.schema = "generic";
    dftu_indexer* ix = open_with(paths, 1, &opts);
    build_ok(ix);
    dftu_indexer_file_list* list = files_ok(ix);
    dftu_indexer_file f;
    check_ok(dftu_indexer_file_list_get(list, 0, &f, NULL));
    TEST_ASSERT_EQUAL_STRING("generic", f.schema);
    dftu_indexer_file_list_free(list);
    dftu_indexer_free(ix);

    opts.schema = "nope";
    dftu_indexer* none = NULL;
    check_invalid(dftu_indexer_open(paths, 1, &opts, &none, NULL));
    free(trace);
}

static void test_schema_registry_calls(void) {
    char* id = NULL;
    check_ok(dftu_schema_register(
        "id: c_abi_schema\nfields: {status: {type: int}}\n", "c.yaml", &id,
        NULL));
    TEST_ASSERT_EQUAL_STRING("c_abi_schema", id);
    dftu_indexer_string_free(id);

    check_ok(dftu_schema_register(
        "id: c_abi_schema\nfields: {status: {type: int}}\n", NULL, &id, NULL));
    dftu_indexer_string_free(id);

    dftu_error err = DFTU_ERROR_INIT;
    char* other = NULL;
    TEST_ASSERT_EQUAL_INT(
        DFTU_E_INVALID,
        dftu_schema_register("id: c_abi_schema\nfields: {other: {type: int}}\n",
                             "d.yaml", &other, &err));
    TEST_ASSERT_NULL(other);
    TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID, err.status);
    TEST_ASSERT_NOT_NULL(strstr(err.message, "c.yaml"));
    dftu_error_release(&err);

    char* list = NULL;
    check_ok(dftu_schema_list(&list, NULL));
    TEST_ASSERT_NOT_NULL(strstr(list, "\"c_abi_schema\""));
    TEST_ASSERT_NOT_NULL(strstr(list, "\"dftracer\""));
    dftu_indexer_string_free(list);

    char* none = NULL;
    check_invalid(dftu_schema_register(NULL, NULL, &none, NULL));
    TEST_ASSERT_TRUE(dftu_schema_load("/no/such/dir", &none, NULL) < 0);
    TEST_ASSERT_NULL(none);

    char* trace = test_environment_create_dft_gzip_file(g_env, 25);
    char* d = NULL;
    check_ok(dftu_schema_detect(trace, &d, NULL));
    TEST_ASSERT_EQUAL_STRING("dftracer", d);
    dftu_indexer_string_free(d);

    char* e = NULL;
    check_ok(dftu_schema_explain(trace, &e, NULL));
    TEST_ASSERT_NOT_NULL(strstr(e, "\"chosen\":\"dftracer\""));
    TEST_ASSERT_NOT_NULL(strstr(e, "\"c_abi_schema\""));
    dftu_indexer_string_free(e);
    check_invalid(dftu_schema_explain(NULL, &none, NULL));

    const char* paths[] = {trace};
    dftu_view* v = dftu_view_from_files(paths, NULL, 1);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_NULL(dftu_view_record_schema(v, "no_such_schema"));
    dftu_view* g = dftu_view_record_schema(v, "generic");
    TEST_ASSERT_NOT_NULL(g);
    char* tree = dftu_view_schema_tree(v);
    TEST_ASSERT_NOT_NULL(tree);
    TEST_ASSERT_NOT_NULL(strstr(tree, "\"path\":\"ph\""));
    dftu_view_string_free(tree);
    TEST_ASSERT_NULL(dftu_view_schema_tree(NULL));
    dftu_dataframe* df = dftu_view_collect(g, NULL);
    TEST_ASSERT_NOT_NULL(df);
    TEST_ASSERT_TRUE(dftu_dataframe_num_rows(df) >= 25);
    dftu_dataframe_free(df);
    dftu_view_free(g);
    dftu_view_free(v);
    free(trace);
}

static void test_evidence_settings(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 25);
    const char* paths[] = {trace};
    dftu_indexer_options opts;
    dftu_indexer_options_init(&opts);
    TEST_ASSERT_EQUAL_UINT64(0, opts.bloom_path_budget);
    TEST_ASSERT_TRUE(opts.bloom_stats_share == 0.05);

    dftu_indexer* ix = open_with(paths, 1, &opts);
    build_ok(ix);
    dftu_indexer_free(ix);

    opts.bloom_stats_share = 0.5;
    ix = open_with(paths, 1, &opts);
    TEST_ASSERT_EQUAL_UINT64(1, status_ok(ix).needs_work);
    dftu_indexer_free(ix);

    opts.bloom_stats_share = 0.05;
    opts.bloom_path_budget = 3;
    ix = open_with(paths, 1, &opts);
    TEST_ASSERT_EQUAL_UINT64(1, status_ok(ix).needs_work);
    dftu_indexer_free(ix);

    opts.bloom_path_budget = 0;
    const double bad[] = {0.0, -0.5, 1.5};
    for (int i = 0; i < 3; ++i) {
        opts.bloom_stats_share = bad[i];
        dftu_indexer* none = NULL;
        dftu_error err = DFTU_ERROR_INIT;
        TEST_ASSERT_EQUAL_INT(DFTU_E_INVALID,
                              dftu_indexer_open(paths, 1, &opts, &none, &err));
        TEST_ASSERT_NOT_NULL(strstr(err.message, "stats_share"));
        dftu_error_release(&err);
    }
    free(trace);
}

static void test_aggregation_settings(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 25);
    const char* paths[] = {trace};
    dftu_indexer_options opts;
    dftu_indexer_options_init(&opts);
    TEST_ASSERT_EQUAL_UINT64(1000000, opts.aggregation_time_interval_us);
    TEST_ASSERT_EQUAL_INT(1, opts.aggregation_compute_percentiles);
    TEST_ASSERT_EQUAL_INT(1, opts.aggregation_group_by_file);

    dftu_indexer* ix = open_with(paths, 1, &opts);
    build_ok(ix);
    TEST_ASSERT_EQUAL_UINT64(0, status_ok(ix).needs_work);
    dftu_indexer_free(ix);

    // Another window is another tier, so the file needs work again.
    opts.aggregation_time_interval_us = 250000;
    ix = open_with(paths, 1, &opts);
    TEST_ASSERT_EQUAL_UINT64(1, status_ok(ix).needs_work);
    dftu_indexer_free(ix);
    free(trace);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_open_with_default_options);
    RUN_TEST(test_open_invalid_arguments);
    RUN_TEST(test_error_is_caller_owned);
    RUN_TEST(test_options_struct_size);
    RUN_TEST(test_free_null);
    RUN_TEST(test_build_then_status);
    RUN_TEST(test_build_failure_names_the_trace);
    RUN_TEST(test_file_list);
    RUN_TEST(test_build_then_count_through_view);
    RUN_TEST(test_truncated_count);
    RUN_TEST(test_manifest_and_explain_json);
    RUN_TEST(test_drop_then_build_extension);
    RUN_TEST(test_schema_option_and_file_field);
    RUN_TEST(test_schema_registry_calls);
    RUN_TEST(test_evidence_settings);
    RUN_TEST(test_aggregation_settings);
    return UNITY_END();
}
