#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::utilities::fileio::parallel;

namespace {

std::string tmp_path(const char* name) {
    return (fs::temp_directory_path() / name).string();
}

std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

ByteView sv(const std::string& s) { return ByteView(s.data(), s.size()); }

}  // namespace

TEST_CASE("StripedWriter - header, chunks, footer land in single file") {
    std::string path = tmp_path("test_striped_basic.txt");
    std::remove(path.c_str());

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        auto w = make_striped_writer();
        if (co_await w->open(path, 4, false, nullptr) != 0) co_return 1;
        if (co_await w->write_header(sv("HDR\n")) != 0) co_return 2;
        if (co_await w->write_chunk(0, sv("worker0\n")) != 0) co_return 3;
        if (co_await w->write_chunk(1, sv("worker1\n")) != 0) co_return 4;
        if (co_await w->write_chunk(2, sv("worker2\n")) != 0) co_return 5;
        if (co_await w->write_footer(sv("END\n")) != 0) co_return 6;
        if (co_await w->close() != 0) co_return 7;
        co_return 0;
    };

    CHECK(runtime.submit(task(), "striped_basic").get() == 0);

    auto content = read_all(path);
    // All bytes must be present regardless of interleave order.
    CHECK(content.size() ==
          std::string("HDR\nworker0\nworker1\nworker2\nEND\n").size());

    std::set<std::string> lines;
    std::stringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) lines.insert(line);
    CHECK(lines.count("HDR") == 1);
    CHECK(lines.count("worker0") == 1);
    CHECK(lines.count("worker1") == 1);
    CHECK(lines.count("worker2") == 1);
    CHECK(lines.count("END") == 1);

    fs::remove(path);
}

TEST_CASE("StripedWriter - empty chunks are no-ops") {
    std::string path = tmp_path("test_striped_empty.txt");
    std::remove(path.c_str());

    Runtime runtime(2);
    auto task = [&]() -> CoroTask<int> {
        auto w = make_striped_writer();
        if (co_await w->open(path, 2, false, nullptr) != 0) co_return 1;
        if (co_await w->write_chunk(0, ByteView()) != 0) co_return 2;
        if (co_await w->write_chunk(1, ByteView()) != 0) co_return 3;
        if (co_await w->close() != 0) co_return 4;
        co_return 0;
    };
    CHECK(runtime.submit(task(), "striped_empty").get() == 0);
    CHECK(fs::exists(path));
    CHECK(fs::file_size(path) == 0);
    fs::remove(path);
}

TEST_CASE("StripedWriter - output_paths returns single entry") {
    std::string path = tmp_path("test_striped_paths.txt");
    std::remove(path.c_str());

    Runtime runtime(2);
    std::vector<std::string> paths;
    auto task = [&]() -> CoroTask<int> {
        auto w = make_striped_writer();
        if (co_await w->open(path, 4, false, nullptr) != 0) co_return 1;
        paths = w->output_paths();
        co_await w->close();
        co_return 0;
    };
    CHECK(runtime.submit(task(), "striped_paths").get() == 0);
    REQUIRE(paths.size() == 1);
    CHECK(paths[0] == path);
    fs::remove(path);
}
