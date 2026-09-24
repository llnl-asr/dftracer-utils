#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/index/abi.h>
#include <dftracer/utils/trace/views/abi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unity.h>

#include "testing_utilities.h"

static test_environment_handle_t g_env = NULL;

void setUp(void) {
    g_env = test_environment_create();
    TEST_ASSERT_NOT_NULL(g_env);
}

void tearDown(void) {
    test_environment_destroy(g_env);
    g_env = NULL;
}

static dftu_indexer* open_one(const char* path) {
    const char* paths[] = {path};
    dftu_indexer_open_result r = dftu_indexer_open(paths, 1, NULL);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(r));
    TEST_ASSERT_NOT_NULL(DFTU_RESULT_VALUE(r));
    return DFTU_RESULT_VALUE(r);
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
    dftu_indexer_open_result a = dftu_indexer_open(NULL, 1, NULL);
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(a));
    TEST_ASSERT_EQUAL_INT(DFTU_COND_INVALID_ARGUMENT,
                          DFTU_RESULT_ERROR(a).condition);
    dftu_indexer_open_result b = dftu_indexer_open(one, 0, NULL);
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(b));
    TEST_ASSERT_EQUAL_INT(DFTU_COND_INVALID_ARGUMENT,
                          DFTU_RESULT_ERROR(b).condition);
    dftu_indexer_open_result c = dftu_indexer_open(with_null, 1, NULL);
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(c));
    TEST_ASSERT_EQUAL_INT(DFTU_COND_INVALID_ARGUMENT,
                          DFTU_RESULT_ERROR(c).condition);
}

static void test_free_null(void) {
    dftu_indexer_free(NULL);
    dftu_indexer_file_list_free(NULL);
}

static void test_build_then_status(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 10);
    dftu_indexer* ix = open_one(trace);

    dftu_indexer_status_result built = dftu_indexer_build(ix);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(built));
    TEST_ASSERT_EQUAL_UINT64(1, DFTU_RESULT_VALUE(built).indexed);

    dftu_indexer_status_result st = dftu_indexer_status(ix);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(st));
    TEST_ASSERT_EQUAL_UINT64(1, DFTU_RESULT_VALUE(st).total);
    TEST_ASSERT_EQUAL_UINT64(1, DFTU_RESULT_VALUE(st).ready);
    TEST_ASSERT_EQUAL_UINT64(0, DFTU_RESULT_VALUE(st).needs_work);
    TEST_ASSERT_EQUAL_UINT64(0, DFTU_RESULT_VALUE(st).indexed);

    dftu_indexer_free(ix);
    free(trace);
}

static void test_build_failure_names_the_trace(void) {
    if (geteuid() == 0) TEST_IGNORE_MESSAGE("root reads every file");
    char* trace = test_environment_create_dft_gzip_file(g_env, 10);
    dftu_indexer* ix = open_one(trace);
    TEST_ASSERT_EQUAL_INT(0, chmod(trace, 0));

    dftu_indexer_status_result r = dftu_indexer_build(ix);
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(r));
    const char* message = DFTU_RESULT_ERROR(r).message;
    TEST_ASSERT_NOT_NULL(message);
    TEST_ASSERT_NOT_NULL(strstr(message, trace));

    TEST_ASSERT_EQUAL_INT(0, chmod(trace, 0644));
    dftu_indexer_free(ix);
    free(trace);
}

static void test_file_list(void) {
    char* a = test_environment_create_dft_gzip_file(g_env, 10);
    char* b = test_environment_create_dft_gzip_file(g_env, 20);
    const char* paths[] = {a, b};
    dftu_indexer_open_result o = dftu_indexer_open(paths, 2, NULL);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(o));
    dftu_indexer* ix = DFTU_RESULT_VALUE(o);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(dftu_indexer_build(ix)));

    dftu_indexer_files_result fr = dftu_indexer_files(ix);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(fr));
    dftu_indexer_file_list* list = DFTU_RESULT_VALUE(fr);
    dftu_indexer_free(ix);  // the list outlives its indexer

    TEST_ASSERT_EQUAL_UINT64(2, dftu_indexer_file_list_count(list));
    for (uint64_t i = 0; i < 2; ++i) {
        dftu_indexer_file_result f = dftu_indexer_file_list_get(list, i);
        TEST_ASSERT_TRUE(DFTU_RESULT_OK(f));
        TEST_ASSERT_EQUAL_UINT64(file_size(DFTU_RESULT_VALUE(f).path),
                                 DFTU_RESULT_VALUE(f).size_bytes);
        TEST_ASSERT_TRUE(DFTU_RESULT_VALUE(f).file_id >= 0);
    }
    dftu_indexer_file_result out = dftu_indexer_file_list_get(list, 2);
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(out));
    TEST_ASSERT_EQUAL_INT(DFTU_COND_INVALID_ARGUMENT,
                          DFTU_RESULT_ERROR(out).condition);

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
    dftu_indexer_open_result o = dftu_indexer_open(paths, 2, NULL);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(o));
    dftu_indexer* ix = DFTU_RESULT_VALUE(o);
    dftu_indexer_status_result built = dftu_indexer_build(ix);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(built));
    TEST_ASSERT_EQUAL_UINT64(1, DFTU_RESULT_VALUE(built).truncated);

    dftu_indexer_files_result fr = dftu_indexer_files(ix);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(fr));
    dftu_indexer_file_list* list = DFTU_RESULT_VALUE(fr);
    for (uint64_t i = 0; i < dftu_indexer_file_list_count(list); ++i) {
        dftu_indexer_file_result f = dftu_indexer_file_list_get(list, i);
        TEST_ASSERT_TRUE(DFTU_RESULT_OK(f));
        TEST_ASSERT_EQUAL_INT(strcmp(DFTU_RESULT_VALUE(f).path, cut) == 0,
                              DFTU_RESULT_VALUE(f).truncated);
    }
    dftu_indexer_file_list_free(list);
    dftu_indexer_free(ix);
    free(full);
    free(cut);
}

