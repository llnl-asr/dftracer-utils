#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/common/arrow/arrow.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::utilities::common::arrow;

static std::string tmp_path(const char* name) {
    return dftu_utils_test::make_unique_test_path(name).string();
}

static void write_test_file(const std::string& path, int num_batches,
                            int rows_per_batch,
                            IpcCompression compression = IpcCompression::NONE) {
    Runtime runtime(2);

    auto task = [&]() -> CoroTask<void> {
        IpcWriter writer;
        int rc = co_await writer.open(path, compression);
        if (rc != 0) co_return;

        RecordBatchBuilder builder;
        builder.declare_schema({{"id", ColumnType::INT64},
                                {"name", ColumnType::STRING},
                                {"value", ColumnType::DOUBLE}});

        for (int b = 0; b < num_batches; ++b) {
            builder.reserve(rows_per_batch);
            for (int i = 0; i < rows_per_batch; ++i) {
                int row_id = b * rows_per_batch + i;
                builder.append_int64(0, row_id);
                std::string name = "item_" + std::to_string(row_id);
                builder.append_string(1, name);
                builder.append_double(2, row_id * 1.5);
                builder.end_row();
            }
            auto batch = builder.finish();
            co_await writer.write_batch(batch);
            builder.reset(true);
        }

        co_await writer.close();
    };

    runtime.submit(task(), "write_test_file").get();
    runtime.shutdown();
}

// ---------------------------------------------------------------------------
// IpcReader Tests
// ---------------------------------------------------------------------------

TEST_CASE("IpcReader - basic read single batch") {
    std::string path = tmp_path("test_ipc_reader_basic.arrow");
    std::remove(path.c_str());

    write_test_file(path, 1, 10);

    IpcReader reader;
    CHECK_FALSE(reader.is_open());
    CHECK(reader.open(path) == 0);
    CHECK(reader.is_open());
    CHECK(reader.num_batches() == 1);

    auto batch = reader.read_batch(0);
    CHECK(batch.valid());
    CHECK(batch.num_rows() == 10);
    CHECK(batch.num_columns() == 3);

    reader.close();
    CHECK_FALSE(reader.is_open());

    fs::remove(path);
}

TEST_CASE("IpcReader - read multiple batches") {
    std::string path = tmp_path("test_ipc_reader_multi.arrow");
    std::remove(path.c_str());

    write_test_file(path, 5, 20);

    IpcReader reader;
    CHECK(reader.open(path) == 0);
    CHECK(reader.num_batches() == 5);

    std::int64_t total_rows = 0;
    for (std::size_t i = 0; i < reader.num_batches(); ++i) {
        auto batch = reader.read_batch(i);
        CHECK(batch.valid());
        CHECK(batch.num_rows() == 20);
        total_rows += batch.num_rows();
    }
    CHECK(total_rows == 100);

    reader.close();
    fs::remove(path);
}

TEST_CASE("IpcReader - read_all") {
    std::string path = tmp_path("test_ipc_reader_all.arrow");
    std::remove(path.c_str());

    write_test_file(path, 3, 15);

    IpcReader reader;
    CHECK(reader.open(path) == 0);

    auto batches = reader.read_all();
    CHECK(batches.size() == 3);

    std::int64_t total_rows = 0;
    for (const auto& batch : batches) {
        CHECK(batch.valid());
        total_rows += batch.num_rows();
    }
    CHECK(total_rows == 45);

    reader.close();
    fs::remove(path);
}

TEST_CASE("IpcReader - for_each_batch") {
    std::string path = tmp_path("test_ipc_reader_foreach.arrow");
    std::remove(path.c_str());

    write_test_file(path, 4, 25);

    IpcReader reader;
    CHECK(reader.open(path) == 0);

    std::int64_t total_rows = 0;
    int batch_count = 0;
    int rc = reader.for_each_batch([&](ArrowExportResult& batch) {
        CHECK(batch.valid());
        total_rows += batch.num_rows();
        batch_count++;
        return 0;
    });

    CHECK(rc == 0);
    CHECK(batch_count == 4);
    CHECK(total_rows == 100);

    reader.close();
    fs::remove(path);
}

TEST_CASE("IpcReader - open fails on non-existent file") {
    IpcReader reader;
    CHECK(reader.open("/nonexistent/path/file.arrow") != 0);
    CHECK_FALSE(reader.is_open());
}

TEST_CASE("IpcReader - open fails on invalid file") {
    std::string path = tmp_path("test_ipc_reader_invalid.arrow");
    std::remove(path.c_str());

    // Write garbage data
    std::FILE* f = std::fopen(path.c_str(), "wb");
    const char* garbage = "this is not an arrow file";
    std::fwrite(garbage, 1, strlen(garbage), f);
    std::fclose(f);

    IpcReader reader;
    CHECK(reader.open(path) != 0);
    CHECK_FALSE(reader.is_open());

    fs::remove(path);
}

