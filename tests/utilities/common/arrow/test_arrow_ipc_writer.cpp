#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/arrow/arrow.h>
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace dftracer::utils::utilities::common::arrow;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string tmp_path(const char* name) {
    return std::string("/tmp/") + name;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("IpcWriter - basic write and close") {
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

    std::string path = tmp_path("test_ipc_basic.arrows");
    std::remove(path.c_str());

    IpcWriter writer;
    CHECK_FALSE(writer.is_open());
    CHECK(writer.open(path) == 0);
    CHECK(writer.is_open());
    CHECK(writer.write_batch(batch) == 0);
    CHECK(writer.close() == 0);
    CHECK_FALSE(writer.is_open());

    CHECK(fs::exists(path));
    CHECK(fs::file_size(path) > 0);
    fs::remove(path);
}

TEST_CASE("IpcWriter - multiple batches") {
    std::string path = tmp_path("test_ipc_multi.arrows");
    std::remove(path.c_str());

    IpcWriter writer;
    CHECK(writer.open(path) == 0);

    RecordBatchBuilder builder;
    builder.declare_schema({{"x", ColumnType::INT64}});

    for (int b = 0; b < 3; ++b) {
        builder.reserve(10);
        for (int i = 0; i < 10; ++i) {
            builder.append_int64(0, b * 10 + i);
            builder.end_row();
        }
        auto batch = builder.finish();
        CHECK(writer.write_batch(batch) == 0);
        builder.reset(true);
    }

    CHECK(writer.close() == 0);
    CHECK(fs::exists(path));
    CHECK(fs::file_size(path) > 0);
    fs::remove(path);
}

TEST_CASE("IpcWriter - close without writing batches") {
    std::string path = tmp_path("test_ipc_empty.arrows");
    std::remove(path.c_str());

    IpcWriter writer;
    CHECK(writer.open(path) == 0);
    // No write_batch calls — close should still succeed (no footer needed).
    CHECK(writer.close() == 0);
    CHECK_FALSE(writer.is_open());
    fs::remove(path);
}

TEST_CASE("IpcWriter - double close is safe") {
    std::string path = tmp_path("test_ipc_dblclose.arrows");
    std::remove(path.c_str());

    IpcWriter writer;
    CHECK(writer.open(path) == 0);
    CHECK(writer.close() == 0);
    CHECK(writer.close() == 0);  // idempotent
    fs::remove(path);
}

TEST_CASE("IpcWriter - move semantics") {
    std::string path = tmp_path("test_ipc_move.arrows");
    std::remove(path.c_str());

    IpcWriter w1;
    CHECK(w1.open(path) == 0);
    CHECK(w1.is_open());

    IpcWriter w2 = std::move(w1);
    CHECK_FALSE(w1.is_open());
    CHECK(w2.is_open());

    RecordBatchBuilder builder;
    builder.declare_schema({{"v", ColumnType::UINT64}});
    builder.append_uint64(0, 42);
    builder.end_row();
    auto batch = builder.finish();

    CHECK(w2.write_batch(batch) == 0);
    CHECK(w2.close() == 0);

    CHECK(fs::exists(path));
    CHECK(fs::file_size(path) > 0);
    fs::remove(path);
}

TEST_CASE("IpcWriter - open fails on bad path") {
    IpcWriter writer;
    CHECK(writer.open("/nonexistent_dir/no_such_file.arrows") != 0);
    CHECK_FALSE(writer.is_open());
}

TEST_CASE("IpcWriter - all column types") {
    std::string path = tmp_path("test_ipc_types.arrows");
    std::remove(path.c_str());

    RecordBatchBuilder builder;
    builder.declare_schema({{"i64", ColumnType::INT64},
                            {"u64", ColumnType::UINT64},
                            {"f64", ColumnType::DOUBLE},
                            {"str", ColumnType::STRING},
                            {"boo", ColumnType::BOOL}});

    std::string sv = "test";
    builder.append_int64(0, -1);
    builder.append_uint64(1, 1);
    builder.append_double(2, 1.0);
    builder.append_string(3, sv);
    builder.append_bool(4, true);
    builder.end_row();

    auto batch = builder.finish();

    IpcWriter writer;
    CHECK(writer.open(path) == 0);
    CHECK(writer.write_batch(batch) == 0);
    CHECK(writer.close() == 0);

    CHECK(fs::exists(path));
    CHECK(fs::file_size(path) > 0);
    fs::remove(path);
}

#else

// Provide main when IPC is disabled so the binary still links.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("IpcWriter - disabled") { CHECK(true); }

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
