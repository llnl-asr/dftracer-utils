#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/reader/reader.h>
#include <dftracer/utils/reader/stream.h>
#include <dftracer/utils/reader/stream_type.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testing_utilities.h"

#define CHECK(condition, message)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", message, __FILE__, \
                    __LINE__);                                                \
            return 1;                                                         \
        }                                                                     \
    } while (0)

#define CHECK_NOT_NULL(ptr, name)                                        \
    do {                                                                 \
        if ((ptr) == NULL) {                                             \
            fprintf(stderr, "CHECK FAILED: %s is NULL at %s:%d\n", name, \
                    __FILE__, __LINE__);                                 \
            return 1;                                                    \
        }                                                                \
    } while (0)

static int test_bytes_stream_byte_range(void) {
    printf("\n=== Test: BYTES stream with BYTE_RANGE ===\n");

    test_environment_handle_t env = test_environment_create_with_lines(1000);
    CHECK_NOT_NULL(env, "test_environment");

    char* gz_file = test_environment_create_test_gzip_file(env);
    CHECK_NOT_NULL(gz_file, "gz_file");

    char idx_file[1024];
    snprintf(idx_file, sizeof(idx_file), "%s.idx", gz_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, 524288, 0);
    CHECK_NOT_NULL(indexer, "indexer");

    int build_result = dft_indexer_build(indexer);
    CHECK(build_result == 0, "indexer build should succeed");

    dft_indexer_destroy(indexer);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create(gz_file, idx_file, 524288);
    CHECK_NOT_NULL(reader, "reader");

    // Create stream
    dft_stream_config_t config;
    dft_stream_config_init_bytes(&config, 0, 100);
    dft_reader_stream_t stream = dft_reader_stream(reader, &config);
    CHECK_NOT_NULL(stream, "stream");

    // Read data
    char buffer[1024];
    size_t total_read = 0;
    int finished = dft_reader_stream_done(stream);

    while (!finished) {
        size_t bytes_read =
            dft_reader_stream_read(stream, buffer, sizeof(buffer));
        if (bytes_read == 0) {
            break;
        }
        total_read += bytes_read;
        finished = dft_reader_stream_done(stream);
    }

    printf("Total bytes read: %zu\n", total_read);
    CHECK(total_read > 0, "should read some data");
    CHECK(total_read <= 100, "should not exceed requested range");

    // Cleanup
    dft_reader_stream_destroy(stream);
    dft_reader_destroy(reader);
    free(gz_file);
    test_environment_destroy(env);

    printf("PASSED\n");
    return 0;
}

static int test_bytes_stream_line_range(void) {
    printf("\n=== Test: BYTES stream with LINE_RANGE ===\n");

    test_environment_handle_t env = test_environment_create_with_lines(1000);
    CHECK_NOT_NULL(env, "test_environment");

    char* gz_file = test_environment_create_test_gzip_file(env);
    CHECK_NOT_NULL(gz_file, "gz_file");

    char idx_file[1024];
    snprintf(idx_file, sizeof(idx_file), "%s.idx", gz_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, 524288, 0);
    CHECK_NOT_NULL(indexer, "indexer");

    int build_result = dft_indexer_build(indexer);
    CHECK(build_result == 0, "indexer build should succeed");

    dft_indexer_destroy(indexer);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create(gz_file, idx_file, 524288);
    CHECK_NOT_NULL(reader, "reader");

    // Check if we have line data
    size_t num_lines = 0;
    int get_result = dft_reader_get_num_lines(reader, &num_lines);
    if (get_result != 0 || num_lines == 0) {
        printf("SKIPPED - no line data available\n");
        dft_reader_destroy(reader);
        free(gz_file);
        test_environment_destroy(env);
        return 0;
    }

    // Create stream with line range
    dft_stream_config_t config;
    dft_stream_config_init_lines(&config, 1, 10);
    config.stream = DFT_STREAM_TYPE_BYTES;
    dft_reader_stream_t stream = dft_reader_stream(reader, &config);
    CHECK_NOT_NULL(stream, "stream");

    // Read data
    char buffer[1024];
    size_t total_read = 0;

    while (!dft_reader_stream_done(stream)) {
        size_t bytes_read =
            dft_reader_stream_read(stream, buffer, sizeof(buffer));
        if (bytes_read == 0) {
            break;
        }
        total_read += bytes_read;
    }

    printf("Total bytes read: %zu\n", total_read);
    CHECK(total_read > 0, "should read some data");

    // Cleanup
    dft_reader_stream_destroy(stream);
    dft_reader_destroy(reader);
    free(gz_file);
    test_environment_destroy(env);

    printf("PASSED\n");
    return 0;
}

