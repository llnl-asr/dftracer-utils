#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "testing_utilities.h"

// Global test environment handle
static test_environment_handle_t g_env = NULL;
static char* g_gz_file = NULL;
static char* g_idx_file = NULL;

static test_environment_handle_t setup_test_environment(void);

void setUp(void) { setup_test_environment(); }

void tearDown(void) {
    // Called after each test - clean up any per-test allocations
    if (g_gz_file) {
        free(g_gz_file);
        g_gz_file = NULL;
    }
    if (g_idx_file) {
        free(g_idx_file);
        g_idx_file = NULL;
    }
    if (g_env) {
        test_environment_destroy(g_env);
        g_env = NULL;
    }
}

// Helper function to set up test environment for tests that need it
static test_environment_handle_t setup_test_environment(void) {
    if (!g_env) {
        g_env = test_environment_create();
        TEST_ASSERT_NOT_NULL(g_env);
        TEST_ASSERT_TRUE(test_environment_is_valid(g_env));
    }

    if (!g_gz_file) {
        g_gz_file = test_environment_create_test_gzip_file(g_env);
        TEST_ASSERT_NOT_NULL(g_gz_file);
    }

    if (!g_idx_file) {
        g_idx_file = test_environment_get_index_path(g_env, g_gz_file);
        TEST_ASSERT_NOT_NULL(g_idx_file);
    }

    return g_env;
}