TEST_CASE("IpcReader - move semantics") {
    std::string path = tmp_path("test_ipc_reader_move.arrow");
    std::remove(path.c_str());

    write_test_file(path, 2, 10);

    IpcReader r1;
    CHECK(r1.open(path) == 0);
    CHECK(r1.is_open());
    CHECK(r1.num_batches() == 2);

    IpcReader r2 = std::move(r1);
    CHECK_FALSE(r1.is_open());
    CHECK(r2.is_open());
    CHECK(r2.num_batches() == 2);

    auto batch = r2.read_batch(0);
    CHECK(batch.valid());

    r2.close();
    fs::remove(path);
}

TEST_CASE("IpcReader - read batch out of range") {
    std::string path = tmp_path("test_ipc_reader_range.arrow");
    std::remove(path.c_str());

    write_test_file(path, 2, 10);

    IpcReader reader;
    CHECK(reader.open(path) == 0);
    CHECK(reader.num_batches() == 2);

    // Valid indices
    CHECK(reader.read_batch(0).valid());
    CHECK(reader.read_batch(1).valid());

    // Invalid index
    CHECK_FALSE(reader.read_batch(2).valid());
    CHECK_FALSE(reader.read_batch(100).valid());

    reader.close();
    fs::remove(path);
}

#ifdef DFTRACER_UTILS_ENABLE_ZSTD
TEST_CASE("IpcReader - read ZSTD compressed file") {
    std::string path = tmp_path("test_ipc_reader_zstd.arrow");
    std::remove(path.c_str());

    write_test_file(path, 3, 50, IpcCompression::ZSTD);

    IpcReader reader;
    CHECK(reader.open(path) == 0);
    CHECK(reader.num_batches() == 3);

    auto batches = reader.read_all();
    CHECK(batches.size() == 3);

    std::int64_t total_rows = 0;
    for (const auto& batch : batches) {
        CHECK(batch.valid());
        total_rows += batch.num_rows();
    }
    CHECK(total_rows == 150);

    reader.close();
    fs::remove(path);
}
#endif

TEST_CASE("IpcReader - roundtrip with all column types") {
    std::string path = tmp_path("test_ipc_reader_types.arrow");
    std::remove(path.c_str());

    {
        Runtime runtime(2);

        auto task = [&]() -> CoroTask<void> {
            IpcWriter writer;
            co_await writer.open(path, IpcCompression::NONE);

            RecordBatchBuilder builder;
            builder.declare_schema({{"i64", ColumnType::INT64},
                                    {"u64", ColumnType::UINT64},
                                    {"f64", ColumnType::DOUBLE},
                                    {"str", ColumnType::STRING},
                                    {"boo", ColumnType::BOOL}});

            builder.reserve(3);
            for (int i = 0; i < 3; ++i) {
                builder.append_int64(0, -i);
                builder.append_uint64(1, i * 100);
                builder.append_double(2, i * 1.5);
                std::string s = "row_" + std::to_string(i);
                builder.append_string(3, s);
                builder.append_bool(4, i % 2 == 0);
                builder.end_row();
            }
            auto batch = builder.finish();
            co_await writer.write_batch(batch);
            co_await writer.close();
        };

        runtime.submit(task(), "write_types").get();
        runtime.shutdown();
    }

    // Read and verify
    {
        IpcReader reader;
        CHECK(reader.open(path) == 0);
        CHECK(reader.num_batches() == 1);

        auto batch = reader.read_batch(0);
        CHECK(batch.valid());
        CHECK(batch.num_rows() == 3);
        CHECK(batch.num_columns() == 5);

        reader.close();
    }

    fs::remove(path);
}

// ---------------------------------------------------------------------------
// Parallel Reader Tests
// ---------------------------------------------------------------------------

// Helper to run parallel read coroutine synchronously
static ParallelReadResult run_parallel_read(Runtime& runtime,
                                            std::vector<std::string> paths) {
    auto task = read_arrow_files_parallel(std::move(paths));
    return runtime.submit(std::move(task), "read_arrow_files").get();
}

TEST_CASE("read_arrow_files_parallel - single file") {
    std::string path = tmp_path("test_parallel_single.arrow");
    std::remove(path.c_str());

    write_test_file(path, 2, 50);

    Runtime runtime(2);

    std::vector<std::string> paths = {path};
    auto result = run_parallel_read(runtime, paths);

    CHECK(result.files_read == 1);
    CHECK(result.files_failed == 0);
    CHECK(result.total_rows == 100);
    CHECK(result.total_batches == 2);
    CHECK(result.file_results.size() == 1);
    CHECK(result.file_results[0].success);
    CHECK(result.file_results[0].batches->size() == 2);

    runtime.shutdown();
    fs::remove(path);
}

