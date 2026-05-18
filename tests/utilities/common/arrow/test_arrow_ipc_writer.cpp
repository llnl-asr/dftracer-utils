#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/utilities/common/arrow/arrow.h>
#include <doctest/doctest.h>

#include <cstdio>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::utilities::common::arrow;

static std::string tmp_path(const char* name) {
    return (fs::temp_directory_path() / name).string();
}

TEST_CASE("IpcWriter - basic write and close") {
    std::string path = tmp_path("test_ipc_basic.arrows");
    std::remove(path.c_str());

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        RecordBatchBuilder builder;
        builder.declare_schema({{"id", ColumnType::INT64},
                                {"name", ColumnType::STRING},
                                {"value", ColumnType::DOUBLE}});
        builder.reserve(2);

        std::string s0 = "hello", s1 = "world";
        builder.append_int64(0, 1);
        builder.append_string(1, s0);
        builder.append_double(2, 3.14);
        builder.end_row();
        builder.append_int64(0, 2);
        builder.append_string(1, s1);
        builder.append_double(2, 2.72);
        builder.end_row();

        auto batch = builder.finish();

        IpcWriter writer;
        if (co_await writer.open(path) != 0) co_return 1;
        if (!writer.is_open()) co_return 2;
        if (co_await writer.write_batch(batch) != 0) co_return 3;
        if (co_await writer.close() != 0) co_return 4;
        if (writer.is_open()) co_return 5;
        co_return 0;
    };
    auto result = runtime.submit(task(), "test").get();

    CHECK(result == 0);
    CHECK(fs::exists(path));
    CHECK(fs::file_size(path) > 0);
    fs::remove(path);
}

TEST_CASE("IpcWriter - multiple batches") {
    std::string path = tmp_path("test_ipc_multi.arrows");
    std::remove(path.c_str());

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        IpcWriter writer;
        if (co_await writer.open(path) != 0) co_return 1;

        RecordBatchBuilder builder;
        builder.declare_schema({{"x", ColumnType::INT64}});

        for (int b = 0; b < 3; ++b) {
            builder.reserve(10);
            for (int i = 0; i < 10; ++i) {
                builder.append_int64(0, b * 10 + i);
                builder.end_row();
            }
            auto batch = builder.finish();
            if (co_await writer.write_batch(batch) != 0) co_return 2;
            builder.reset(true);
        }

        if (co_await writer.close() != 0) co_return 3;
        co_return 0;
    };
    auto result = runtime.submit(task(), "test").get();

    CHECK(result == 0);
    CHECK(fs::exists(path));
    CHECK(fs::file_size(path) > 0);
    fs::remove(path);
}

TEST_CASE("IpcWriter - close without writing batches") {
    std::string path = tmp_path("test_ipc_empty.arrows");
    std::remove(path.c_str());

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        IpcWriter writer;
        if (co_await writer.open(path) != 0) co_return 1;
        if (co_await writer.close() != 0) co_return 2;
        if (writer.is_open()) co_return 3;
        co_return 0;
    };
    auto result = runtime.submit(task(), "test").get();

    CHECK(result == 0);
    fs::remove(path);
}

#ifdef DFTRACER_UTILS_ENABLE_ZSTD
TEST_CASE("IpcWriter - explicit ZSTD compression") {
    std::string path = tmp_path("test_ipc_zstd_compression.arrows");
    std::remove(path.c_str());

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        RecordBatchBuilder builder;
        builder.declare_schema(
            {{"x", ColumnType::INT64}, {"y", ColumnType::DOUBLE}});
        builder.reserve(100);
        for (int i = 0; i < 100; ++i) {
            builder.append_int64(0, i);
            builder.append_double(1, i * 1.5);
            builder.end_row();
        }
        auto batch = builder.finish();

        IpcWriter writer;
        if (co_await writer.open(path, IpcCompression::ZSTD) != 0) co_return 1;
        if (co_await writer.write_batch(batch) != 0) co_return 2;
        if (co_await writer.close() != 0) co_return 3;
        co_return 0;
    };
    auto result = runtime.submit(task(), "test").get();

    CHECK(result == 0);
    CHECK(fs::exists(path));
    CHECK(fs::file_size(path) > 0);
    fs::remove(path);
}
#endif

