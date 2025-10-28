#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/reader/reader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "testing_utilities.h"

// Global test format - can be set to switch between formats
static test_format_t g_test_format = TEST_FORMAT_GZIP;
static char g_format_name[32] = "GZIP";
static test_environment_handle_t g_env = NULL;
static char* g_test_file = NULL;
static char* g_idx_file = NULL;

// Helper to get test file based on current format
static char* create_format_test_file(test_environment_handle_t env) {
    return test_environment_create_test_file_with_format(env, g_test_format);
}

// Setup function for format-specific environment
static void setup_format_environment(test_format_t format, const char* name) {
    g_test_format = format;
    strcpy(g_format_name, name);

    // Clean up previous environment
    if (g_test_file) {
        free(g_test_file);
        g_test_file = NULL;
    }
    if (g_idx_file) {
        free(g_idx_file);
        g_idx_file = NULL;
    }
    if (g_env) {
        test_environment_destroy(g_env);
        g_env = NULL;
    }

    // Create new environment
    g_env = test_environment_create();
    TEST_ASSERT_NOT_NULL(g_env);
    TEST_ASSERT_TRUE(test_environment_is_valid(g_env));

    g_test_file = create_format_test_file(g_env);
    TEST_ASSERT_NOT_NULL(g_test_file);

    g_idx_file = test_environment_get_index_path(g_env, g_test_file);
    TEST_ASSERT_NOT_NULL(g_idx_file);
}

void setUp(void) {
    // Called before each test
}

void tearDown(void) {
    // Called after each test - clean up any per-test allocations
}