static void test_build_then_count_through_view(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 25);
    dftu_indexer* ix = open_one(trace);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(dftu_indexer_build(ix)));
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
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(dftu_indexer_build(ix)));

    dftu_indexer_string_result m = dftu_indexer_manifest(ix);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(m));
    TEST_ASSERT_NOT_NULL(strstr(DFTU_RESULT_VALUE(m), "\"name\":\"counts\""));
    TEST_ASSERT_NOT_NULL(strstr(DFTU_RESULT_VALUE(m), "\"current\":true"));
    dftu_indexer_string_free(DFTU_RESULT_VALUE(m));

    dftu_indexer_string_result e =
        dftu_indexer_explain(ix, "name == \"absent\"");
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(e));
    TEST_ASSERT_NOT_NULL(strstr(DFTU_RESULT_VALUE(e), "\"may_match\":false"));
    dftu_indexer_string_free(DFTU_RESULT_VALUE(e));

    dftu_indexer_string_result bad = dftu_indexer_explain(ix, "name ==");
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(bad));
    TEST_ASSERT_EQUAL_INT(DFTU_COND_INVALID_ARGUMENT,
                          DFTU_RESULT_ERROR(bad).condition);
    dftu_indexer_string_result null_query = dftu_indexer_explain(ix, NULL);
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(null_query));
    TEST_ASSERT_EQUAL_INT(DFTU_COND_INVALID_ARGUMENT,
                          DFTU_RESULT_ERROR(null_query).condition);
    dftu_indexer_string_result no_ix = dftu_indexer_manifest(NULL);
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(no_ix));
    dftu_indexer_string_free(NULL);

    dftu_indexer_free(ix);
    free(trace);
}

static void test_drop_then_build_extension(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 25);
    dftu_indexer* ix = open_one(trace);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(dftu_indexer_build(ix)));

    dftu_indexer_status_result nope = dftu_indexer_drop_extension(ix, "nope");
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(nope));
    TEST_ASSERT_EQUAL_INT(DFTU_COND_INVALID_ARGUMENT,
                          DFTU_RESULT_ERROR(nope).condition);
    dftu_indexer_status_result null_name =
        dftu_indexer_drop_extension(ix, NULL);
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(null_name));

    dftu_indexer_status_result dropped =
        dftu_indexer_drop_extension(ix, "counts");
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(dropped));
    TEST_ASSERT_EQUAL_UINT64(1, DFTU_RESULT_VALUE(dropped).needs_work);
    dftu_indexer_string_result m = dftu_indexer_manifest(ix);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(m));
    TEST_ASSERT_NULL(strstr(DFTU_RESULT_VALUE(m), "\"name\":\"counts\""));
    dftu_indexer_string_free(DFTU_RESULT_VALUE(m));

    dftu_indexer_status_result built = dftu_indexer_build(ix);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(built));
    TEST_ASSERT_EQUAL_UINT64(0, DFTU_RESULT_VALUE(built).needs_work);

    dftu_indexer_status_result rebuilt =
        dftu_indexer_rebuild_extension(ix, "bloom");
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(rebuilt));
    TEST_ASSERT_EQUAL_UINT64(1, DFTU_RESULT_VALUE(rebuilt).ready);
    dftu_indexer_free(ix);

    const char* only[] = {"zonemap"};
    dftu_indexer_options opts;
    dftu_indexer_options_init(&opts);
    opts.extensions = only;
    opts.extension_count = 1;
    const char* paths[] = {trace};
    dftu_indexer_open_result o = dftu_indexer_open(paths, 1, &opts);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(o));
    dftu_indexer_free(DFTU_RESULT_VALUE(o));
    const char* bad[] = {"dft.stats"};
    opts.extensions = bad;
    dftu_indexer_open_result b = dftu_indexer_open(paths, 1, &opts);
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(b));
    TEST_ASSERT_EQUAL_INT(DFTU_COND_INVALID_ARGUMENT,
                          DFTU_RESULT_ERROR(b).condition);
    free(trace);
}

