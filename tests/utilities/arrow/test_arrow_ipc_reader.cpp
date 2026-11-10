// SPDX-License-Identifier: MIT

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/utilities/arrow/column_builder.h>
#include <dftracer/utils/utilities/arrow/ipc_reader.h>
#include <dftracer/utils/utilities/arrow/ipc_writer.h>
#include <dftracer/utils/utilities/arrow/parallel_reader.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdio>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::utilities::arrow;

static dstd::String tmp_path(const char* name) {
    return dftu_utils_test::make_unique_test_path(name).str();
}

static void write_test_file(const dstd::String& path, int batch_count,
                            int rows_per_batch,
                            IpcCompression compression = IpcCompression::None) {
    Runtime runtime(2);

    auto task = [&]() -> CoroTask<void> {
        IpcWriter writer;
        int rc = co_await writer.open(path, compression);
        if (rc != 0) co_return;

        RecordBatchBuilder builder;
        builder.declare_schema({{"id", ColumnType::Int64},
                                {"name", ColumnType::String},
                                {"value", ColumnType::Double}});

        for (int b = 0; b < batch_count; ++b) {
            builder.reserve(rows_per_batch);
            for (int i = 0; i < rows_per_batch; ++i) {
                int row_id = b * rows_per_batch + i;
                builder.append_int64(0, row_id);
                dstd::String name = "item_" + dstd::to_string(row_id);
                builder.append_string(1, name);
                builder.append_double(2, row_id * 1.5);
                builder.end_row();
            }
            auto batch = dftu_utils_test::ok(builder.finish());
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
    dstd::String path = tmp_path("test_ipc_reader_basic.arrow");
    std::remove(path.c_str());

    write_test_file(path, 1, 10);

    IpcReader reader;
    CHECK_FALSE(reader.is_open());
    CHECK(reader.open(path) == 0);
    CHECK(reader.is_open());
    CHECK(reader.batch_count() == 1);

    auto batch = reader.read_batch(0);
    CHECK(batch.valid());
    CHECK(batch.num_rows() == 10);
    CHECK(batch.num_columns() == 3);

    reader.close();
    CHECK_FALSE(reader.is_open());

    dstd::fs::remove(path).discard();
}

TEST_CASE("IpcReader - read multiple batches") {
    dstd::String path = tmp_path("test_ipc_reader_multi.arrow");
    std::remove(path.c_str());

    write_test_file(path, 5, 20);

    IpcReader reader;
    CHECK(reader.open(path) == 0);
    CHECK(reader.batch_count() == 5);

    std::int64_t total_rows = 0;
    for (std::size_t i = 0; i < reader.batch_count(); ++i) {
        auto batch = reader.read_batch(i);
        CHECK(batch.valid());
        CHECK(batch.num_rows() == 20);
        total_rows += batch.num_rows();
    }
    CHECK(total_rows == 100);

    reader.close();
    dstd::fs::remove(path).discard();
}

TEST_CASE("IpcReader - read_all") {
    dstd::String path = tmp_path("test_ipc_reader_all.arrow");
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
    dstd::fs::remove(path).discard();
}

TEST_CASE("IpcReader - for_each_batch") {
    dstd::String path = tmp_path("test_ipc_reader_foreach.arrow");
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
    dstd::fs::remove(path).discard();
}

TEST_CASE("IpcReader - open fails on non-existent file") {
    IpcReader reader;
    CHECK(reader.open("/nonexistent/path/file.arrow") != 0);
    CHECK_FALSE(reader.is_open());
}

TEST_CASE("IpcReader - open fails on invalid file") {
    dstd::String path = tmp_path("test_ipc_reader_invalid.arrow");
    std::remove(path.c_str());

    // Write garbage data
    std::FILE* f = std::fopen(path.c_str(), "wb");
    const char* garbage = "this is not an arrow file";
    std::fwrite(garbage, 1, strlen(garbage), f);
    std::fclose(f);

    IpcReader reader;
    CHECK(reader.open(path) != 0);
    CHECK_FALSE(reader.is_open());

    dstd::fs::remove(path).discard();
}

TEST_CASE("IpcReader - move semantics") {
    dstd::String path = tmp_path("test_ipc_reader_move.arrow");
    std::remove(path.c_str());

    write_test_file(path, 2, 10);

    IpcReader r1;
    CHECK(r1.open(path) == 0);
    CHECK(r1.is_open());
    CHECK(r1.batch_count() == 2);

    IpcReader r2 = std::move(r1);
    CHECK_FALSE(r1.is_open());
    CHECK(r2.is_open());
    CHECK(r2.batch_count() == 2);

    auto batch = r2.read_batch(0);
    CHECK(batch.valid());

    r2.close();
    dstd::fs::remove(path).discard();
}

TEST_CASE("IpcReader - read batch out of range") {
    dstd::String path = tmp_path("test_ipc_reader_range.arrow");
    std::remove(path.c_str());

    write_test_file(path, 2, 10);

    IpcReader reader;
    CHECK(reader.open(path) == 0);
    CHECK(reader.batch_count() == 2);

    // Valid indices
    CHECK(reader.read_batch(0).valid());
    CHECK(reader.read_batch(1).valid());

    // Invalid index
    CHECK_FALSE(reader.read_batch(2).valid());
    CHECK_FALSE(reader.read_batch(100).valid());

    reader.close();
    dstd::fs::remove(path).discard();
}

TEST_CASE("IpcReader - read ZSTD compressed file") {
    dstd::String path = tmp_path("test_ipc_reader_zstd.arrow");
    std::remove(path.c_str());

    write_test_file(path, 3, 50, IpcCompression::Zstd);

    IpcReader reader;
    CHECK(reader.open(path) == 0);
    CHECK(reader.batch_count() == 3);

    auto batches = reader.read_all();
    CHECK(batches.size() == 3);

    std::int64_t total_rows = 0;
    for (const auto& batch : batches) {
        CHECK(batch.valid());
        total_rows += batch.num_rows();
    }
    CHECK(total_rows == 150);

    reader.close();
    dstd::fs::remove(path).discard();
}

TEST_CASE("IpcReader - roundtrip with all column types") {
    dstd::String path = tmp_path("test_ipc_reader_types.arrow");
    std::remove(path.c_str());

    {
        Runtime runtime(2);

        auto task = [&]() -> CoroTask<void> {
            IpcWriter writer;
            co_await writer.open(path, IpcCompression::None);

            RecordBatchBuilder builder;
            builder.declare_schema({{"i64", ColumnType::Int64},
                                    {"u64", ColumnType::Uint64},
                                    {"f64", ColumnType::Double},
                                    {"str", ColumnType::String},
                                    {"boo", ColumnType::Bool}});

            builder.reserve(3);
            for (int i = 0; i < 3; ++i) {
                builder.append_int64(0, -i);
                builder.append_uint64(1, i * 100);
                builder.append_double(2, i * 1.5);
                dstd::String s = "row_" + dstd::to_string(i);
                builder.append_string(3, s);
                builder.append_bool(4, i % 2 == 0);
                builder.end_row();
            }
            auto batch = dftu_utils_test::ok(builder.finish());
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
        CHECK(reader.batch_count() == 1);

        auto batch = reader.read_batch(0);
        CHECK(batch.valid());
        CHECK(batch.num_rows() == 3);
        CHECK(batch.num_columns() == 5);

        reader.close();
    }

    dstd::fs::remove(path).discard();
}

// ---------------------------------------------------------------------------
// Parallel Reader Tests
// ---------------------------------------------------------------------------

// Helper to run parallel read coroutine synchronously
static ParallelReadResult run_parallel_read(Runtime& runtime,
                                            dstd::Vector<dstd::String> paths) {
    auto task = read_arrow_files_parallel(std::move(paths));
    return runtime.submit(std::move(task), "read_arrow_files").get();
}

TEST_CASE("read_arrow_files_parallel - single file") {
    dstd::String path = tmp_path("test_parallel_single.arrow");
    std::remove(path.c_str());

    write_test_file(path, 2, 50);

    Runtime runtime(2);

    dstd::Vector<dstd::String> paths = {path};
    auto result = run_parallel_read(runtime, paths);

    CHECK(result.files_read == 1);
    CHECK(result.files_failed == 0);
    CHECK(result.total_rows == 100);
    CHECK(result.total_batches == 2);
    CHECK(result.file_results.size() == 1);
    CHECK(result.file_results[0].success);
    CHECK(result.file_results[0].batches->size() == 2);

    runtime.shutdown();
    dstd::fs::remove(path).discard();
}

TEST_CASE("read_arrow_files_parallel - multiple files") {
    dstd::String dir = tmp_path("test_parallel_multi");
    REQUIRE(dstd::fs::remove_all(dir).has_value());
    REQUIRE(dstd::fs::create_directories(dir).has_value());

    dstd::Vector<dstd::String> paths;
    for (int i = 0; i < 4; ++i) {
        dstd::String path = dir + "/file_" + dstd::to_string(i) + ".arrow";
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
    dstd::fs::remove_all(dir).discard();
}

TEST_CASE("read_arrow_files_parallel - handles non-existent files") {
    dstd::String path = tmp_path("test_parallel_exists.arrow");
    std::remove(path.c_str());
    write_test_file(path, 1, 10);

    Runtime runtime(2);

    dstd::Vector<dstd::String> paths = {path, "/nonexistent/file.arrow"};

    auto result = run_parallel_read(runtime, paths);

    CHECK(result.files_read == 1);
    CHECK(result.files_failed == 1);
    CHECK(result.total_rows == 10);

    runtime.shutdown();
    dstd::fs::remove(path).discard();
}

TEST_CASE("read_arrow_files_parallel - empty list") {
    Runtime runtime(2);

    dstd::Vector<dstd::String> paths;
    auto result = run_parallel_read(runtime, paths);

    CHECK(result.files_read == 0);
    CHECK(result.files_failed == 0);
    CHECK(result.total_rows == 0);
    CHECK(result.total_batches == 0);
    CHECK(result.file_results.empty());

    runtime.shutdown();
}

TEST_CASE("read_arrow_files_parallel - mixed compression") {
    dstd::String dir = tmp_path("test_parallel_mixed");
    REQUIRE(dstd::fs::remove_all(dir).has_value());
    REQUIRE(dstd::fs::create_directories(dir).has_value());

    dstd::String path_none = dir + "/none.arrow";
    dstd::String path_zstd = dir + "/zstd.arrow";

    write_test_file(path_none, 2, 30, IpcCompression::None);
    write_test_file(path_zstd, 2, 30, IpcCompression::Zstd);

    Runtime runtime(2);

    dstd::Vector<dstd::String> paths = {path_none, path_zstd};
    auto result = run_parallel_read(runtime, paths);

    CHECK(result.files_read == 2);
    CHECK(result.files_failed == 0);
    CHECK(result.total_rows == 120);
    CHECK(result.total_batches == 4);

    runtime.shutdown();
    dstd::fs::remove_all(dir).discard();
}

#else

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("IpcReader - disabled") { CHECK(true); }

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