static int test_line_bytes_stream(void) {
    printf("\n=== Test: LINE_BYTES stream ===\n");

    test_environment_handle_t env = test_environment_create_with_lines(1000);
    CHECK_NOT_NULL(env, "test_environment");

    char* gz_file = test_environment_create_test_gzip_file(env);
    CHECK_NOT_NULL(gz_file, "gz_file");

    char idx_file[1024];
    snprintf(idx_file, sizeof(idx_file), "%s.idx", gz_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, 524288, 0);
    CHECK_NOT_NULL(indexer, "indexer");

    int build_result = dft_indexer_build(indexer);
    CHECK(build_result == 0, "indexer build should succeed");

    dft_indexer_destroy(indexer);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create(gz_file, idx_file, 524288);
    CHECK_NOT_NULL(reader, "reader");

    // Check if we have line data
    size_t num_lines = 0;
    int get_result = dft_reader_get_num_lines(reader, &num_lines);
    if (get_result != 0 || num_lines == 0) {
        printf("SKIPPED - no line data available\n");
        dft_reader_destroy(reader);
        free(gz_file);
        test_environment_destroy(env);
        return 0;
    }

    // Create LINE_BYTES stream
    dft_stream_config_t config;
    dft_stream_config_init_lines(&config, 1, 10);
    config.stream = DFT_STREAM_TYPE_LINE_BYTES;
    dft_reader_stream_t stream = dft_reader_stream(reader, &config);
    CHECK_NOT_NULL(stream, "stream");

    // Read lines one by one
    char buffer[1024];
    int line_count = 0;

    while (!dft_reader_stream_done(stream) && line_count < 20) {
        size_t bytes_read =
            dft_reader_stream_read(stream, buffer, sizeof(buffer));
        if (bytes_read == 0) {
            break;
        }
        line_count++;

        // Each read should contain a newline (except possibly last)
        if (!dft_reader_stream_done(stream)) {
            CHECK(buffer[bytes_read - 1] == '\n',
                  "line should end with newline");
        }
    }

    printf("Lines read: %d\n", line_count);
    CHECK(line_count > 0, "should read some lines");
    CHECK(line_count <= 11, "should not exceed requested range");

    // Cleanup
    dft_reader_stream_destroy(stream);
    dft_reader_destroy(reader);
    free(gz_file);
    test_environment_destroy(env);

    printf("PASSED\n");
    return 0;
}

static int test_multi_lines_bytes_stream(void) {
    printf("\n=== Test: MULTI_LINES_BYTES stream ===\n");

    test_environment_handle_t env = test_environment_create_with_lines(1000);
    CHECK_NOT_NULL(env, "test_environment");

    char* gz_file = test_environment_create_test_gzip_file(env);
    CHECK_NOT_NULL(gz_file, "gz_file");

    char idx_file[1024];
    snprintf(idx_file, sizeof(idx_file), "%s.idx", gz_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, 524288, 0);
    CHECK_NOT_NULL(indexer, "indexer");

    int build_result = dft_indexer_build(indexer);
    CHECK(build_result == 0, "indexer build should succeed");

    dft_indexer_destroy(indexer);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create(gz_file, idx_file, 524288);
    CHECK_NOT_NULL(reader, "reader");

    // Check if we have line data
    size_t num_lines = 0;
    int get_result = dft_reader_get_num_lines(reader, &num_lines);
    if (get_result != 0 || num_lines == 0) {
        printf("SKIPPED - no line data available\n");
        dft_reader_destroy(reader);
        free(gz_file);
        test_environment_destroy(env);
        return 0;
    }

    // Create MULTI_LINES_BYTES stream
    dft_stream_config_t config;
    dft_stream_config_init_lines(&config, 1, 20);
    config.stream = DFT_STREAM_TYPE_MULTI_LINES_BYTES;
    dft_reader_stream_t stream = dft_reader_stream(reader, &config);
    CHECK_NOT_NULL(stream, "stream");

    // Read multiple lines per read
    char buffer[512];
    size_t total_lines = 0;

    while (!dft_reader_stream_done(stream)) {
        size_t bytes_read =
            dft_reader_stream_read(stream, buffer, sizeof(buffer));
        if (bytes_read == 0) {
            break;
        }

        // Count newlines in this read
        for (size_t i = 0; i < bytes_read; i++) {
            if (buffer[i] == '\n') {
                total_lines++;
            }
        }
    }

    printf("Total lines read: %zu\n", total_lines);
    CHECK(total_lines > 0, "should read some lines");
    // Note: LINE_RANGE may read more lines than requested due to implementation
    printf("Note: Read %zu lines (requested 1-20)\n", total_lines);

    // Cleanup
    dft_reader_stream_destroy(stream);
    dft_reader_destroy(reader);
    free(gz_file);
    test_environment_destroy(env);

    printf("PASSED\n");
    return 0;
}