// Generic tests that work for both formats
void test_format_indexer_creation_and_destruction(void) {
    printf("Testing %s indexer creation/destruction\n", g_format_name);

    dft_indexer_handle_t indexer =
        dft_indexer_create(g_test_file, g_idx_file, mb_to_b(1.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    if (indexer) {
        dft_indexer_destroy(indexer);
    }
}

void test_format_index_building(void) {
    printf("Testing %s index building\n", g_format_name);

    dft_indexer_handle_t indexer =
        dft_indexer_create(g_test_file, g_idx_file, mb_to_b(1.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Verify line count is reasonable
    uint64_t line_count = dft_indexer_get_num_lines(indexer);
    TEST_ASSERT_TRUE(line_count > 0);
    printf("  %s format has %lu lines\n", g_format_name,
           (unsigned long)line_count);

    dft_indexer_destroy(indexer);
}

void test_format_reader_creation_and_basic_reading(void) {
    printf("Testing %s reader creation and basic reading\n", g_format_name);

    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_test_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    // Get max bytes
    size_t max_bytes;
    result = dft_reader_get_max_bytes(reader, &max_bytes);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_TRUE(max_bytes > 0);
    printf("  %s format max bytes: %zu\n", g_format_name, max_bytes);

    // Read some data
    const size_t buffer_size = 1024;
    char buffer[1024];
    size_t total_bytes = 0;
    char* output = NULL;

    int bytes_read;
    while ((bytes_read = dft_reader_read(reader, 0, 100, buffer, buffer_size)) >
           0) {
        output = realloc(output, total_bytes + bytes_read);
        TEST_ASSERT_NOT_NULL(output);
        memcpy(output + total_bytes, buffer, bytes_read);
        total_bytes += bytes_read;
    }

    TEST_ASSERT_TRUE(total_bytes > 0);
    TEST_ASSERT_TRUE(total_bytes <= 100);
    printf("  %s format read %zu bytes\n", g_format_name, total_bytes);

    // Basic content validation
    TEST_ASSERT_NOT_NULL(strstr(output, "\"id\": "));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"message\": "));

    free(output);
    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_format_json_boundary_detection(void) {
    printf("Testing %s JSON boundary detection\n", g_format_name);

    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_test_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    char buffer[1024];
    size_t total_bytes = 0;
    char* output = NULL;

    // Read with JSON boundary detection
    int bytes_read;
    while ((bytes_read = dft_reader_read_line_bytes(reader, 0, 150, buffer,
                                                    sizeof(buffer))) > 0) {
        output = realloc(output, total_bytes + bytes_read);
        TEST_ASSERT_NOT_NULL(output);
        memcpy(output + total_bytes, buffer, bytes_read);
        total_bytes += bytes_read;
    }

    if (output && total_bytes > 0) {
        output = realloc(output, total_bytes + 1);
        TEST_ASSERT_NOT_NULL(output);
        output[total_bytes] = '\\0';

        // Should end with complete JSON line
        TEST_ASSERT_EQUAL_CHAR('\\n', output[total_bytes - 1]);

        // Should contain valid JSON
        TEST_ASSERT_NOT_NULL(strstr(output, "\"id\": "));
        TEST_ASSERT_NOT_NULL(strstr(output, "}"));

        printf("  %s format JSON boundary read %zu bytes\n", g_format_name,
               total_bytes);
        free(output);
    }

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

// TAR.GZ specific tests
void test_tar_gz_multiple_files_detection(void) {
    if (g_test_format != TEST_FORMAT_TAR_GZIP) {
        TEST_IGNORE_MESSAGE("TAR.GZ specific test skipped for other formats");
    }

    printf("Testing TAR.GZ multiple files detection\n");

    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_test_file, g_idx_file, mb_to_b(1.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader and read full content
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    size_t max_bytes;
    result = dft_reader_get_max_bytes(reader, &max_bytes);
    TEST_ASSERT_EQUAL_INT(0, result);

    char* full_content = malloc(max_bytes + 1);
    TEST_ASSERT_NOT_NULL(full_content);
    size_t total_read = 0;

    char buffer[4096];
    int bytes_read;
    while ((bytes_read = dft_reader_read(reader, 0, max_bytes, buffer,
                                         sizeof(buffer))) > 0) {
        memcpy(full_content + total_read, buffer, bytes_read);
        total_read += bytes_read;
    }

    full_content[total_read] = '\\0';

    // Should contain data from multiple files
    int has_main = strstr(full_content, "\"file\": \"main\"") != NULL;
    int has_secondary = strstr(full_content, "\"file\": \"secondary\"") != NULL;
    int has_additional =
        strstr(full_content, "\"file\": \"additional\"") != NULL;

    printf("  TAR.GZ contains: main=%s, secondary=%s, additional=%s\n",
           has_main ? "yes" : "no", has_secondary ? "yes" : "no",
           has_additional ? "yes" : "no");

    // Should have at least 2 different file sources
    int file_types_found = has_main + has_secondary + has_additional;
    TEST_ASSERT_TRUE(file_types_found >= 2);

    free(full_content);
    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_tar_gz_directory_structure(void) {
    if (g_test_format != TEST_FORMAT_TAR_GZIP) {
        TEST_IGNORE_MESSAGE("TAR.GZ specific test skipped for other formats");
    }

    printf("Testing TAR.GZ directory structure handling\n");

    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_test_file, g_idx_file, mb_to_b(1.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Should successfully index files in subdirectories
    uint64_t line_count = dft_indexer_get_num_lines(indexer);
    TEST_ASSERT_TRUE(line_count > 0);

    printf("  TAR.GZ with directories indexed %lu lines\n",
           (unsigned long)line_count);

    dft_indexer_destroy(indexer);
}

// GZIP specific tests
void test_gzip_single_file_structure(void) {
    if (g_test_format != TEST_FORMAT_GZIP) {
        TEST_IGNORE_MESSAGE("GZIP specific test skipped for other formats");
    }

    printf("Testing GZIP single file structure\n");

    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_test_file, g_idx_file, mb_to_b(1.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader and read some content
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    char buffer[1024];
    int bytes_read = dft_reader_read(reader, 0, 500, buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(bytes_read > 0);

    // Should NOT contain file field (that's TAR.GZ specific)
    TEST_ASSERT_NULL(strstr(buffer, "\"file\": "));

    // Should contain standard test message format
    TEST_ASSERT_NOT_NULL(strstr(buffer, "Test message"));

    printf("  GZIP single file verified (no file field found)\n");

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

// Macro to run a test with both formats
#define RUN_TEST_WITH_BOTH_FORMATS(test_func)                     \
    do {                                                          \
        setup_format_environment(TEST_FORMAT_GZIP, "GZIP");       \
        RUN_TEST(test_func);                                      \
        setup_format_environment(TEST_FORMAT_TAR_GZIP, "TAR.GZ"); \
        RUN_TEST(test_func);                                      \
    } while (0)

// Macro to run format-specific tests
#define RUN_FORMAT_SPECIFIC_TESTS()                               \
    do {                                                          \
        setup_format_environment(TEST_FORMAT_TAR_GZIP, "TAR.GZ"); \
        RUN_TEST(test_tar_gz_multiple_files_detection);           \
        RUN_TEST(test_tar_gz_directory_structure);                \
        setup_format_environment(TEST_FORMAT_GZIP, "GZIP");       \
        RUN_TEST(test_gzip_single_file_structure);                \
    } while (0)

int main(void) {
    UNITY_BEGIN();

    printf("\\n=== Running Format-Agnostic Tests ===\\n");

    // Run core tests with both formats
    RUN_TEST_WITH_BOTH_FORMATS(test_format_indexer_creation_and_destruction);
    RUN_TEST_WITH_BOTH_FORMATS(test_format_index_building);
    RUN_TEST_WITH_BOTH_FORMATS(test_format_reader_creation_and_basic_reading);
    RUN_TEST_WITH_BOTH_FORMATS(test_format_json_boundary_detection);

    printf("\\n=== Running Format-Specific Tests ===\\n");

    // Run format-specific tests
    RUN_FORMAT_SPECIFIC_TESTS();

    // Clean up
    if (g_test_file) {
        free(g_test_file);
    }
    if (g_idx_file) {
        free(g_idx_file);
    }
    if (g_env) {
        test_environment_destroy(g_env);
    }

    return UNITY_END();
}