#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/trace/views/typed_rows.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "test_view_common.h"

namespace {

using dftracer::utils::index::Field;
using dftracer::utils::index::Json;

constexpr int RECORDS = 3000;

struct Access {
    Field<std::string, "op"> op;
    Field<std::int64_t, "lat"> lat;
    Field<std::optional<std::int64_t>, "io.off"> off;
    Field<std::optional<Json>, "tags"> tags;
    Field<std::optional<std::string>, "hosts.1"> host;
};

struct Event {
    Field<std::string, "name"> name;
    Field<double, "ts"> ts;
    Field<std::optional<std::int64_t>, "args.size"> size;
};

// Record i: op read on even i, lat i, io.off 10*i on multiples of 3, tags on
// multiples of 5, and hosts on multiples of 7.
std::string write_access(TestEnvironment& env, const std::string& extra) {
    const std::string plain = env.get_dir() + "/access.ndjson";
    {
        std::ofstream out(plain);
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
    const std::string gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    fs::remove(plain);
    dftracer::utils::index::Indexer::open({gz}).build();
    return gz;
}

View view_of(const std::string& gz) {
    return View::from_file(gz, determine_index_path(gz, ""));
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
            if (i % 7 == 0) CHECK(**r.host == "h" + std::to_string(i));
        }

        const auto df = view_of(gz).collect().get();
        REQUIRE(df.num_rows() == RECORDS);
        std::vector<std::pair<std::string, std::int64_t>> want, have;
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
        const std::string pfw = env.get_dir() + "/meta.pfw";
        {
            std::ofstream ofs(pfw);
            ofs << R"({"ph":"M","name":"process_name","pid":1,"tid":1,)"
                   R"("args":{"name":"app"}})"
                << "\n";
            for (int i = 0; i < 20; ++i)
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,)"
                       R"("tid":1,"ts":)"
                    << 1000 + i << R"(,"dur":5,"args":{"size":)" << i << "}}\n";
        }
        const std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        fs::remove(pfw);
        const auto events = run(rows<Event>(view_of(gz)));
        REQUIRE(events.size() == 20);
        for (const Event& e : events) {
            CHECK(*e.name == "read");
            CHECK(e.size->has_value());
        }
        const auto any = view_of(gz).phase(Phase::Any);
        CHECK(run(rows<Event>(any)).size() == any.collect().get().num_rows());
        const auto meta = run(rows<Event>(view_of(gz).phase(Phase::Metadata)));
        REQUIRE(meta.size() == 1);
        CHECK(*meta[0].name == "process_name");
    }
}
