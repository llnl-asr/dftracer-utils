#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::utilities::fileio::parallel;

namespace {

std::string tmp_path(const char* name) {
    return dftu_utils_test::make_unique_test_path(name).string();
}

std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

ByteView sv(const std::string& s) { return ByteView(s.data(), s.size()); }

void cleanup(const std::vector<std::string>& paths) {
    for (const auto& p : paths) std::remove(p.c_str());
}

}  // namespace

TEST_CASE("ShardedWriter - one shard per worker with header/footer placement") {
    std::string base = tmp_path("test_sharded_basic");
    std::vector<std::string> expected = {base + ".shard_0", base + ".shard_1",
                                         base + ".shard_2"};
    cleanup(expected);

    Runtime runtime(2);
    std::vector<std::string> paths;
    auto task = [&]() -> CoroTask<int> {
        auto w = make_sharded_writer();
        if (co_await w->open(base, 3, false, nullptr) != 0) co_return 1;
        if (co_await w->write_header(sv("HDR\n")) != 0) co_return 2;
        if (co_await w->write_chunk(0, sv("A\n")) != 0) co_return 3;
        if (co_await w->write_chunk(1, sv("B\n")) != 0) co_return 4;
        if (co_await w->write_chunk(2, sv("C\n")) != 0) co_return 5;
        if (co_await w->write_footer(sv("END\n")) != 0) co_return 6;
        paths = w->output_paths();
        if (co_await w->close() != 0) co_return 7;
        co_return 0;
    };
    CHECK(runtime.submit(task(), "sharded_basic").get() == 0);

    REQUIRE(paths.size() == 3);
    CHECK(paths == expected);

    CHECK(read_all(paths[0]) == "HDR\nA\n");  // header + worker 0
    CHECK(read_all(paths[1]) == "B\n");
    CHECK(read_all(paths[2]) == "C\nEND\n");  // worker 2 + footer

    cleanup(expected);
}

TEST_CASE("ShardedWriter - gzip_extension appends .gz to shard names") {
    std::string base = tmp_path("test_sharded_gz");
    std::vector<std::string> expected = {base + ".shard_0.gz",
                                         base + ".shard_1.gz"};
    cleanup(expected);

    Runtime runtime(2);
    std::vector<std::string> paths;
    auto task = [&]() -> CoroTask<int> {
        auto w = make_sharded_writer();
        if (co_await w->open(base, 2, true, nullptr) != 0) co_return 1;
        if (co_await w->write_chunk(0, sv("X")) != 0) co_return 2;
        if (co_await w->write_chunk(1, sv("Y")) != 0) co_return 3;
        paths = w->output_paths();
        if (co_await w->close() != 0) co_return 4;
        co_return 0;
    };
    CHECK(runtime.submit(task(), "sharded_gz").get() == 0);
    CHECK(paths == expected);
    for (const auto& p : expected) CHECK(fs::exists(p));
    cleanup(expected);
}

TEST_CASE("ShardedWriter - out-of-range worker_idx fails") {
    std::string base = tmp_path("test_sharded_oor");
    cleanup({base + ".shard_0"});

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        auto w = make_sharded_writer();
        if (co_await w->open(base, 1, false, nullptr) != 0) co_return 1;
        auto rc = co_await w->write_chunk(5, sv("oops"));
        co_await w->close();
        co_return rc == 0 ? 99 : 0;
    };
    CHECK(runtime.submit(task(), "sharded_oor").get() == 0);
    cleanup({base + ".shard_0"});
}