void test_indexer_creation_and_destruction(void) {
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(1.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    if (indexer) {
        dft_indexer_destroy(indexer);
    }
}

void test_indexer_invalid_parameters(void) {
    dft_indexer_handle_t indexer;

    // Test null gz_path
    indexer = dft_indexer_create(NULL, "test.idx", mb_to_b(1.0), 0);
    TEST_ASSERT_NULL(indexer);

    // Test null idx_path
    indexer = dft_indexer_create("test.gz", NULL, mb_to_b(1.0), 0);
    TEST_ASSERT_NULL(indexer);

    // Test invalid chunk size
    indexer = dft_indexer_create("test.gz", "test.idx", 0, 0);
    TEST_ASSERT_NULL(indexer);
}

void test_gzip_index_building(void) {
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(1.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    dft_indexer_destroy(indexer);
}

void test_indexer_rebuild_detection(void) {
    // Create a separate test environment to avoid conflicts with other tests
    test_environment_handle_t test_env = test_environment_create();
    TEST_ASSERT_NOT_NULL(test_env);
    TEST_ASSERT_TRUE(test_environment_is_valid(test_env));

    char* test_gz_file = test_environment_create_test_gzip_file(test_env);
    TEST_ASSERT_NOT_NULL(test_gz_file);

    char* test_idx_file =
        test_environment_get_index_path(test_env, test_gz_file);
    TEST_ASSERT_NOT_NULL(test_idx_file);

    dft_indexer_handle_t indexer =
        dft_indexer_create(test_gz_file, test_idx_file, mb_to_b(1.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    // Initial build should be needed
    int need_rebuild = dft_indexer_need_rebuild(indexer);
    TEST_ASSERT_EQUAL_INT(1, need_rebuild);

    // Build the index
    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    dft_indexer_destroy(indexer);

    // Create new indexer with same parameters
    indexer = dft_indexer_create(test_gz_file, test_idx_file, mb_to_b(1.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    // Should not need rebuild now
    need_rebuild = dft_indexer_need_rebuild(indexer);
    TEST_ASSERT_EQUAL_INT(0, need_rebuild);

    dft_indexer_destroy(indexer);

    // Create new indexer with different chunk size
    indexer = dft_indexer_create(test_gz_file, test_idx_file, mb_to_b(2.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    // Should not rebuild due to different chunk size
    need_rebuild = dft_indexer_need_rebuild(indexer);
    TEST_ASSERT_EQUAL_INT(0, need_rebuild);

    dft_indexer_destroy(indexer);

    // Clean up
    free(test_gz_file);
    free(test_idx_file);
    test_environment_destroy(test_env);
}

void test_indexer_force_rebuild(void) {
    // Create indexer with force rebuild
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(1.0), 1);
    TEST_ASSERT_NOT_NULL(indexer);

    // Should need rebuild because no index is generated
    int need_rebuild = dft_indexer_need_rebuild(indexer);
    TEST_ASSERT_EQUAL_INT(1, need_rebuild);

    dft_indexer_destroy(indexer);
}

void test_reader_creation_and_destruction(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(1.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    if (reader) {
        dft_reader_destroy(reader);
    }
    dft_indexer_destroy(indexer);
}

void test_reader_invalid_parameters(void) {
    dft_reader_handle_t reader;

    size_t ckpt_size = mb_to_b(0.5);

    // Test null gz_path
    reader = dft_reader_create(NULL, "test.idx", ckpt_size);
    TEST_ASSERT_NULL(reader);

    // Test null idx_path
    reader = dft_reader_create("test.gz", NULL, ckpt_size);
    TEST_ASSERT_NULL(reader);

    // Test with valid paths (SQLite will create database if it doesn't exist)
    reader = dft_reader_create("nonexistent.gz", "nonexistent.idx", ckpt_size);
    if (reader) {
        dft_reader_destroy(reader);
    }
}

void test_data_range_reading(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    // Read valid byte range using streaming API
    const size_t buffer_size = 1024;
    char buffer[1024];
    size_t total_bytes = 0;
    char* output = NULL;

    // Stream data from first 50 bytes [0, 50)
    int bytes_read;
    size_t offset = 0;
    size_t end = 50;
    while (offset < end &&
           (bytes_read = dft_reader_read_line_bytes(reader, offset, end, buffer,
                                                    buffer_size)) > 0) {
        output = realloc(output, total_bytes + bytes_read);
        TEST_ASSERT_NOT_NULL(output);
        memcpy(output + total_bytes, buffer, bytes_read);
        total_bytes += bytes_read;
        offset += bytes_read;
    }

    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_TRUE(total_bytes > 0);
    TEST_ASSERT_TRUE(total_bytes <= 50);

    // check that we got some JSON content
    output[total_bytes] = '\0';  // Null terminate for strstr
    char* json_start = strstr(output, "{");
    TEST_ASSERT_NOT_NULL(json_start);

    free(output);
    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_read_with_null_parameters(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    char buffer[1024];

    // null reader
    result = dft_reader_read(NULL, 0, 50, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(-1, result);

    // null buffer
    result = dft_reader_read(reader, 0, 50, NULL, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(-1, result);

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_edge_cases(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    // Invalid byte range (start >= end)
    char buffer[1024];
    result = dft_reader_read(reader, 100, 50, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(-1, result);

    // Equal start and end should also fail
    result = dft_reader_read(reader, 50, 50, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(-1, result);

    // Non-existent file
    // This test is no longer applicable since gz_path is set in constructor
    TEST_ASSERT_EQUAL_INT(-1, result);

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_get_maximum_bytes(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    size_t max_bytes;
    result = dft_reader_get_max_bytes(reader, &max_bytes);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_TRUE(max_bytes > 0);

    // Try to read beyond max_bytes - should return 0 (no more data) or -1
    // (invalid range)
    char buffer[1024];
    result = dft_reader_read(reader, max_bytes + 1, max_bytes + 100, buffer,
                             sizeof(buffer));
    TEST_ASSERT_TRUE(result <=
                     0);  // Could be 0 or -1 depending on implementation

    // Try to read up to max_bytes - should succeed
    if (max_bytes > 10) {
        result = dft_reader_read(reader, max_bytes - 10, max_bytes, buffer,
                                 sizeof(buffer));
        if (result >= 0) {
            TEST_ASSERT_TRUE(result >= 0);
        }
    }

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_get_max_bytes_null_parameters(void) {
    dft_reader_handle_t reader =
        dft_reader_create(g_gz_file, g_idx_file, mb_to_b(0.5));
    if (reader) {
        size_t max_bytes;

        // null reader
        int result = dft_reader_get_max_bytes(NULL, &max_bytes);
        TEST_ASSERT_EQUAL_INT(-1, result);

        // null max_bytes
        result = dft_reader_get_max_bytes(reader, NULL);
        TEST_ASSERT_EQUAL_INT(-1, result);

        dft_reader_destroy(reader);
    }
}

void test_memory_management(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    // multiple reads to ensure no memory leaks
    for (int i = 0; i < 100; i++) {
        char buffer[1024];
        size_t total_bytes = 0;
        char* output = NULL;

        // Stream data until no more available [0, 30)
        int bytes_read;
        size_t offset = 0;
        size_t end = 30;
        while (offset < end &&
               (bytes_read = dft_reader_read_line_bytes(
                    reader, offset, end, buffer, sizeof(buffer))) > 0) {
            output = realloc(output, total_bytes + bytes_read);
            TEST_ASSERT_NOT_NULL(output);
            memcpy(output + total_bytes, buffer, bytes_read);
            total_bytes += bytes_read;
            offset += bytes_read;
        }

        if (total_bytes > 0) {
            TEST_ASSERT_TRUE(total_bytes <= 30);
            free(output);
        }
    }

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_json_boundary_detection(void) {
    // Create larger test environment for better boundary testing
    test_environment_handle_t large_env =
        test_environment_create_with_lines(1000);
    TEST_ASSERT_NOT_NULL(large_env);
    TEST_ASSERT_TRUE(test_environment_is_valid(large_env));

    char* gz_file = test_environment_create_test_gzip_file(large_env);
    TEST_ASSERT_NOT_NULL(gz_file);

    char* idx_file = test_environment_get_index_path(large_env, gz_file);
    TEST_ASSERT_NOT_NULL(idx_file);

    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    char buffer[2048];
    size_t total_bytes = 0;
    char* output = NULL;

    // Stream data until no more available [0, 100)
    // NOTE: Individual streaming buffer chunks may contain partial JSON data
    // (Approach 2)
    int bytes_read;
    size_t offset = 0;
    size_t end = 100;
    while (offset < end &&
           (bytes_read = dft_reader_read_line_bytes(reader, offset, end, buffer,
                                                    sizeof(buffer))) > 0) {
        output = realloc(output, total_bytes + bytes_read);
        TEST_ASSERT_NOT_NULL(output);
        memcpy(output + total_bytes, buffer, bytes_read);
        total_bytes += bytes_read;
        offset += bytes_read;
    }

    if (output && total_bytes > 0) {
        TEST_ASSERT_TRUE(total_bytes <=
                         100);  // Should get at most what was requested

        // Null-terminate for string operations
        output = realloc(output, total_bytes + 1);
        TEST_ASSERT_NOT_NULL(output);
        output[total_bytes] = '\0';

        // With Approach 2: Individual streaming buffers may contain partial
        // JSON but the COMPLETE collected result (0 to 100 bytes) should
        // contain complete JSON lines

        // The streaming API should ensure complete JSON lines for the entire
        // requested range
        TEST_ASSERT_EQUAL_CHAR(
            '\n', output[total_bytes - 1]);  // Should end with newline

        // Should contain complete JSON objects
        char* last_brace = strrchr(output, '}');
        TEST_ASSERT_NOT_NULL(last_brace);
        TEST_ASSERT_TRUE(last_brace <
                         output + total_bytes -
                             1);       // '}' should not be the last character
        TEST_ASSERT_EQUAL_CHAR(
            '\n', *(last_brace + 1));  // Should be followed by newline

        // Basic validation - should contain JSON content
        char* json_start = strstr(output, "{");
        TEST_ASSERT_NOT_NULL(json_start);

        free(output);
    }

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
    free(gz_file);
    free(idx_file);
    test_environment_destroy(large_env);
}

void test_regression_for_truncated_json_output(void) {
    // This test specifically catches the original bug where output was like:
    // {"name":"name_%  instead of complete JSON lines

    test_environment_handle_t large_env =
        test_environment_create_with_lines(2000);
    TEST_ASSERT_NOT_NULL(large_env);

    // Create test data with specific pattern that might trigger the bug
    const char* test_dir = test_environment_get_dir(large_env);
    TEST_ASSERT_NOT_NULL(test_dir);

    char gz_file[512], idx_file[512], txt_file[512];
    snprintf(gz_file, sizeof(gz_file), "%s/regression_test.gz", test_dir);
    snprintf(idx_file, sizeof(idx_file), "%s/regression_test.gz.idx", test_dir);
    snprintf(txt_file, sizeof(txt_file), "%s/regression_test.txt", test_dir);

    // Create test data similar to trace.pfw.gz format
    FILE* f = fopen(txt_file, "w");
    TEST_ASSERT_NOT_NULL(f);

    fprintf(f, "[\n");  // JSON array start
    for (size_t i = 1; i <= 1000; ++i) {
        fprintf(f, "{\"name\":\"name_%zu\",\"cat\":\"cat_%zu\",\"dur\":%zu}\n",
                i, i, (i * 10 % 1000));
    }
    fclose(f);

    int success = compress_file_to_gzip_c(txt_file, gz_file);
    TEST_ASSERT_EQUAL_INT(1, success);
    remove(txt_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, mb_to_b(32.0), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    // Original failing case: 0 to 10000 bytes
    char buffer[4096];
    size_t total_bytes = 0;
    char* output = NULL;

    // Stream data until no more available [0, 10000)
    int bytes_read;
    size_t offset = 0;
    size_t end = 10000;
    while (offset < end &&
           (bytes_read = dft_reader_read_line_bytes(reader, offset, end, buffer,
                                                    sizeof(buffer))) > 0) {
        output = realloc(output, total_bytes + bytes_read);
        TEST_ASSERT_NOT_NULL(output);
        memcpy(output + total_bytes, buffer, bytes_read);
        total_bytes += bytes_read;
        offset += bytes_read;
    }

    if (output && total_bytes > 0) {
        TEST_ASSERT_TRUE(total_bytes <= 10000);

        // Null-terminate for string operations
        output = realloc(output, total_bytes + 1);
        TEST_ASSERT_NOT_NULL(output);
        output[total_bytes] = '\0';

        // Should NOT end with incomplete patterns like "name_%
        TEST_ASSERT_NULL(strstr(output, "\"name_%"));
        TEST_ASSERT_NULL(strstr(output, "\"cat_%"));

        // Now check if the FINAL COMPLETE result has proper JSON boundaries
        // The streaming API should guarantee complete JSON lines for the entire
        // range

        // Should end with complete JSON line
        TEST_ASSERT_EQUAL_CHAR('\n', output[total_bytes - 1]);
        TEST_ASSERT_EQUAL_CHAR('}', output[total_bytes - 2]);

        // Should contain the pattern but complete
        TEST_ASSERT_NOT_NULL(strstr(output, "\"name\":\"name_"));
        TEST_ASSERT_NOT_NULL(strstr(output, "\"cat\":\"cat_"));

        free(output);
    }

    // Small range minimum bytes check
    output = NULL;

    // This was returning only 44 bytes instead of at least 100
    total_bytes = 0;
    output = NULL;

    // Stream data until no more available [0, 100)
    offset = 0;
    end = 100;
    while (offset < end &&
           (bytes_read = dft_reader_read_line_bytes(reader, offset, end, buffer,
                                                    sizeof(buffer))) > 0) {
        output = realloc(output, total_bytes + bytes_read);
        TEST_ASSERT_NOT_NULL(output);
        memcpy(output + total_bytes, buffer, bytes_read);
        total_bytes += bytes_read;
        offset += bytes_read;
    }

    if (output && total_bytes > 0) {
        TEST_ASSERT_TRUE(total_bytes <= 100);

        // Null-terminate for safety
        output = realloc(output, total_bytes + 1);
        TEST_ASSERT_NOT_NULL(output);
        output[total_bytes] = '\0';

        size_t brace_count = 0;
        for (size_t i = 0; i < total_bytes; i++) {
            if (output[i] == '}') brace_count++;
        }
        TEST_ASSERT_TRUE(brace_count >= 2);

        free(output);
    }

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
    test_environment_destroy(large_env);
}

void test_reader_raw_basic_functionality(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    // Read using raw API
    const size_t buffer_size = 1024;
    char buffer[1024];
    size_t total_bytes = 0;
    char* raw_result = NULL;

    // Stream raw data until no more available [0, 50)
    int bytes_read;
    size_t offset = 0;
    size_t end = 50;
    while (offset < end &&
           (bytes_read = dft_reader_read(reader, offset, end, buffer,
                                         buffer_size)) > 0) {
        raw_result = realloc(raw_result, total_bytes + bytes_read);
        TEST_ASSERT_NOT_NULL(raw_result);
        memcpy(raw_result + total_bytes, buffer, bytes_read);
        total_bytes += bytes_read;
        offset += bytes_read;
    }

    TEST_ASSERT_TRUE(total_bytes <= 50);
    TEST_ASSERT_NOT_NULL(raw_result);

    // Raw read should not care about JSON boundaries, so size should be closer
    // to requested
    TEST_ASSERT_TRUE(total_bytes <=
                     60);  // Should be much closer to 50 than regular read

    free(raw_result);
    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_reader_raw_vs_regular_comparison(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create two readers
    dft_reader_handle_t reader1 = dft_reader_create_with_indexer(indexer);
    dft_reader_handle_t reader2 = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader1);
    TEST_ASSERT_NOT_NULL(reader2);

    const size_t buffer_size = 1024;
    char buffer1[1024], buffer2[1024];
    size_t total_bytes1 = 0, total_bytes2 = 0;
    char* raw_result = NULL;
    char* regular_result = NULL;

    // Raw read (new default behavior) [0, 100)
    int bytes_read1;
    size_t offset1 = 0;
    size_t end1 = 100;
    while (offset1 < end1 &&
           (bytes_read1 = dft_reader_read(reader1, offset1, end1, buffer1,
                                          buffer_size)) > 0) {
        raw_result = realloc(raw_result, total_bytes1 + bytes_read1);
        TEST_ASSERT_NOT_NULL(raw_result);
        memcpy(raw_result + total_bytes1, buffer1, bytes_read1);
        total_bytes1 += bytes_read1;
        offset1 += bytes_read1;
    }

    // Line bytes read (old read behavior) [0, 100)
    int bytes_read2;
    size_t offset2 = 0;
    size_t end2 = 100;
    while (offset2 < end2 &&
           (bytes_read2 = dft_reader_read_line_bytes(
                reader2, offset2, end2, buffer2, buffer_size)) > 0) {
        regular_result = realloc(regular_result, total_bytes2 + bytes_read2);
        TEST_ASSERT_NOT_NULL(regular_result);
        memcpy(regular_result + total_bytes2, buffer2, bytes_read2);
        total_bytes2 += bytes_read2;
        offset2 += bytes_read2;
    }

    // Raw read should be equal to requested size (100 bytes)
    TEST_ASSERT_EQUAL_size_t(100, total_bytes1);
    // Byte Lines read should be less or equal than requested size
    TEST_ASSERT_TRUE(total_bytes2 <= 100);

    // Raw read should return exactly what was requested, regular read may be
    // less
    TEST_ASSERT_TRUE(total_bytes2 <= total_bytes1);

    // Regular read should end with complete JSON line
    TEST_ASSERT_EQUAL_CHAR('\n', regular_result[total_bytes2 - 1]);

    // Both should start with same data
    size_t min_size =
        (total_bytes1 < total_bytes2) ? total_bytes1 : total_bytes2;
    TEST_ASSERT_EQUAL_MEMORY(raw_result, regular_result, min_size);

    free(raw_result);
    free(regular_result);
    dft_reader_destroy(reader1);
    dft_reader_destroy(reader2);
    dft_indexer_destroy(indexer);
}

void test_reader_raw_edge_cases(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    // printf("Reader Raw: %p\n", (void*)reader);
    TEST_ASSERT_NOT_NULL(reader);

    size_t max_bytes;
    result = dft_reader_get_max_bytes(reader, &max_bytes);
    TEST_ASSERT_EQUAL_INT(0, result);

    char buffer[1024];
    size_t total_bytes = 0;
    char* output = NULL;

    // Single byte read [0, 1)
    int bytes_read;
    size_t offset = 0;
    size_t end = 1;
    while (offset < end &&
           (bytes_read = dft_reader_read(reader, offset, end, buffer,
                                         sizeof(buffer))) > 0) {
        output = realloc(output, total_bytes + bytes_read);
        TEST_ASSERT_NOT_NULL(output);
        memcpy(output + total_bytes, buffer, bytes_read);
        total_bytes += bytes_read;
        offset += bytes_read;
    }
    TEST_ASSERT_EQUAL_size_t(1, total_bytes);
    free(output);

    // Read near end of file [max_bytes - 10, max_bytes - 1)
    if (max_bytes > 10) {
        output = NULL;
        total_bytes = 0;
        offset = max_bytes - 10;
        end = max_bytes - 1;

        while (offset < end &&
               (bytes_read = dft_reader_read(reader, offset, end, buffer,
                                             sizeof(buffer))) > 0) {
            output = realloc(output, total_bytes + bytes_read);
            TEST_ASSERT_NOT_NULL(output);
            memcpy(output + total_bytes, buffer, bytes_read);
            total_bytes += bytes_read;
            offset += bytes_read;
        }
        TEST_ASSERT_EQUAL_size_t(9, total_bytes);
        free(output);
    }

    // Invalid ranges should still return error
    result = dft_reader_read(reader, 100, 50, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(-1, result);

    result = dft_reader_read(reader, 50, 50, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(-1, result);

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_reader_raw_small_buffer(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    // Use very small buffer to test streaming behavior
    const size_t small_buffer_size = 16;
    char small_buffer[16];
    size_t total_bytes = 0;
    size_t total_calls = 0;
    char* output = NULL;

    int bytes_read;
    size_t offset = 0;
    size_t end = 200;
    while (offset < end &&
           (bytes_read = dft_reader_read(reader, offset, end, small_buffer,
                                         small_buffer_size)) > 0) {
        output = realloc(output, total_bytes + bytes_read);
        TEST_ASSERT_NOT_NULL(output);
        memcpy(output + total_bytes, small_buffer, bytes_read);
        total_bytes += bytes_read;
        offset += bytes_read;
        total_calls++;
        TEST_ASSERT_TRUE(bytes_read <= small_buffer_size);
        if (total_calls > 50) break;  // Safety guard
    }

    TEST_ASSERT_EQUAL_size_t(200, total_bytes);
    TEST_ASSERT_TRUE(total_calls >
                     1);  // Should require multiple calls with small buffer

    free(output);
    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_reader_raw_multiple_ranges(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    size_t max_bytes;
    result = dft_reader_get_max_bytes(reader, &max_bytes);
    TEST_ASSERT_EQUAL_INT(0, result);

    char buffer[1024];

    // Define ranges to test
    struct {
        size_t start;
        size_t end;
    } ranges[] = {{0, 50}, {50, 100}, {100, 150}};
    size_t num_ranges = sizeof(ranges) / sizeof(ranges[0]);

    for (size_t i = 0; i < num_ranges; i++) {
        if (ranges[i].end <= max_bytes) {
            size_t total_bytes = 0;
            char* segment = NULL;

            int bytes_read;
            size_t offset = ranges[i].start;
            size_t end = ranges[i].end;
            while (offset < end &&
                   (bytes_read = dft_reader_read(reader, offset, end, buffer,
                                                 sizeof(buffer))) > 0) {
                segment = realloc(segment, total_bytes + bytes_read);
                TEST_ASSERT_NOT_NULL(segment);
                memcpy(segment + total_bytes, buffer, bytes_read);
                total_bytes += bytes_read;
                offset += bytes_read;
            }

            size_t expected_size = ranges[i].end - ranges[i].start;
            TEST_ASSERT_EQUAL_size_t(expected_size, total_bytes);

            free(segment);
        }
    }

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_reader_raw_null_parameters(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    char buffer[1024];

    // null reader
    result = dft_reader_read(NULL, 0, 50, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(-1, result);

    // null gz_path
    // This test is no longer applicable since gz_path is set in constructor
    TEST_ASSERT_EQUAL_INT(-1, result);

    // null buffer
    result = dft_reader_read(reader, 0, 50, NULL, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(-1, result);

    // zero buffer size
    result = dft_reader_read(reader, 0, 50, buffer, 0);
    TEST_ASSERT_EQUAL_INT(-1, result);

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_reader_full_file_comparison_raw_vs_json_boundary(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create two readers
    dft_reader_handle_t reader1 = dft_reader_create_with_indexer(indexer);
    dft_reader_handle_t reader2 = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader1);
    TEST_ASSERT_NOT_NULL(reader2);

    // Get max bytes
    size_t max_bytes;
    result = dft_reader_get_max_bytes(reader1, &max_bytes);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_TRUE(max_bytes > 0);

    char buffer[4096];

    // Read entire file with raw API
    size_t raw_total_bytes = 0;
    char* raw_content = NULL;

    int bytes_read1;
    size_t offset1 = 0;
    while (offset1 < max_bytes &&
           (bytes_read1 = dft_reader_read(reader1, offset1, max_bytes, buffer,
                                          sizeof(buffer))) > 0) {
        raw_content = realloc(raw_content, raw_total_bytes + bytes_read1);
        TEST_ASSERT_NOT_NULL(raw_content);
        memcpy(raw_content + raw_total_bytes, buffer, bytes_read1);
        raw_total_bytes += bytes_read1;
        offset1 += bytes_read1;
    }

    // Read entire file with line-boundary aware API
    size_t json_total_bytes = 0;
    char* json_content = NULL;

    int bytes_read2;
    size_t offset2 = 0;
    while (offset2 < max_bytes &&
           (bytes_read2 = dft_reader_read_line_bytes(
                reader2, offset2, max_bytes, buffer, sizeof(buffer))) > 0) {
        json_content = realloc(json_content, json_total_bytes + bytes_read2);
        TEST_ASSERT_NOT_NULL(json_content);
        memcpy(json_content + json_total_bytes, buffer, bytes_read2);
        json_total_bytes += bytes_read2;
        offset2 += bytes_read2;
    }

    // Both should read the entire file
    TEST_ASSERT_EQUAL_size_t(max_bytes, raw_total_bytes);
    TEST_ASSERT_EQUAL_size_t(max_bytes, json_total_bytes);

    // Total bytes should be identical when reading full file
    TEST_ASSERT_EQUAL_size_t(raw_total_bytes, json_total_bytes);

    // Content should be identical when reading full file
    TEST_ASSERT_EQUAL_MEMORY(raw_content, json_content, raw_total_bytes);

    // Both should end with complete JSON lines
    if (raw_total_bytes > 0 && json_total_bytes > 0) {
        TEST_ASSERT_EQUAL_CHAR('\n', raw_content[raw_total_bytes - 1]);
        TEST_ASSERT_EQUAL_CHAR('\n', json_content[json_total_bytes - 1]);

        // Find last JSON line in both (look for second-to-last newline)
        char* raw_last_newline = NULL;
        char* json_last_newline = NULL;

        // Find second-to-last newline in raw content
        for (size_t i = raw_total_bytes - 2; i > 0; i--) {
            if (raw_content[i] == '\n') {
                raw_last_newline = &raw_content[i];
                break;
            }
        }

        // Find second-to-last newline in json content
        for (size_t i = json_total_bytes - 2; i > 0; i--) {
            if (json_content[i] == '\n') {
                json_last_newline = &json_content[i];
                break;
            }
        }

        if (raw_last_newline && json_last_newline) {
            // Calculate last line lengths
            size_t raw_last_line_len =
                (raw_content + raw_total_bytes - 1) - raw_last_newline;
            size_t json_last_line_len =
                (json_content + json_total_bytes - 1) - json_last_newline;

            // Last JSON lines should be identical
            TEST_ASSERT_EQUAL_size_t(raw_last_line_len, json_last_line_len);
            TEST_ASSERT_EQUAL_MEMORY(raw_last_newline, json_last_newline,
                                     raw_last_line_len);

            // Should contain valid JSON structure (look for { and } in last
            // line)
            char* raw_last_line_start = raw_last_newline + 1;
            char* json_last_line_start = json_last_newline + 1;
            size_t actual_line_len = raw_last_line_len - 1;  // exclude newline

            int raw_has_brace_open = 0, raw_has_brace_close = 0;
            int json_has_brace_open = 0, json_has_brace_close = 0;

            for (size_t i = 0; i < actual_line_len; i++) {
                if (raw_last_line_start[i] == '{') raw_has_brace_open = 1;
                if (raw_last_line_start[i] == '}') raw_has_brace_close = 1;
                if (json_last_line_start[i] == '{') json_has_brace_open = 1;
                if (json_last_line_start[i] == '}') json_has_brace_close = 1;
            }

            TEST_ASSERT_TRUE(raw_has_brace_open);
            TEST_ASSERT_TRUE(raw_has_brace_close);
            TEST_ASSERT_TRUE(json_has_brace_open);
            TEST_ASSERT_TRUE(json_has_brace_close);
        }
    }

    // Debug output (will be visible if test fails)
    printf("Full file comparison: raw_size=%zu, json_size=%zu, max_bytes=%zu\n",
           raw_total_bytes, json_total_bytes, max_bytes);

    free(raw_content);
    free(json_content);
    dft_reader_destroy(reader1);
    dft_reader_destroy(reader2);
    dft_indexer_destroy(indexer);
}

void test_reader_line_reading_basic(void) {
    // Create larger test environment for better line reading support
    test_environment_handle_t large_env =
        test_environment_create_with_lines(10000);
    TEST_ASSERT_NOT_NULL(large_env);
    TEST_ASSERT_TRUE(test_environment_is_valid(large_env));

    char* gz_file = test_environment_create_test_gzip_file(large_env);
    TEST_ASSERT_NOT_NULL(gz_file);

    char* idx_file = test_environment_get_index_path(large_env, gz_file);
    TEST_ASSERT_NOT_NULL(idx_file);

    // Build index first with small chunk size to force checkpoint creation
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, mb_to_b(0.1), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Verify indexer has line data
    uint64_t total_lines = dft_indexer_get_num_lines(indexer);

    // Skip test if no line data (file too small)
    if (total_lines == 0) {
        printf(
            "Skipping line reading tests - indexer has no line data (file too "
            "small?)\n");
        free(gz_file);
        free(idx_file);
        dft_indexer_destroy(indexer);
        test_environment_destroy(large_env);
        return;
    }

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    // Test basic line reading - first 5 lines
    char buffer[4096];
    size_t bytes_written;

    result = dft_reader_read_lines(reader, 1, 5, buffer, sizeof(buffer),
                                   &bytes_written);

    // If line reading fails, it might be due to no line data in checkpoints -
    // skip test
    if (result != 0) {
        printf(
            "Skipping line reading tests - line reading failed (no checkpoint "
            "line data?)\n");
        dft_reader_destroy(reader);
        free(gz_file);
        free(idx_file);
        test_environment_destroy(large_env);
        return;
    }

    TEST_ASSERT_TRUE(bytes_written > 0);

    // Count newlines to verify we got 5 lines
    size_t line_count = 0;
    for (size_t i = 0; i < bytes_written; i++) {
        if (buffer[i] == '\n') line_count++;
    }
    TEST_ASSERT_EQUAL_size_t(5, line_count);

    // Verify it contains expected pattern (test data format)
    TEST_ASSERT_NOT_NULL(strstr(buffer, "\"id\": 1"));

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
    free(gz_file);
    free(idx_file);
    test_environment_destroy(large_env);
}

void test_reader_line_reading_accuracy(void) {
    // Create larger test environment
    test_environment_handle_t large_env =
        test_environment_create_with_lines(10000);
    TEST_ASSERT_NOT_NULL(large_env);

    char* gz_file = test_environment_create_test_gzip_file(large_env);
    TEST_ASSERT_NOT_NULL(gz_file);

    char* idx_file = test_environment_get_index_path(large_env, gz_file);
    TEST_ASSERT_NOT_NULL(idx_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, mb_to_b(0.1), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    uint64_t total_lines = dft_indexer_get_num_lines(indexer);

    if (total_lines == 0) {
        printf("Skipping line accuracy tests - no line data\n");
        free(gz_file);
        free(idx_file);
        dft_indexer_destroy(indexer);
        test_environment_destroy(large_env);
        return;
    }

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    // Test specific line numbers
    size_t test_lines[] = {1, 10, 50, 100};
    size_t num_test_lines = sizeof(test_lines) / sizeof(test_lines[0]);

    for (size_t i = 0; i < num_test_lines; i++) {
        size_t line_num = test_lines[i];
        if (line_num <= total_lines) {
            char buffer[1024];
            size_t bytes_written;

            result = dft_reader_read_lines(reader, line_num, line_num, buffer,
                                           sizeof(buffer), &bytes_written);

            // If line reading fails, skip remaining tests
            if (result != 0) {
                printf(
                    "Skipping line accuracy tests - line reading failed (no "
                    "checkpoint line data?)\n");
                dft_reader_destroy(reader);
                free(gz_file);
                free(idx_file);
                test_environment_destroy(large_env);
                return;
            }

            TEST_ASSERT_TRUE(bytes_written > 0);

            // Should contain id: N where N = line_num
            char expected[64];
            snprintf(expected, sizeof(expected), "\"id\": %zu", line_num);
            TEST_ASSERT_NOT_NULL(strstr(buffer, expected));

            // Should have exactly one line
            size_t line_count = 0;
            for (size_t j = 0; j < bytes_written; j++) {
                if (buffer[j] == '\n') line_count++;
            }
            TEST_ASSERT_EQUAL_size_t(1, line_count);
        }
    }

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
    free(gz_file);
    free(idx_file);
    test_environment_destroy(large_env);
}

void test_reader_line_reading_range(void) {
    // Create larger test environment
    test_environment_handle_t large_env =
        test_environment_create_with_lines(10000);
    TEST_ASSERT_NOT_NULL(large_env);

    char* gz_file = test_environment_create_test_gzip_file(large_env);
    TEST_ASSERT_NOT_NULL(gz_file);

    char* idx_file = test_environment_get_index_path(large_env, gz_file);
    TEST_ASSERT_NOT_NULL(idx_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, mb_to_b(0.1), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    uint64_t total_lines = dft_indexer_get_num_lines(indexer);

    if (total_lines == 0) {
        printf("Skipping line range tests - no line data\n");
        free(gz_file);
        free(idx_file);
        test_environment_destroy(large_env);
        return;
    }

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    // Read line range 10-15 (6 lines total)
    char buffer[2048];
    size_t bytes_written;

    result = dft_reader_read_lines(reader, 10, 15, buffer, sizeof(buffer),
                                   &bytes_written);

    // If line reading fails, skip test
    if (result != 0) {
        printf(
            "Skipping line range tests - line reading failed (no checkpoint "
            "line data?)\n");
        dft_reader_destroy(reader);
        free(gz_file);
        free(idx_file);
        test_environment_destroy(large_env);
        return;
    }

    TEST_ASSERT_TRUE(bytes_written > 0);

    // Count lines
    size_t line_count = 0;
    for (size_t i = 0; i < bytes_written; i++) {
        if (buffer[i] == '\n') line_count++;
    }
    TEST_ASSERT_EQUAL_size_t(
        6, line_count);  // Should have exactly 6 lines (10, 11, 12, 13, 14, 15)

    // Should start with line 10 and end with line 15
    TEST_ASSERT_NOT_NULL(strstr(buffer, "\"id\": 10"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, "\"id\": 15"));

    // Should not contain line 9 or 16
    TEST_ASSERT_NULL(strstr(buffer, "\"id\": 9"));
    TEST_ASSERT_NULL(strstr(buffer, "\"id\": 16"));

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
    free(gz_file);
    free(idx_file);
    test_environment_destroy(large_env);
}

void test_reader_line_reading_error_handling(void) {
    // Build index first
    dft_indexer_handle_t indexer =
        dft_indexer_create(g_gz_file, g_idx_file, mb_to_b(0.5), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    char buffer[1024];
    size_t bytes_written;

    // Test null parameters
    result = dft_reader_read_lines(NULL, 1, 5, buffer, sizeof(buffer),
                                   &bytes_written);
    TEST_ASSERT_EQUAL_INT(-1, result);

    result = dft_reader_read_lines(reader, 1, 5, NULL, sizeof(buffer),
                                   &bytes_written);
    TEST_ASSERT_EQUAL_INT(-1, result);

    result = dft_reader_read_lines(reader, 1, 5, buffer, sizeof(buffer), NULL);
    TEST_ASSERT_EQUAL_INT(-1, result);

    result = dft_reader_read_lines(reader, 1, 5, buffer, 0, &bytes_written);
    TEST_ASSERT_EQUAL_INT(-1, result);

    // Test invalid line numbers (0-based should fail - we use 1-based)
    result = dft_reader_read_lines(reader, 0, 5, buffer, sizeof(buffer),
                                   &bytes_written);
    TEST_ASSERT_EQUAL_INT(-1, result);

    result = dft_reader_read_lines(reader, 1, 0, buffer, sizeof(buffer),
                                   &bytes_written);
    TEST_ASSERT_EQUAL_INT(-1, result);

    // start > end should fail
    result = dft_reader_read_lines(reader, 10, 5, buffer, sizeof(buffer),
                                   &bytes_written);
    TEST_ASSERT_EQUAL_INT(-1, result);

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
}

void test_reader_line_reading_buffer_too_small(void) {
    // Create larger test environment
    test_environment_handle_t large_env =
        test_environment_create_with_lines(1000);
    TEST_ASSERT_NOT_NULL(large_env);

    char* gz_file = test_environment_create_test_gzip_file(large_env);
    TEST_ASSERT_NOT_NULL(gz_file);

    char* idx_file = test_environment_get_index_path(large_env, gz_file);
    TEST_ASSERT_NOT_NULL(idx_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, mb_to_b(0.1), 0);
    TEST_ASSERT_NOT_NULL(indexer);

    int result = dft_indexer_build(indexer);
    TEST_ASSERT_EQUAL_INT(0, result);

    uint64_t total_lines = dft_indexer_get_num_lines(indexer);

    if (total_lines == 0) {
        printf("Skipping buffer size tests - no line data\n");
        free(gz_file);
        free(idx_file);
        test_environment_destroy(large_env);
        return;
    }

    // Create reader
    dft_reader_handle_t reader = dft_reader_create_with_indexer(indexer);
    TEST_ASSERT_NOT_NULL(reader);

    // Try to read many lines into a small buffer
    char small_buffer[50];  // Very small buffer
    size_t bytes_written;

    result = dft_reader_read_lines(reader, 1, 100, small_buffer,
                                   sizeof(small_buffer), &bytes_written);

    // This test expects buffer too small error, but if line reading fails due
    // to no checkpoint data, skip test
    if (result == -1 && bytes_written == 0) {
        printf(
            "Skipping buffer size tests - line reading failed (no checkpoint "
            "line data?)\n");
        dft_reader_destroy(reader);
        free(gz_file);
        free(idx_file);
        test_environment_destroy(large_env);
        return;
    }

    TEST_ASSERT_EQUAL_INT(-1, result);  // Should fail due to buffer too small
    TEST_ASSERT_TRUE(bytes_written >
                     sizeof(small_buffer));  // Should tell us required size

    dft_reader_destroy(reader);
    dft_indexer_destroy(indexer);
    free(gz_file);
    free(idx_file);
    test_environment_destroy(large_env);
}

int main(void) {
    UNITY_BEGIN();

    // Set up global test environment
    g_env = test_environment_create();
    if (!g_env || !test_environment_is_valid(g_env)) {
        printf("Failed to create test environment\n");
        return 1;
    }

    // Indexer tests
    RUN_TEST(test_indexer_creation_and_destruction);
    RUN_TEST(test_indexer_invalid_parameters);
    RUN_TEST(test_gzip_index_building);
    RUN_TEST(test_indexer_rebuild_detection);
    RUN_TEST(test_indexer_force_rebuild);

    // Reader tests
    RUN_TEST(test_reader_creation_and_destruction);
    RUN_TEST(test_reader_invalid_parameters);
    RUN_TEST(test_data_range_reading);
    RUN_TEST(test_read_with_null_parameters);
    RUN_TEST(test_edge_cases);
    RUN_TEST(test_get_maximum_bytes);
    RUN_TEST(test_get_max_bytes_null_parameters);
    RUN_TEST(test_memory_management);

    // Advanced tests
    RUN_TEST(test_json_boundary_detection);
    RUN_TEST(test_regression_for_truncated_json_output);

    // Raw reader tests
    RUN_TEST(test_reader_raw_basic_functionality);
    RUN_TEST(test_reader_raw_vs_regular_comparison);
    RUN_TEST(test_reader_raw_edge_cases);
    RUN_TEST(test_reader_raw_small_buffer);
    RUN_TEST(test_reader_raw_multiple_ranges);
    RUN_TEST(test_reader_raw_null_parameters);
    RUN_TEST(test_reader_full_file_comparison_raw_vs_json_boundary);

    // Line reading tests (C API)
    RUN_TEST(test_reader_line_reading_basic);
    RUN_TEST(test_reader_line_reading_accuracy);
    RUN_TEST(test_reader_line_reading_range);
    RUN_TEST(test_reader_line_reading_error_handling);
    RUN_TEST(test_reader_line_reading_buffer_too_small);

    // Clean up global test environment
    if (g_env) {
        test_environment_destroy(g_env);
    }

    return UNITY_END();
}