static void test_schema_option_and_file_field(void) {
    char* trace = test_environment_create_dft_gzip_file(g_env, 25);
    const char* paths[] = {trace};
    dftu_indexer_options opts;
    dftu_indexer_options_init(&opts);
    TEST_ASSERT_NULL(opts.schema);
    opts.schema = "generic";
    dftu_indexer_open_result o = dftu_indexer_open(paths, 1, &opts);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(o));
    dftu_indexer* ix = DFTU_RESULT_VALUE(o);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(dftu_indexer_build(ix)));
    dftu_indexer_files_result fr = dftu_indexer_files(ix);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(fr));
    dftu_indexer_file_result f =
        dftu_indexer_file_list_get(DFTU_RESULT_VALUE(fr), 0);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(f));
    TEST_ASSERT_EQUAL_STRING("generic", DFTU_RESULT_VALUE(f).schema);
    dftu_indexer_file_list_free(DFTU_RESULT_VALUE(fr));
    dftu_indexer_free(ix);

    opts.schema = "nope";
    dftu_indexer_open_result bad = dftu_indexer_open(paths, 1, &opts);
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(bad));
    TEST_ASSERT_EQUAL_INT(DFTU_COND_INVALID_ARGUMENT,
                          DFTU_RESULT_ERROR(bad).condition);
    free(trace);
}

static void test_schema_registry_calls(void) {
    dftu_indexer_string_result r = dftu_schema_register(
        "id: c_abi_schema\nfields: {status: {type: int}}\n", "c.yaml");
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(r));
    TEST_ASSERT_EQUAL_STRING("c_abi_schema", DFTU_RESULT_VALUE(r));
    dftu_indexer_string_free(DFTU_RESULT_VALUE(r));

    dftu_indexer_string_result again = dftu_schema_register(
        "id: c_abi_schema\nfields: {status: {type: int}}\n", NULL);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(again));
    dftu_indexer_string_free(DFTU_RESULT_VALUE(again));

    dftu_indexer_string_result other = dftu_schema_register(
        "id: c_abi_schema\nfields: {other: {type: int}}\n", "d.yaml");
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(other));
    TEST_ASSERT_EQUAL_INT(DFTU_COND_INVALID_ARGUMENT,
                          DFTU_RESULT_ERROR(other).condition);
    TEST_ASSERT_NOT_NULL(strstr(DFTU_RESULT_ERROR(other).message, "c.yaml"));

    dftu_indexer_string_result list = dftu_schema_list();
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(list));
    TEST_ASSERT_NOT_NULL(strstr(DFTU_RESULT_VALUE(list), "\"c_abi_schema\""));
    TEST_ASSERT_NOT_NULL(strstr(DFTU_RESULT_VALUE(list), "\"dftracer\""));
    dftu_indexer_string_free(DFTU_RESULT_VALUE(list));

    TEST_ASSERT_FALSE(DFTU_RESULT_OK(dftu_schema_register(NULL, NULL)));
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(dftu_schema_load("/no/such/dir")));

    char* trace = test_environment_create_dft_gzip_file(g_env, 25);
    dftu_indexer_string_result d = dftu_schema_detect(trace);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(d));
    TEST_ASSERT_EQUAL_STRING("dftracer", DFTU_RESULT_VALUE(d));
    dftu_indexer_string_free(DFTU_RESULT_VALUE(d));

    dftu_indexer_string_result e = dftu_schema_explain(trace);
    TEST_ASSERT_TRUE(DFTU_RESULT_OK(e));
    TEST_ASSERT_NOT_NULL(
        strstr(DFTU_RESULT_VALUE(e), "\"chosen\":\"dftracer\""));
    TEST_ASSERT_NOT_NULL(strstr(DFTU_RESULT_VALUE(e), "\"c_abi_schema\""));
    dftu_indexer_string_free(DFTU_RESULT_VALUE(e));
    TEST_ASSERT_FALSE(DFTU_RESULT_OK(dftu_schema_explain(NULL)));

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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_open_with_default_options);
    RUN_TEST(test_open_invalid_arguments);
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
    return UNITY_END();
}
