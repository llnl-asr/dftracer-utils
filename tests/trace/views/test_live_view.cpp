// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/views/live_view.h>
#include <trace/views/test_view_common.h>
#include <zlib.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

constexpr int ROWS = 20;

dstd::String member(int first) {
    dstd::String text;
    for (int i = first; i < first + ROWS; ++i) {
        text += common::str_cat(
            R"({"id":)", i,
            R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
            1000 + i * 10, R"(,"dur":5,"ph":"X"})", "\n");
    }
    z_stream zs{};
    REQUIRE(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8,
                         Z_DEFAULT_STRATEGY) == Z_OK);
    dstd::String out(deflateBound(&zs, text.size()) + 32, '\0');
    zs.next_in = reinterpret_cast<Bytef*>(text.data());
    zs.avail_in = static_cast<uInt>(text.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    REQUIRE(deflate(&zs, Z_FINISH) == Z_STREAM_END);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

void put(const dstd::String& path, const dstd::String& bytes) {
    REQUIRE(dstd::fs::write_file(path, bytes).has_value());
}

std::int64_t count(const View& v) {
    return static_cast<std::int64_t>(v.collect().get().num_rows());
}

}  // namespace

TEST_SUITE("LiveView") {
    TEST_CASE("an unfinished tail is left out until it is finished") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0), m1 = member(100);
        put(gz, m0 + m1.substr(0, m1.size() / 2));

        LiveView live({gz}, "", env.dir() + "/snaps");
        CHECK(count(live.refresh()) == ROWS);
        CHECK(live.verified_bytes() == m0.size());
        CHECK(live.skipped_tail_bytes() == m1.size() / 2);
        CHECK(live.holes().empty());

        put(gz, m0 + m1);
        CHECK(count(live.refresh()) == 2 * ROWS);
        CHECK(live.skipped_tail_bytes() == 0);
    }

    TEST_CASE("a hole stops the read and a filled hole resumes it") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0), m1 = member(100), m2 = member(200);
        put(gz, m0 + dstd::String(m1.size(), '\0') + m2);

        LiveView live({gz}, "", env.dir() + "/snaps");
        CHECK(count(live.refresh()) == ROWS);
        const dstd::Vector<LiveHole> holes = live.holes();
        REQUIRE(holes.size() == 1);
        CHECK(holes[0].path == gz);
        CHECK(holes[0].offset == m0.size());
        CHECK(holes[0].length == m1.size());

        put(gz, m0 + m1 + m2);
        CHECK(count(live.refresh()) == 3 * ROWS);
        CHECK(live.holes().empty());
    }

    TEST_CASE("a corrupt member is not read") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0);
        dstd::String bad = member(100);
        bad[bad.size() - 6] ^= 0x55;  // the CRC of the trailer
        put(gz, m0 + bad);

        LiveView live({gz}, "", env.dir() + "/snaps");
        CHECK(count(live.refresh()) == ROWS);
        CHECK(live.verified_bytes() == m0.size());
        CHECK(live.skipped_tail_bytes() == bad.size());
    }

    TEST_CASE("a replaced file drops the rows of the old one") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        put(gz, member(0) + member(100));
        LiveView live({gz}, "", env.dir() + "/snaps");
        CHECK(count(live.refresh()) == 2 * ROWS);

        const dstd::String other = member(500);
        put(gz, other.substr(0, other.size() / 2));
        CHECK(count(live.refresh()) == 0);
        CHECK(live.verified_bytes() == 0);

        put(gz, other);
        CHECK(count(live.refresh()) == ROWS);
    }

    TEST_CASE("a directory is scanned again and its new files join") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir() + "/traces";
        REQUIRE(dstd::fs::create_directories(dir + "/rank0").has_value());
        put(dir + "/rank0/a.pfw.gz", member(0));
        // Inside the traces, so a rescan must not read the snapshots back.
        LiveView live({dir}, "", dir + "/snaps");
        CHECK(count(live.refresh()) == ROWS);

        put(dir + "/b-spill.pfw.gz", member(100));
        CHECK(count(live.refresh()) == 2 * ROWS);
        CHECK(count(live.refresh()) == 2 * ROWS);
    }

    TEST_CASE("several new files are all read") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String a = env.dir() + "/a.pfw.gz";
        const dstd::String b = env.dir() + "/b.pfw.gz";
        put(a, member(0));
        put(b, member(100));
        LiveView live({a, b}, "", env.dir() + "/snaps");
        CHECK(count(live.refresh()) == 2 * ROWS);
    }

    TEST_CASE("a named file that does not exist throws") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        LiveView live({env.dir() + "/absent.pfw.gz"}, "", env.dir() + "/snaps");
        CHECK_THROWS(live.refresh());
    }
}
