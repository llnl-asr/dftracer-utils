// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/store/abi.h>
#include <dftracer/utils/store/data_file.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;

namespace {

dstd::String data_file_of(const char* dir) {
    dstd::String found;
    for (const auto& entry : dftu_utils_test::list_dir(dstd::String(dir))) {
        if (entry.path.view().extension() == ".dfs") {
            found = dstd::String(entry.path.str());
        }
    }
    return found;
}

// Cuts the footer and removes the summary, so that opening the store recovers
// the commit chain by reading the file and keeps its descriptor.
void drop_footer(const char* dir) {
    const dstd::String path = data_file_of(dir);
    REQUIRE_FALSE(path.empty());
    const dstd::String bytes = dftu_utils_test::read_text(path);
    REQUIRE(bytes.size() >= store::engine::FILE_TRAILER_BYTES);
    const std::uint64_t footer = dstd::read_u64_le(
        reinterpret_cast<const std::uint8_t*>(bytes.data()) + bytes.size() -
        store::engine::FILE_TRAILER_BYTES + 8U);
    REQUIRE(footer < bytes.size());
    REQUIRE(::truncate(path.c_str(), static_cast<off_t>(footer)) == 0);
    ::unlink((dstd::String(dir) + "/" +
              dstd::String(store::engine::SUMMARY_FILE_NAME))
                 .c_str());
}

int descriptor_of_data_file(const char* dir) {
    const dstd::String found = data_file_of(dir);
    struct stat file_stat{};
    if (found.empty() || ::stat(found.c_str(), &file_stat) != 0) return -1;
    for (int fd = 0; fd < 4096; ++fd) {
        struct stat fd_stat{};
        if (::fstat(fd, &fd_stat) == 0 && fd_stat.st_dev == file_stat.st_dev &&
            fd_stat.st_ino == file_stat.st_ino) {
            return fd;
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("cursor_next returns the failure of a scan that cannot read") {
    dftu_utils_test::ScopedTestDir dir("store_abi_cursor_error");
    const dstd::String root = dir.str();
    const char* path = "/traces/a.pfw";
    const dftu_store_scope scope = {DFTU_STORE_SCOPE_FILE, &path, 1};

    dftu_store_t* writer = nullptr;
    REQUIRE(dftu_store_open(root.c_str(), &writer, nullptr) == DFTU_OK);
    dftu_store_ns_t* ns = nullptr;
    REQUIRE(dftu_store_ns(writer, "dftu.test.cursor", 1, DFTU_STORE_FOLD_NONE,
                          0, 0, &ns, nullptr) == DFTU_OK);
    dftu_store_batch_t* batch = nullptr;
    REQUIRE(dftu_store_batch(writer, ns, &scope, 0, nullptr, 0, &batch,
                             nullptr) == DFTU_OK);
    for (int i = 0; i < 1000; ++i) {
        const dstd::String key = dstd::fmt<"k-{}">(i);
        const dstd::String value = dstd::fmt<"value-{}">(i);
        REQUIRE(dftu_store_batch_put(
                    batch, reinterpret_cast<const std::uint8_t*>(key.data()),
                    key.size(),
                    reinterpret_cast<const std::uint8_t*>(value.data()),
                    value.size(), nullptr) == DFTU_OK);
    }
    REQUIRE(dftu_store_batch_commit(batch, nullptr) == DFTU_OK);
    REQUIRE(dftu_store_close(writer, nullptr) == DFTU_OK);
    dftu_store_ns_free(ns);
    dftu_store_free(writer);

    drop_footer(root.c_str());

    dftu_store_t* store = nullptr;
    REQUIRE(dftu_store_open(root.c_str(), &store, nullptr) == DFTU_OK);
    REQUIRE(dftu_store_ns(store, "dftu.test.cursor", 1, DFTU_STORE_FOLD_NONE, 0,
                          0, &ns, nullptr) == DFTU_OK);
    dftu_store_snapshot_t* snapshot = nullptr;
    REQUIRE(dftu_store_snapshot(store, &snapshot, nullptr) == DFTU_OK);

    // A directory in place of the data file's descriptor makes every read of
    // the file fail with EISDIR.
    const int data_fd = descriptor_of_data_file(root.c_str());
    REQUIRE(data_fd >= 0);
    const int saved_fd = ::dup(data_fd);
    REQUIRE(saved_fd >= 0);
    const int dir_fd = ::open(root.c_str(), O_RDONLY);
    REQUIRE(dir_fd >= 0);
    REQUIRE(::dup2(dir_fd, data_fd) == data_fd);

    dftu_store_cursor_t* cursor = nullptr;
    REQUIRE(dftu_store_snapshot_scan_prefix(
                snapshot, ns, &scope,
                reinterpret_cast<const std::uint8_t*>("k-"), 2, &cursor,
                nullptr) == DFTU_OK);
    dftu_error err = DFTU_ERROR_INIT;
    const dftu_status first = dftu_store_cursor_next(cursor, &err);
    CHECK(first < 0);
    CHECK(err.status == first);
    CHECK(err.code != 0);
    REQUIRE(err.message != nullptr);
    CHECK(std::strlen(err.message) != 0);
    dftu_error_release(&err);
    CHECK(err.message == nullptr);

    dftu_error again = DFTU_ERROR_INIT;
    CHECK(dftu_store_cursor_next(cursor, &again) == first);
    CHECK(again.status == first);
    dftu_error_release(&again);
    CHECK(dftu_store_cursor_next(cursor, nullptr) == first);

    REQUIRE(::dup2(saved_fd, data_fd) == data_fd);
    ::close(saved_fd);
    ::close(dir_fd);
    dftu_store_cursor_free(cursor);
    dftu_store_snapshot_free(snapshot);
    dftu_store_ns_free(ns);
    dftu_store_free(store);
}
