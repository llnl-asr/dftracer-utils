// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/trace/views/typed_rows.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <cstdint>
#include <limits>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

constexpr int RECORDS = 3000;

struct Access {
    index::Field<dstd::String, "op"> op;
    index::Field<std::int64_t, "lat"> lat;
    index::Field<dstd::Optional<std::int64_t>, "io.off"> off;
    index::Field<dstd::Optional<index::Json>, "tags"> tags;
    index::Field<dstd::Optional<dstd::String>, "hosts.1"> host;
};

struct Event {
    index::Field<dstd::String, "name"> name;
    index::Field<double, "ts"> ts;
    index::Field<dstd::Optional<std::int64_t>, "args.size"> size;
};

// Record i: op read on even i, lat i, io.off 10*i on multiples of 3, tags on
// multiples of 5, and hosts on multiples of 7.
dstd::String write_access(TestEnvironment& env, const dstd::String& extra) {
    const dstd::String plain = env.dir() + "/access.ndjson";
    {
        dftu_utils_test::FileOut out(plain);
        for (int i = 0; i < RECORDS; ++i) {
            out << R"({"op":")" << (i % 2 ? "write" : "read") << R"(","lat":)"
                << i;
            if (i % 3 == 0) out << R"(,"io":{"off":)" << i * 10 << "}";
            if (i % 5 == 0) out << R"(,"tags":{"b":[1,2],"a":"x"})";
            if (i % 7 == 0) out << R"(,"hosts":["h0","h)" << i << R"("])";
            out << "}\n";
        }
        out << extra;
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    dstd::fs::remove(plain).discard();
    dftracer::utils::index::Indexer::open({dstd::String(dstd::StringView(gz))})
        .build();
    return gz;
}

struct Wide {
    index::Field<std::int64_t, "k"> k;
    index::Field<dstd::Optional<double>, "inf"> inf;
    index::Field<dstd::Optional<std::uint64_t>, "big"> big;
    index::Field<dstd::Optional<dstd::String>, "s"> s;
    index::Field<dstd::Optional<index::Json>, "j"> j;
};

View view_of(const dstd::String& gz) {
    return View::from_file(
        dstd::String(gz),
        dstd::String(dstd::String(determine_index_path(gz, ""))));
}

}  // namespace

