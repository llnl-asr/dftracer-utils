// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/views/live_view.h>
#include <trace/views/test_view_common.h>
#include <zlib.h>

#include <atomic>
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

std::uint64_t verified(const LiveView& live) {
    std::uint64_t n = 0;
    for (const auto& st : live.status()) n += st.verified_bytes;
    return n;
}

std::uint64_t skipped(const LiveView& live) {
    std::uint64_t n = 0;
    for (const auto& st : live.status()) n += st.skipped_bytes;
    return n;
}

double total_rows(const View& v,
                  const dstd::SharedPtr<ScanCounter>& counter = nullptr) {
    const View counted = counter ? v.count_scans(counter) : v;
    const auto df = counted.group_by({GroupKey::cat()})
                        .agg({{AggOp::Count, "", "n"}})
                        .collect()
                        .get();
    double n = 0;
    for (std::int64_t r = 0; r < df.num_rows(); ++r) n += bnum(df, r, "n");
    return n;
}

}  // namespace

TEST_SUITE("LiveView") {
    TEST_CASE("an unfinished tail is left out until it is finished") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0), m1 = member(100);
        put(gz, m0 + m1.substr(0, m1.size() / 2));

        LiveView live({gz});
        CHECK(count(live.snapshot()) == ROWS);
        CHECK(verified(live) == m0.size());
        CHECK(skipped(live) == m1.size() / 2);
        CHECK(live.status()[0].state != LiveFileState::Hole);

        put(gz, m0 + m1);
        CHECK(count(live.snapshot()) == 2 * ROWS);
        CHECK(skipped(live) == 0);
    }

    TEST_CASE("a hole stops the read and a filled hole resumes it") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0), m1 = member(100), m2 = member(200);
        put(gz, m0 + dstd::String(m1.size(), '\0') + m2);

        LiveView live({gz});
        CHECK(count(live.snapshot()) == ROWS);
        REQUIRE(live.status().size() == 1);
        CHECK(live.status()[0].path == gz);
        CHECK(live.status()[0].state == LiveFileState::Hole);
        CHECK(live.status()[0].verified_bytes == m0.size());
        CHECK(live.status()[0].hole_bytes == m1.size());

        put(gz, m0 + m1 + m2);
        CHECK(count(live.snapshot()) == 3 * ROWS);
        CHECK(live.status()[0].state != LiveFileState::Hole);
    }

    TEST_CASE("a corrupt member is not read") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0);
        dstd::String bad = member(100);
        bad[bad.size() - 6] ^= 0x55;  // the CRC of the trailer
        put(gz, m0 + bad);

        LiveView live({gz});
        CHECK(count(live.snapshot()) == ROWS);
        CHECK(verified(live) == m0.size());
        CHECK(skipped(live) == bad.size());
    }

    TEST_CASE("a replaced file drops the rows of the old one") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        put(gz, member(0) + member(100));
        LiveView live({gz});
        CHECK(count(live.snapshot()) == 2 * ROWS);

        const dstd::String other = member(500);
        put(gz, other.substr(0, other.size() / 2));
        CHECK(count(live.snapshot()) == 0);
        CHECK(verified(live) == 0);

        put(gz, other);
        CHECK(count(live.snapshot()) == ROWS);
    }

    TEST_CASE("a directory is scanned again and its new files join") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir() + "/traces";
        REQUIRE(dstd::fs::create_directories(dir + "/rank0").has_value());
        put(dir + "/rank0/a.pfw.gz", member(0));
        // Inside the traces, so a rescan must not read the snapshots back.
        LiveView live({dir});
        CHECK(count(live.snapshot()) == ROWS);

        put(dir + "/b-spill.pfw.gz", member(100));
        CHECK(count(live.snapshot()) == 2 * ROWS);
        CHECK(count(live.snapshot()) == 2 * ROWS);
    }

    TEST_CASE("several new files are all read") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String a = env.dir() + "/a.pfw.gz";
        const dstd::String b = env.dir() + "/b.pfw.gz";
        put(a, member(0));
        put(b, member(100));
        LiveView live({a, b});
        CHECK(count(live.snapshot()) == 2 * ROWS);
    }

    TEST_CASE("a named file that does not exist throws") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        LiveView live({env.dir() + "/absent.pfw.gz"});
        CHECK_THROWS(live.snapshot());
    }

    TEST_CASE("an old view keeps its rows after the file grows") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0), m1 = member(100);
        put(gz, m0);
        LiveView live({gz});
        const View first = live.snapshot();
        CHECK(count(first) == ROWS);
        const auto current = dstd::make_shared<ScanCounter>();
        CHECK(total_rows(first, current) == ROWS);
        CHECK(current->chunks_scanned.load() == 0);

        put(gz, m0 + m1);
        const View second = live.snapshot();
        CHECK(count(second) == 2 * ROWS);
        const auto newest = dstd::make_shared<ScanCounter>();
        CHECK(total_rows(second, newest) == 2 * ROWS);
        CHECK(newest->chunks_scanned.load() == 0);

        CHECK(count(first) == ROWS);
        const auto pinned = dstd::make_shared<ScanCounter>();
        CHECK(total_rows(first, pinned) == ROWS);
        CHECK(pinned->chunks_scanned.load() > 0);
    }

    TEST_CASE("a view with no whole member yet reads nothing") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0);
        put(gz, m0.substr(0, m0.size() / 2));
        LiveView live({gz});
        CHECK(count(live.snapshot()) == 0);
        REQUIRE(live.status().size() == 1);
        CHECK(live.status()[0].verified_bytes == 0);
        CHECK(live.status()[0].state == LiveFileState::Tail);
        put(gz, m0);
        CHECK(count(live.snapshot()) == ROWS);
        CHECK(live.status()[0].state == LiveFileState::Whole);
    }

    TEST_CASE("readers keep their rows while the index grows") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        dstd::String bytes = member(0);
        put(gz, bytes);
        LiveView live({gz});
        const View first = live.snapshot();

        std::atomic<bool> is_done{false};
        std::atomic<std::int64_t> wrong{0};
        dstd::Thread reader([&] {
            while (!is_done.load()) {
                if (count(first) != ROWS) wrong.fetch_add(1);
                if (total_rows(first) != ROWS) wrong.fetch_add(1);
            }
        });
        for (int step = 1; step <= 12; ++step) {
            bytes += member(100 * step);
            put(gz, bytes);
            CHECK(count(live.snapshot()) == (step + 1) * ROWS);
        }
        is_done.store(true);
        reader.join();
        CHECK(wrong.load() == 0);
    }
}