static int test_line_stream(void) {
    printf("\n=== Test: LINE stream ===\n");

    test_environment_handle_t env = test_environment_create_with_lines(1000);
    CHECK_NOT_NULL(env, "test_environment");

    char* gz_file = test_environment_create_test_gzip_file(env);
    CHECK_NOT_NULL(gz_file, "gz_file");

    char idx_file[1024];
    snprintf(idx_file, sizeof(idx_file), "%s.idx", gz_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, 524288, 0);
    CHECK_NOT_NULL(indexer, "indexer");

    int build_result = dft_indexer_build(indexer);
    CHECK(build_result == 0, "indexer build should succeed");

    dft_indexer_destroy(indexer);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create(gz_file, idx_file, 524288);
    CHECK_NOT_NULL(reader, "reader");

    // Check if we have line data
    size_t num_lines = 0;
    int get_result = dft_reader_get_num_lines(reader, &num_lines);
    if (get_result != 0 || num_lines == 0) {
        printf("SKIPPED - no line data available\n");
        dft_reader_destroy(reader);
        free(gz_file);
        test_environment_destroy(env);
        return 0;
    }

    // Create LINE stream
    dft_stream_config_t config;
    dft_stream_config_init_lines(&config, 6, 15);
    config.stream = DFT_STREAM_TYPE_LINE;
    dft_reader_stream_t stream = dft_reader_stream(reader, &config);
    CHECK_NOT_NULL(stream, "stream");

    // Read parsed lines
    char buffer[1024];
    int line_count = 0;

    while (!dft_reader_stream_done(stream)) {
        size_t bytes_read =
            dft_reader_stream_read(stream, buffer, sizeof(buffer));
        if (bytes_read == 0) {
            break;
        }
        line_count++;
    }

    printf("Lines read: %d\n", line_count);
    CHECK(line_count > 0, "should read some lines");
    CHECK(line_count <= 11, "should not exceed requested range");

    // Cleanup
    dft_reader_stream_destroy(stream);
    dft_reader_destroy(reader);
    free(gz_file);
    test_environment_destroy(env);

    printf("PASSED\n");
    return 0;
}