TEST_CASE("PartitionWriter - basic single file") {
    std::string dir = tmp_path("test_partition_basic");
    fs::remove_all(dir);

    Runtime runtime(2);
    PartitionWriteStats stats;

    auto task = [&]() -> CoroTask<int> {
        PartitionWriter writer;
        if (co_await writer.open(dir, 0) != 0) co_return 1;
        if (!writer.is_open()) co_return 2;

        RecordBatchBuilder builder;
        builder.declare_schema({{"id", ColumnType::INT64}});
        builder.reserve(100);
        for (int i = 0; i < 100; ++i) {
            builder.append_int64(0, i);
            builder.end_row();
        }
        auto batch = builder.finish();

        if (co_await writer.write_batch(batch) != 0) co_return 3;
        stats = co_await writer.close();
        if (writer.is_open()) co_return 4;
        co_return 0;
    };
    auto result = runtime.submit(task(), "test").get();

    CHECK(result == 0);
    CHECK(stats.files.size() == 1);
    CHECK(stats.total_rows == 100);
    CHECK(stats.row_counts.size() == 1);
    CHECK(stats.row_counts[0] == 100);
    CHECK(fs::exists(stats.files[0]));

    fs::remove_all(dir);
}

TEST_CASE("PartitionRouter - NONE mode pass-through") {
    std::string dir = tmp_path("test_router_none");
    fs::remove_all(dir);

    PartitionConfig config;
    config.mode = PartitionConfig::Mode::NONE;

    Runtime runtime(2);
    RouterWriteStats stats;

    auto task = [&]() -> CoroTask<int> {
        PartitionRouter router;
        if (router.open(dir, config, 0) != 0) co_return 1;

        RecordBatchBuilder builder;
        builder.declare_schema(
            {{"id", ColumnType::INT64}, {"cat", ColumnType::STRING}});
        builder.reserve(10);
        for (int i = 0; i < 10; ++i) {
            builder.append_int64(0, i);
            builder.append_string(1, "POSIX");
            builder.end_row();
        }
        auto batch = builder.finish();

        if (co_await router.write_batch(batch) != 0) co_return 2;
        stats = co_await router.close();
        co_return 0;
    };
    auto result = runtime.submit(task(), "test").get();

    CHECK(result == 0);
    CHECK(stats.total_rows == 10);
    CHECK(stats.partitions.size() == 1);
    CHECK(stats.partitions.count("") == 1);

    fs::remove_all(dir);
}

TEST_CASE("PartitionRouter - COLUMN mode single column") {
    std::string dir = tmp_path("test_router_column");
    fs::remove_all(dir);

    PartitionConfig config;
    config.mode = PartitionConfig::Mode::COLUMN;
    config.partition_columns = {"cat"};

    Runtime runtime(2);
    RouterWriteStats stats;

    auto task = [&]() -> CoroTask<int> {
        PartitionRouter router;
        if (router.open(dir, config, 0) != 0) co_return 1;

        RecordBatchBuilder builder;
        builder.declare_schema(
            {{"id", ColumnType::INT64}, {"cat", ColumnType::STRING}});
        builder.reserve(6);

        for (int i = 0; i < 3; ++i) {
            builder.append_int64(0, i);
            builder.append_string(1, "POSIX");
            builder.end_row();
        }
        for (int i = 3; i < 6; ++i) {
            builder.append_int64(0, i);
            builder.append_string(1, "APP");
            builder.end_row();
        }
        auto batch = builder.finish();

        if (co_await router.write_batch(batch) != 0) co_return 2;
        stats = co_await router.close();
        co_return 0;
    };
    auto result = runtime.submit(task(), "test").get();

    CHECK(result == 0);
    CHECK(stats.total_rows == 6);
    CHECK(stats.partitions.size() == 2);
    CHECK(stats.partitions.count("cat=POSIX") == 1);
    CHECK(stats.partitions.count("cat=APP") == 1);
    CHECK(stats.partitions["cat=POSIX"].total_rows == 3);
    CHECK(stats.partitions["cat=APP"].total_rows == 3);

    CHECK(fs::exists(dir + "/cat=POSIX"));
    CHECK(fs::exists(dir + "/cat=APP"));

    fs::remove_all(dir);
}

#else

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("IpcWriter - disabled") { CHECK(true); }

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
