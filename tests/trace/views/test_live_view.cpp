// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/trace/views/live_view.h>
#include <task_result.h>
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

dstd::String gzip_member(dstd::String text) {
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

dstd::String member(int first) {
    dstd::String text;
    for (int i = first; i < first + ROWS; ++i) {
        text += common::str_cat(
            R"({"id":)", i,
            R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
            1000 + i * 10, R"(,"dur":5,"ph":"X"})", "\n");
    }
    return gzip_member(std::move(text));
}

dstd::Map<dstd::String, std::uint64_t> leaf_counts(const View& v) {
    dstd::Map<dstd::String, std::uint64_t> out;
    for (const auto& leaf : dftu_utils_test::ok(v.schema_tree()))
        out[leaf.path] = leaf.count.value_or(0);
    return out;
}

dstd::Map<std::int64_t, std::int64_t> bucket_counts(const View& v) {
    const auto df = dftu_utils_test::ok(
        v.time_bucket_min(100).agg({{AggOp::Count, "", "n"}}).collect().get());
    dstd::Map<std::int64_t, std::int64_t> out;
    const auto& b = df.columns[df.column_index("time_bucket")];
    const auto& n = df.columns[df.column_index("n")];
    for (std::int64_t i = 0; i < n.length(); ++i)
        out[to_number<long long>(dstd::String(b.string_at(i)))] +=
            n.values<std::int64_t>()[i];
    return out;
}

void put(const dstd::String& path, const dstd::String& bytes) {
    REQUIRE(dstd::fs::write_file(path, bytes).has_value());
}

View snap(LiveView& live) { return dftu_utils_test::ok(live.snapshot()); }

dstd::Optional<View> rows(LiveView& live) {
    return dftu_utils_test::ok(live.new_rows());
}

std::int64_t count(const View& v) {
    return static_cast<std::int64_t>(
        dftu_utils_test::ok(v.collect().get()).num_rows());
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
    const auto df = dftu_utils_test::ok(counted.group_by({GroupKey::cat()})
                                            .agg({{AggOp::Count, "", "n"}})
                                            .collect()
                                            .get());
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
        CHECK(count(snap(live)) == ROWS);
        CHECK(verified(live) == m0.size());
        CHECK(skipped(live) == m1.size() / 2);
        CHECK(live.status()[0].state != LiveFileState::Hole);

        put(gz, m0 + m1);
        CHECK(count(snap(live)) == 2 * ROWS);
        CHECK(skipped(live) == 0);
    }

    TEST_CASE("a hole stops the read and a filled hole resumes it") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0), m1 = member(100), m2 = member(200);
        put(gz, m0 + dstd::String(m1.size(), '\0') + m2);

        LiveView live({gz});
        CHECK(count(snap(live)) == ROWS);
        REQUIRE(live.status().size() == 1);
        CHECK(live.status()[0].path == gz);
        CHECK(live.status()[0].state == LiveFileState::Hole);
        CHECK(live.status()[0].verified_bytes == m0.size());
        CHECK(live.status()[0].hole_bytes == m1.size());

        put(gz, m0 + m1 + m2);
        CHECK(count(snap(live)) == 3 * ROWS);
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
        CHECK(count(snap(live)) == ROWS);
        CHECK(verified(live) == m0.size());
        CHECK(skipped(live) == bad.size());
    }

    TEST_CASE("a replaced file drops the rows of the old one") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        put(gz, member(0) + member(100));
        LiveView live({gz});
        CHECK(count(snap(live)) == 2 * ROWS);

        const dstd::String other = member(500);
        put(gz, other.substr(0, other.size() / 2));
        CHECK(count(snap(live)) == 0);
        CHECK(verified(live) == 0);

        put(gz, other);
        CHECK(count(snap(live)) == ROWS);
    }

    TEST_CASE("a directory is scanned again and its new files join") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir() + "/traces";
        REQUIRE(dstd::fs::create_directories(dir + "/rank0").has_value());
        put(dir + "/rank0/a.pfw.gz", member(0));
        // Inside the traces, so a rescan must not read the snapshots back.
        LiveView live({dir});
        CHECK(count(snap(live)) == ROWS);

        put(dir + "/b-spill.pfw.gz", member(100));
        CHECK(count(snap(live)) == 2 * ROWS);
        CHECK(count(snap(live)) == 2 * ROWS);
    }

    TEST_CASE("several new files are all read") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String a = env.dir() + "/a.pfw.gz";
        const dstd::String b = env.dir() + "/b.pfw.gz";
        put(a, member(0));
        put(b, member(100));
        LiveView live({a, b});
        CHECK(count(snap(live)) == 2 * ROWS);
    }

    TEST_CASE("a named file that does not exist is an Io error") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        LiveView live({env.dir() + "/absent.pfw.gz"});
        expect_error(live.snapshot(), common::ErrorCode::Io);
    }

    TEST_CASE("an old view keeps its rows after the file grows") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0), m1 = member(100);
        put(gz, m0);
        LiveView live({gz});
        const View first = snap(live);
        CHECK(count(first) == ROWS);
        const auto current = dstd::make_shared<ScanCounter>();
        CHECK(total_rows(first, current) == ROWS);
        CHECK(current->chunks_scanned.load() == 0);

        put(gz, m0 + m1);
        const View second = snap(live);
        CHECK(count(second) == 2 * ROWS);
        const auto newest = dstd::make_shared<ScanCounter>();
        CHECK(total_rows(second, newest) == 2 * ROWS);
        CHECK(newest->chunks_scanned.load() == 0);

        CHECK(count(first) == ROWS);
        const auto pinned = dstd::make_shared<ScanCounter>();
        CHECK(total_rows(first, pinned) == ROWS);
        CHECK(pinned->chunks_scanned.load() == 0);
    }

    TEST_CASE("a view with no whole member yet reads nothing") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0);
        put(gz, m0.substr(0, m0.size() / 2));
        LiveView live({gz});
        CHECK(count(snap(live)) == 0);
        REQUIRE(live.status().size() == 1);
        CHECK(live.status()[0].verified_bytes == 0);
        CHECK(live.status()[0].state == LiveFileState::Tail);
        put(gz, m0);
        CHECK(count(snap(live)) == ROWS);
        CHECK(live.status()[0].state == LiveFileState::Whole);
    }

    TEST_CASE("new rows come once, whatever the snapshots between them") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0), m1 = member(100), m2 = member(200);
        put(gz, m0);
        LiveView live({gz});
        auto first = rows(live);
        REQUIRE(first.has_value());
        CHECK(count(*first) == ROWS);
        CHECK_FALSE(rows(live).has_value());

        put(gz, m0 + m1);
        CHECK(count(snap(live)) == 2 * ROWS);
        auto second = rows(live);
        REQUIRE(second.has_value());
        CHECK(count(*second) == ROWS);
        CHECK(total_rows(*second) == ROWS);
        CHECK_FALSE(rows(live).has_value());

        put(gz, m0 + m1 + m2.substr(0, m2.size() / 2));
        CHECK_FALSE(rows(live).has_value());
        put(gz, m0 + m1 + m2);
        auto third = rows(live);
        REQUIRE(third.has_value());
        CHECK(count(*third) == ROWS);

        CHECK(count(*first) == ROWS);
        CHECK(count(*second) == ROWS);
        CHECK(count(snap(live)) == 3 * ROWS);
    }

    TEST_CASE("new rows keep a line that ends in a later member") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0);
        const dstd::String line =
            R"({"id":99,"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":9000,"dur":5,"ph":"X"})";
        const dstd::String open = gzip_member(line.substr(0, 20));
        const dstd::String rest = gzip_member(line.substr(20) + "\n");
        put(gz, m0);
        LiveView live({gz});
        REQUIRE(rows(live).has_value());

        put(gz, m0 + open);
        CHECK_FALSE(rows(live).has_value());
        put(gz, m0 + open + rest);
        auto later = rows(live);
        REQUIRE(later.has_value());
        CHECK(count(*later) == 1);
        CHECK_FALSE(rows(live).has_value());
    }

    TEST_CASE("new rows start over when a new file replaces the old one") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0), m1 = member(100);
        const dstd::String a = member(300), b = member(400), c = member(500);
        put(gz, m0 + m1);
        LiveView live({gz});
        REQUIRE(rows(live).has_value());

        put(gz, a);
        auto shorter = rows(live);
        REQUIRE(shorter.has_value());
        CHECK(count(*shorter) == ROWS);

        put(gz, b + c + a);
        auto longer = rows(live);
        REQUIRE(longer.has_value());
        CHECK(count(*longer) == 3 * ROWS);
    }

    TEST_CASE("new rows hold a file that joins a directory") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String dir = env.dir() + "/traces";
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        put(dir + "/a.pfw.gz", member(0));
        LiveView live({dir});
        REQUIRE(rows(live).has_value());

        put(dir + "/b.pfw.gz", member(100));
        auto joined = rows(live);
        REQUIRE(joined.has_value());
        CHECK(count(*joined) == ROWS);
    }

    TEST_CASE("new rows share no cached DUQL side with a whole view") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0);
        dstd::String late;
        for (int i = 0; i < ROWS; ++i) {
            late += common::str_cat(
                R"({"id":)", 100 + i,
                R"(,"name":"late","cat":"POSIX","pid":1,"tid":1,"ts":)",
                5000 + i * 10, R"(,"dur":5,"ph":"X"})", "\n");
        }
        const char* query = "where name in (from data | select name)";
        put(gz, m0);
        LiveView live({gz});
        REQUIRE(rows(live).has_value());
        put(gz, m0 + gzip_member(std::move(late)));
        auto fresh = rows(live);
        REQUIRE(fresh.has_value());
        const View whole = snap(live);

        CHECK(count(fresh->duql(query)) == ROWS);
        CHECK(count(whole.duql(query)) == 2 * ROWS);
        CHECK(count(fresh->duql(query)) == ROWS);
    }

    TEST_CASE("an old view keeps its schema counts and paths") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        const dstd::String m0 = member(0);
        dstd::String late;
        for (int i = 0; i < ROWS; ++i) {
            late += common::str_cat(
                R"({"id":)", 100 + i,
                R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
                5000 + i * 10, R"(,"dur":5,"ph":"X","args":{"late":1}})", "\n");
        }
        put(gz, m0);
        LiveView live({gz});
        const View old = snap(live);
        put(gz, m0 + gzip_member(std::move(late)));
        const View newest = snap(live);

        const auto old_leaves = leaf_counts(old);
        CHECK(old_leaves.at("name") == ROWS);
        CHECK(old_leaves.count("args.late") == 0);
        const auto new_leaves = leaf_counts(newest);
        CHECK(new_leaves.at("name") == 2 * ROWS);
        CHECK(new_leaves.at("args.late") == ROWS);
    }

    TEST_CASE("an old view of path records keeps its bucket origin") {
        dftu_utils_test::ok_ref(index::register_schema(R"(
id: live_path_roles
fields:
  start: {type: int, role: time}
  took: {type: int, role: duration}
  op: {type: string, role: name}
)",
                                                       "test"));
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.jsonl.gz";
        const auto records = [](int start) {
            dstd::String text;
            for (int i = 0; i < ROWS; ++i)
                text += common::str_cat(R"({"start":)", start + i * 10,
                                        R"(,"took":5,"op":"read"})", "\n");
            return gzip_member(std::move(text));
        };
        const dstd::String m0 = records(1000);
        put(gz, m0);
        index::IndexerOptions options;
        options.schema = "live_path_roles";
        dftu_utils_test::ok(
            dftu_utils_test::ok(index::Indexer::open({gz}, options)).build());

        LiveView live({gz});
        const View old = snap(live);
        const auto before = bucket_counts(old);
        CHECK(before.begin()->first == 1000);

        put(gz, m0 + records(7));
        const View newest = snap(live);
        CHECK(bucket_counts(old) == before);
        CHECK(bucket_counts(newest).begin()->first == 7);
    }

    TEST_CASE("readers keep their rows while the index grows") {
        TestEnvironment env(10);
        REQUIRE(env.is_valid());
        const dstd::String gz = env.dir() + "/a.pfw.gz";
        put(gz, member(0));
        LiveView live({gz});
        const View first = snap(live);

        std::atomic<bool> is_done{false};
        std::atomic<std::int64_t> wrong_rows{ROWS};
        std::atomic<double> wrong_counted{ROWS};
        dstd::Thread reader([&] {
            while (!is_done.load()) {
                const std::int64_t rows = count(first);
                if (rows != ROWS) wrong_rows.store(rows);
                const double counted = total_rows(first);
                if (counted != ROWS) wrong_counted.store(counted);
            }
        });
        // Appended, as a tracer writes: a rewrite would empty the file under
        // the reader for a moment.
        for (int step = 1; step <= 12; ++step) {
            REQUIRE(dstd::fs::append_file(gz, member(100 * step)).has_value());
            CHECK(count(snap(live)) == (step + 1) * ROWS);
        }
        is_done.store(true);
        reader.join();
        CHECK(wrong_rows.load() == ROWS);
        CHECK(wrong_counted.load() == ROWS);
    }
}