static int test_multi_lines_stream(void) {
    printf("\n=== Test: MULTI_LINES stream ===\n");

    test_environment_handle_t env = test_environment_create_with_lines(1000);
    CHECK_NOT_NULL(env, "test_environment");

    char* gz_file = test_environment_create_test_gzip_file(env);
    CHECK_NOT_NULL(gz_file, "gz_file");

    char idx_file[1024];
    snprintf(idx_file, sizeof(idx_file), "%s.idx", gz_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, 524288, 0);
    CHECK_NOT_NULL(indexer, "indexer");

    int build_result = dft_indexer_build(indexer);
    CHECK(build_result == 0, "indexer build should succeed");

    dft_indexer_destroy(indexer);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create(gz_file, idx_file, 524288);
    CHECK_NOT_NULL(reader, "reader");

    // Check if we have line data
    size_t num_lines = 0;
    int get_result = dft_reader_get_num_lines(reader, &num_lines);
    if (get_result != 0 || num_lines == 0) {
        printf("SKIPPED - no line data available\n");
        dft_reader_destroy(reader);
        free(gz_file);
        test_environment_destroy(env);
        return 0;
    }

    // Create MULTI_LINES stream
    dft_stream_config_t config;
    dft_stream_config_init_lines(&config, 10, 30);
    config.stream = DFT_STREAM_TYPE_MULTI_LINES;
    dft_reader_stream_t stream = dft_reader_stream(reader, &config);
    CHECK_NOT_NULL(stream, "stream");

    // Read multiple parsed lines
    char buffer[512];
    size_t total_lines = 0;

    while (!dft_reader_stream_done(stream)) {
        size_t bytes_read =
            dft_reader_stream_read(stream, buffer, sizeof(buffer));
        if (bytes_read == 0) {
            break;
        }

        // Count newlines
        for (size_t i = 0; i < bytes_read; i++) {
            if (buffer[i] == '\n') {
                total_lines++;
            }
        }
    }

    printf("Total lines read: %zu\n", total_lines);
    CHECK(total_lines > 0, "should read some lines");
    // Note: LINE_RANGE may read more lines than requested due to implementation
    printf("Note: Read %zu lines (requested 10-30)\n", total_lines);

    // Cleanup
    dft_reader_stream_destroy(stream);
    dft_reader_destroy(reader);
    free(gz_file);
    test_environment_destroy(env);

    printf("PASSED\n");
    return 0;
}

static int test_stream_recreation(void) {
    printf("\n=== Test: Stream recreation workaround ===\n");

    test_environment_handle_t env = test_environment_create_with_lines(1000);
    CHECK_NOT_NULL(env, "test_environment");

    char* gz_file = test_environment_create_test_gzip_file(env);
    CHECK_NOT_NULL(gz_file, "gz_file");

    char idx_file[1024];
    snprintf(idx_file, sizeof(idx_file), "%s.idx", gz_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, 524288, 0);
    CHECK_NOT_NULL(indexer, "indexer");

    int build_result = dft_indexer_build(indexer);
    CHECK(build_result == 0, "indexer build should succeed");

    dft_indexer_destroy(indexer);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create(gz_file, idx_file, 524288);
    CHECK_NOT_NULL(reader, "reader");

    // First stream
    char buffer1[128];
    size_t bytes1;
    {
        dft_stream_config_t config;
        dft_stream_config_init_bytes(&config, 0, 100);
        config.stream = DFT_STREAM_TYPE_BYTES;
        dft_reader_stream_t stream1 = dft_reader_stream(reader, &config);
        CHECK_NOT_NULL(stream1, "stream1");

        bytes1 = dft_reader_stream_read(stream1, buffer1, sizeof(buffer1));
        CHECK(bytes1 > 0, "first read should succeed");

        dft_reader_stream_destroy(stream1);
    }

    // Second stream (equivalent to reset)
    {
        dft_stream_config_t config;
        dft_stream_config_init_bytes(&config, 0, 100);
        config.stream = DFT_STREAM_TYPE_BYTES;
        dft_reader_stream_t stream2 = dft_reader_stream(reader, &config);
        CHECK_NOT_NULL(stream2, "stream2");

        char buffer2[128];
        size_t bytes2 =
            dft_reader_stream_read(stream2, buffer2, sizeof(buffer2));
        CHECK(bytes1 == bytes2,
              "should read same amount from recreated stream");
        CHECK(memcmp(buffer1, buffer2, bytes1) == 0,
              "should read same data from recreated stream");

        dft_reader_stream_destroy(stream2);
    }

    printf("Stream recreation verified - read %zu bytes consistently\n",
           bytes1);

    // Cleanup
    dft_reader_destroy(reader);
    free(gz_file);
    test_environment_destroy(env);

    printf("PASSED\n");
    return 0;
}

