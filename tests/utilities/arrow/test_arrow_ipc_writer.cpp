// SPDX-License-Identifier: MIT

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/utilities/arrow/column_builder.h>
#include <dftracer/utils/utilities/arrow/ipc_writer.h>
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

TEST_CASE("IpcWriter - basic write and close") {
    dstd::String path = tmp_path("test_ipc_basic.arrows");
    std::remove(path.c_str());

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        RecordBatchBuilder builder;
        builder.declare_schema({{"id", ColumnType::Int64},
                                {"name", ColumnType::String},
                                {"value", ColumnType::Double}});
        builder.reserve(2);

        dstd::String s0 = "hello", s1 = "world";
        builder.append_int64(0, 1);
        builder.append_string(1, s0);
        builder.append_double(2, 3.14);
        builder.end_row();
        builder.append_int64(0, 2);
        builder.append_string(1, s1);
        builder.append_double(2, 2.72);
        builder.end_row();

        auto batch = dftu_utils_test::ok(builder.finish());

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
    CHECK(dstd::fs::exists(path).value());
    CHECK(dstd::fs::file_size(path).value() > 0);
    dstd::fs::remove(path).discard();
}

TEST_CASE("IpcWriter - multiple batches") {
    dstd::String path = tmp_path("test_ipc_multi.arrows");
    std::remove(path.c_str());

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        IpcWriter writer;
        if (co_await writer.open(path) != 0) co_return 1;

        RecordBatchBuilder builder;
        builder.declare_schema({{"x", ColumnType::Int64}});

        for (int b = 0; b < 3; ++b) {
            builder.reserve(10);
            for (int i = 0; i < 10; ++i) {
                builder.append_int64(0, b * 10 + i);
                builder.end_row();
            }
            auto batch = dftu_utils_test::ok(builder.finish());
            if (co_await writer.write_batch(batch) != 0) co_return 2;
            builder.reset(true);
        }

        if (co_await writer.close() != 0) co_return 3;
        co_return 0;
    };
    auto result = runtime.submit(task(), "test").get();

    CHECK(result == 0);
    CHECK(dstd::fs::exists(path).value());
    CHECK(dstd::fs::file_size(path).value() > 0);
    dstd::fs::remove(path).discard();
}

TEST_CASE("IpcWriter - close without writing batches") {
    dstd::String path = tmp_path("test_ipc_empty.arrows");
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
    dstd::fs::remove(path).discard();
}

// With no pooled slot every batch takes the overflow slot, which the writer
// must own and free; the file is the same as with the pool.
TEST_CASE("IpcWriter - batches without a pooled slot match the pooled file") {
    auto write_file = [&](const dstd::String& path, IpcCompression compression,
                          std::size_t pool_slots) {
        std::remove(path.c_str());
        Runtime runtime(2);
        auto task = [&]() -> CoroTask<int> {
            IpcWriter writer;
            if (co_await writer.open(path, compression, pool_slots) != 0)
                co_return 1;
            RecordBatchBuilder builder;
            builder.declare_schema(
                {{"x", ColumnType::Int64}, {"y", ColumnType::Double}});
            for (int b = 0; b < 4; ++b) {
                builder.reserve(50);
                for (int i = 0; i < 50; ++i) {
                    builder.append_int64(0, b * 50 + i);
                    builder.append_double(1, i * 0.5);
                    builder.end_row();
                }
                auto batch = dftu_utils_test::ok(builder.finish());
                if (co_await writer.write_batch(batch) != 0) co_return 2;
                builder.reset(true);
            }
            if (co_await writer.close() != 0) co_return 3;
            co_return 0;
        };
        return runtime.submit(task(), "test").get();
    };
    dstd::Vector<IpcCompression> modes{IpcCompression::None};
    modes.push_back(IpcCompression::Zstd);
    for (const IpcCompression mode : modes) {
        const dstd::String pooled = tmp_path("test_ipc_pooled.arrows");
        const dstd::String bare = tmp_path("test_ipc_bare.arrows");
        REQUIRE(write_file(pooled, mode, 4) == 0);
        REQUIRE(write_file(bare, mode, 0) == 0);
        CHECK(dstd::fs::file_size(bare).value() ==
              dstd::fs::file_size(pooled).value());
        dstd::fs::remove(pooled).discard();
        dstd::fs::remove(bare).discard();
    }
}

TEST_CASE("IpcWriter - explicit ZSTD compression") {
    dstd::String path = tmp_path("test_ipc_zstd_compression.arrows");
    std::remove(path.c_str());

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        RecordBatchBuilder builder;
        builder.declare_schema(
            {{"x", ColumnType::Int64}, {"y", ColumnType::Double}});
        builder.reserve(100);
        for (int i = 0; i < 100; ++i) {
            builder.append_int64(0, i);
            builder.append_double(1, i * 1.5);
            builder.end_row();
        }
        auto batch = dftu_utils_test::ok(builder.finish());

        IpcWriter writer;
        if (co_await writer.open(path, IpcCompression::Zstd) != 0) co_return 1;
        if (co_await writer.write_batch(batch) != 0) co_return 2;
        if (co_await writer.close() != 0) co_return 3;
        co_return 0;
    };
    auto result = runtime.submit(task(), "test").get();

    CHECK(result == 0);
    CHECK(dstd::fs::exists(path).value());
    CHECK(dstd::fs::file_size(path).value() > 0);
    dstd::fs::remove(path).discard();
}

#else

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// Nothing to test without the writer: reported as skipped, not as a pass.
TEST_CASE("IpcWriter - the Arrow IPC writer is not built" * doctest::skip()) {}

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