TEST_CASE("read_arrow_files_parallel - multiple files") {
    std::string dir = tmp_path("test_parallel_multi");
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::vector<std::string> paths;
    for (int i = 0; i < 4; ++i) {
        std::string path = dir + "/file_" + std::to_string(i) + ".arrow";
        write_test_file(path, 2, 25);
        paths.push_back(path);
    }

    Runtime runtime(4);

    auto result = run_parallel_read(runtime, paths);

    CHECK(result.files_read == 4);
    CHECK(result.files_failed == 0);
    CHECK(result.total_rows == 200);   // 4 files * 2 batches * 25 rows
    CHECK(result.total_batches == 8);  // 4 files * 2 batches
    CHECK(result.file_results.size() == 4);

    for (const auto& fr : result.file_results) {
        CHECK(fr.success);
        CHECK(fr.total_rows == 50);
        CHECK(fr.batches->size() == 2);
    }

    runtime.shutdown();
    fs::remove_all(dir);
}

TEST_CASE("read_arrow_files_parallel - handles non-existent files") {
    std::string path = tmp_path("test_parallel_exists.arrow");
    std::remove(path.c_str());
    write_test_file(path, 1, 10);

    Runtime runtime(2);

    std::vector<std::string> paths = {path, "/nonexistent/file.arrow"};

    auto result = run_parallel_read(runtime, paths);

    CHECK(result.files_read == 1);
    CHECK(result.files_failed == 1);
    CHECK(result.total_rows == 10);

    runtime.shutdown();
    fs::remove(path);
}

TEST_CASE("read_arrow_files_parallel - empty list") {
    Runtime runtime(2);

    std::vector<std::string> paths;
    auto result = run_parallel_read(runtime, paths);

    CHECK(result.files_read == 0);
    CHECK(result.files_failed == 0);
    CHECK(result.total_rows == 0);
    CHECK(result.total_batches == 0);
    CHECK(result.file_results.empty());

    runtime.shutdown();
}

TEST_CASE("read_arrow_files_streaming - completion order callback") {
    std::string dir = tmp_path("test_streaming");
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::vector<std::string> paths;
    for (int i = 0; i < 4; ++i) {
        std::string path = dir + "/file_" + std::to_string(i) + ".arrow";
        write_test_file(path, 1, 25);
        paths.push_back(path);
    }

    Runtime runtime(4);

    std::vector<std::string> received_paths;
    std::int64_t total_rows = 0;
    ParallelReadResult result;

    auto task = run_coro_scope(
        runtime.executor(),
        [&result, &received_paths, &total_rows](
            CoroScope& scope,
            std::vector<std::string> file_paths) -> CoroTask<void> {
            result = co_await read_arrow_files_streaming(
                scope, std::move(file_paths), [&](ArrowFileReadResult&& fr) {
                    if (fr.success) {
                        received_paths.push_back(fr.path);
                        total_rows += fr.total_rows;
                    }
                    return true;  // continue
                });
        },
        paths);

    runtime.submit(std::move(task), "test_streaming").get();

    CHECK(result.files_read == 4);
    CHECK(result.files_failed == 0);
    CHECK(result.total_rows == 100);
    CHECK(received_paths.size() == 4);
    CHECK(total_rows == 100);

    runtime.shutdown();
    fs::remove_all(dir);
}

TEST_CASE("read_arrow_files_streaming - early cancel") {
    std::string dir = tmp_path("test_streaming_cancel");
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::vector<std::string> paths;
    for (int i = 0; i < 4; ++i) {
        std::string path = dir + "/file_" + std::to_string(i) + ".arrow";
        write_test_file(path, 1, 25);
        paths.push_back(path);
    }

    Runtime runtime(4);

    int callback_count = 0;
    ParallelReadResult result;

    auto task = run_coro_scope(
        runtime.executor(),
        [&result, &callback_count](
            CoroScope& scope,
            std::vector<std::string> file_paths) -> CoroTask<void> {
            result = co_await read_arrow_files_streaming(
                scope, std::move(file_paths), [&](ArrowFileReadResult&&) {
                    callback_count++;
                    return callback_count < 2;  // cancel after 2
                });
        },
        paths);

    runtime.submit(std::move(task), "test_streaming_cancel").get();

    // All files still processed (for stats), but callback cancelled early
    CHECK(result.files_read == 4);
    CHECK(callback_count == 2);  // Only 2 callbacks before cancel

    runtime.shutdown();
    fs::remove_all(dir);
}

#ifdef DFTRACER_UTILS_ENABLE_ZSTD
TEST_CASE("read_arrow_files_parallel - mixed compression") {
    std::string dir = tmp_path("test_parallel_mixed");
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::string path_none = dir + "/none.arrow";
    std::string path_zstd = dir + "/zstd.arrow";

    write_test_file(path_none, 2, 30, IpcCompression::NONE);
    write_test_file(path_zstd, 2, 30, IpcCompression::ZSTD);

    Runtime runtime(2);

    std::vector<std::string> paths = {path_none, path_zstd};
    auto result = run_parallel_read(runtime, paths);

    CHECK(result.files_read == 2);
    CHECK(result.files_failed == 0);
    CHECK(result.total_rows == 120);
    CHECK(result.total_batches == 4);

    runtime.shutdown();
    fs::remove_all(dir);
}
#endif

#else

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("IpcReader - disabled") { CHECK(true); }

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