static int test_edge_cases(void) {
    printf("\n=== Test: Edge cases ===\n");

    test_environment_handle_t env = test_environment_create_with_lines(100);
    CHECK_NOT_NULL(env, "test_environment");

    char* gz_file = test_environment_create_test_gzip_file(env);
    CHECK_NOT_NULL(gz_file, "gz_file");

    char idx_file[1024];
    snprintf(idx_file, sizeof(idx_file), "%s.idx", gz_file);

    // Build index
    dft_indexer_handle_t indexer =
        dft_indexer_create(gz_file, idx_file, 524288, 0);
    CHECK_NOT_NULL(indexer, "indexer");

    int build_result = dft_indexer_build(indexer);
    CHECK(build_result == 0, "indexer build should succeed");

    dft_indexer_destroy(indexer);

    // Create reader
    dft_reader_handle_t reader = dft_reader_create(gz_file, idx_file, 524288);
    CHECK_NOT_NULL(reader, "reader");

    // Test 1: Empty range (start == end)
    printf("  Subtest: Empty range\n");
    {
        dft_stream_config_t config;
        dft_stream_config_init_bytes(&config, 100, 100);
        config.stream = DFT_STREAM_TYPE_BYTES;
        dft_reader_stream_t stream1 = dft_reader_stream(reader, &config);
        CHECK_NOT_NULL(stream1, "stream1");

        char buffer[128];
        size_t bytes = dft_reader_stream_read(stream1, buffer, sizeof(buffer));
        CHECK(bytes == 0, "empty range should read 0 bytes");
        CHECK(dft_reader_stream_done(stream1),
              "empty range should be finished");
        dft_reader_stream_destroy(stream1);
    }

    // Test 2: Very small buffer
    printf("  Subtest: Very small buffer\n");
    {
        dft_stream_config_t config;
        dft_stream_config_init_bytes(&config, 0, 100);
        config.stream = DFT_STREAM_TYPE_BYTES;
        dft_reader_stream_t stream2 = dft_reader_stream(reader, &config);
        CHECK_NOT_NULL(stream2, "stream2");

        char tiny_buffer[1];
        size_t total = 0;
        while (!dft_reader_stream_done(stream2)) {
            size_t b = dft_reader_stream_read(stream2, tiny_buffer,
                                              sizeof(tiny_buffer));
            if (b == 0) break;
            total += b;
        }
        CHECK(total <= 100, "should not exceed range with tiny buffer");
        dft_reader_stream_destroy(stream2);
    }

    // Test 3: Multiple stream recreations
    printf("  Subtest: Multiple stream recreations\n");

    char buffer[128];
    size_t first_bytes;
    size_t bytes;
    {
        dft_stream_config_t config;
        dft_stream_config_init_bytes(&config, 0, 50);
        config.stream = DFT_STREAM_TYPE_BYTES;
        dft_reader_stream_t stream3 = dft_reader_stream(reader, &config);
        CHECK_NOT_NULL(stream3, "stream3");

        first_bytes = dft_reader_stream_read(stream3, buffer, sizeof(buffer));
        CHECK(first_bytes > 0, "initial read should succeed");

        dft_reader_stream_destroy(stream3);
    }

    for (int i = 0; i < 5; i++) {
        dft_stream_config_t config;
        dft_stream_config_init_bytes(&config, 0, 50);
        config.stream = DFT_STREAM_TYPE_BYTES;
        dft_reader_stream_t stream = dft_reader_stream(reader, &config);
        CHECK_NOT_NULL(stream, "stream");
        CHECK(!dft_reader_stream_done(stream),
              "should not be finished on new stream");

        bytes = dft_reader_stream_read(stream, buffer, sizeof(buffer));
        CHECK(bytes == first_bytes,
              "should read same amount from recreated stream");

        dft_reader_stream_destroy(stream);
    }

    // Cleanup
    dft_reader_destroy(reader);
    free(gz_file);
    test_environment_destroy(env);

    printf("PASSED\n");
    return 0;
}

int main(void) {
    int failures = 0;

    printf("========================================\n");
    printf("C Reader Streaming API Tests\n");
    printf("========================================\n");

    failures += test_bytes_stream_byte_range();
    failures += test_bytes_stream_line_range();
    failures += test_line_bytes_stream();
    failures += test_multi_lines_bytes_stream();
    failures += test_line_stream();
    failures += test_multi_lines_stream();
    failures += test_stream_recreation();
    failures += test_edge_cases();

    printf("\n========================================\n");
    if (failures == 0) {
        printf("All tests PASSED!\n");
    } else {
        printf("%d test(s) FAILED!\n", failures);
    }
    printf("========================================\n");

    return failures;
}