TEST_SUITE("TypedRows") {
    TEST_CASE("typed rows equal the collected columns") {
        TestEnvironment env(10);
        const auto gz = write_access(env, "");
        auto got = run(rows<Access>(view_of(gz)));
        REQUIRE(got.size() == RECORDS);
        std::sort(got.begin(), got.end(), [](const Access& a, const Access& b) {
            return *a.lat < *b.lat;
        });
        for (int i = 0; i < RECORDS; ++i) {
            const Access& r = got[static_cast<std::size_t>(i)];
            CAPTURE(i);
            CHECK(*r.lat == i);
            CHECK(*r.op == (i % 2 ? "write" : "read"));
            CHECK(r.off->has_value() == (i % 3 == 0));
            if (i % 3 == 0) CHECK(**r.off == i * 10);
            CHECK(r.tags->has_value() == (i % 5 == 0));
            if (i % 5 == 0) CHECK((*r.tags)->text == R"({"a":"x","b":[1,2]})");
            CHECK(r.host->has_value() == (i % 7 == 0));
            if (i % 7 == 0)
                CHECK(dstd::String(**r.host) == "h" + dstd::to_string(i));
        }

        const auto df = dftu_utils_test::ok(view_of(gz).collect().get());
        REQUIRE(df.num_rows() == RECORDS);
        dstd::Vector<dstd::Pair<dstd::String, std::int64_t>> want, have;
        for (std::int64_t i = 0; i < RECORDS; ++i)
            want.emplace_back(bstr(df, i, "op"),
                              static_cast<std::int64_t>(bnum(df, i, "lat")));
        for (const Access& r : got) have.emplace_back(*r.op, *r.lat);
        std::sort(want.begin(), want.end());
        std::sort(have.begin(), have.end());
        CHECK(want == have);
    }

    TEST_CASE("a value that does not convert keeps the default") {
        TestEnvironment env(10);
        const auto gz = write_access(
            env, R"({"op":7,"lat":"n/a","io":{"off":1.5},"hosts":"h"})"
                 "\n");
        auto got = run(rows<Access>(view_of(gz).duql("lat == \"n/a\"")));
        REQUIRE(got.size() == 1);
        CHECK(got[0].op->empty());
        CHECK(*got[0].lat == 0);
        CHECK_FALSE(got[0].off->has_value());
        CHECK_FALSE(got[0].host->has_value());
    }

    TEST_CASE("filters apply") {
        TestEnvironment env(10);
        const auto gz = write_access(env, "");
        const auto got = run(rows<Access>(view_of(gz).duql(R"(op == "read")")));
        CHECK(got.size() == RECORDS / 2);
        CHECK(std::all_of(got.begin(), got.end(),
                          [](const Access& r) { return *r.op == "read"; }));
    }

    TEST_CASE("dftracer metadata records are left out unless selected") {
        TestEnvironment env(10);
        const dstd::String pfw = env.dir() + "/meta.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            ofs << R"({"ph":"M","name":"process_name","pid":1,"tid":1,)"
                   R"("args":{"name":"app"}})"
                << "\n";
            for (int i = 0; i < 20; ++i)
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,)"
                       R"("tid":1,"ts":)"
                    << 1000 + i << R"(,"dur":5,"args":{"size":)" << i << "}}\n";
        }
        const dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        dstd::fs::remove(pfw).discard();
        const auto events = run(rows<Event>(view_of(gz)));
        REQUIRE(events.size() == 20);
        for (const Event& e : events) {
            CHECK(*e.name == "read");
            CHECK(e.size->has_value());
        }
        const auto any = view_of(gz).phase(Phase::Any);
        CHECK(run(rows<Event>(any)).size() ==
              dftu_utils_test::ok(any.collect().get()).num_rows());
        const auto meta = run(rows<Event>(view_of(gz).phase(Phase::Metadata)));
        REQUIRE(meta.size() == 1);
        CHECK(*meta[0].name == "process_name");
    }

    TEST_CASE("lines with a huge number, a bad escape or bad bytes are kept") {
        TestEnvironment env(10);
        const dstd::String plain = env.dir() + "/wide.ndjson";
        {
            dftu_utils_test::FileOut out(plain);
            out << R"({"k":1,"inf":1e400,"s":"a"})" << "\n";
            out << R"({"k":2,"s":"a\qb"})" << "\n";
            out << "{\"k\":3,\"s\":\"b\xff"
                   "d\"}\n";
            out << R"({"k":4,"big":18446744073709551615,"s":"c"})" << "\n";
            out << R"({"k":5,"inf":-1e400,"s":"d"})" << "\n";
            out << R"({"k":6,"j":{"b":[1e400],"a":"q\q"},"s":"e"})" << "\n";
            out << R"({"k":7,"big":123456789012345678901234567890,"s":"f"})"
                << "\n";
        }
        const dstd::String gz = plain + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                                   1 << 20));
        dstd::fs::remove(plain).discard();
        dftracer::utils::index::Indexer::open(
            {dstd::String(dstd::StringView(gz))})
            .build();
        auto got = run(rows<Wide>(view_of(gz)));
        REQUIRE(got.size() == 7);
        std::sort(got.begin(), got.end(),
                  [](const Wide& a, const Wide& b) { return *a.k < *b.k; });
        CHECK(**got[0].inf == std::numeric_limits<double>::infinity());
        CHECK(**got[1].s == R"(a\qb)");
        CHECK(**got[2].s == "b?d");
        CHECK(**got[3].big == 18446744073709551615ULL);
        CHECK(**got[4].inf == -std::numeric_limits<double>::infinity());
        CHECK((*got[5].j)->text == R"({"b":[1e400],"a":"q\q"})");
        CHECK_FALSE(got[6].big->has_value());
        CHECK(**got[6].s == "f");
    }
}
