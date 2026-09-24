#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/condition.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>

#include <string>

#include "test_view_common.h"

namespace {

constexpr int FILES = 6000;
constexpr int EVENTS = 2 * FILES;

std::string path_of(int f) { return "/data/f" + std::to_string(f); }

// FH/HH/SH dictionary records, a start event keyed to its executable, then
// two reads per file in file order, so each chunk holds its own files.
std::string write_trace(TestEnvironment& env) {
    const std::string plain = env.get_dir() + "/r.pfw";
    {
        std::ofstream out(plain);
        for (int f = 0; f < FILES; ++f)
            out << R"({"name":"FH","ph":"M","pid":1,"tid":1,"args":{"name":")"
                << path_of(f) << R"(","value":"k)" << f << "\"}}\n";
        out << R"({"name":"HH","ph":"M","pid":1,"tid":1,"args":{"name":"node0","value":"h0"}})"
            << "\n";
        out << R"({"name":"SH","ph":"M","pid":1,"tid":1,"args":{"name":"/opt/laghos","value":"e0"}})"
            << "\n";
        out << R"({"name":"start","cat":"dftracer","ph":"X","pid":1,"tid":1,"ts":1,"dur":0,"args":{"exec_hash":"e0"}})"
            << "\n";
        for (int i = 0; i < EVENTS; ++i)
            out << R"({"name":"read","cat":"POSIX","ph":"X","pid":1,"tid":1,"ts":)"
                << 10 + i << R"(,"dur":1,"args":{"fhash":"k)" << i / 2
                << R"(","hhash":"h0"}})" << "\n";
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

TEST_SUITE("ResolvedView") {
    TEST_CASE(
        "resolved columns filter, select and group through dictionaries") {
        TestEnvironment env(10);
        const auto gz = write_trace(env);

        auto rows =
            view_of(gz)
                .query(R"(resolved.fhash.path == "/data/f10")")
                .select({"name", "resolved.fhash.path", "resolved.hhash.name"})
                .collect()
                .get();
        REQUIRE(rows.num_rows() == 2);
        for (std::int64_t r = 0; r < 2; ++r) {
            CHECK(bstr(rows, r, "resolved.fhash.path") == "/data/f10");
            CHECK(bstr(rows, r, "resolved.hhash.name") == "node0");
        }

        auto grouped =
            view_of(gz)
                .query(R"(resolved.fhash.path == "/data/f10")")
                .group_by({GroupKey::resolved("resolved.fhash.path")})
                .agg({{AggOp::Count, "", "n"}})
                .collect()
                .get();
        REQUIRE(grouped.num_rows() == 1);
        CHECK(bstr(grouped, 0, "resolved.fhash.path") == "/data/f10");
        CHECK(bnum(grouped, 0, "n") == 2);

        auto exec = view_of(gz)
                        .query(R"(resolved.exec_hash.value like "%laghos%")")
                        .collect()
                        .get();
        REQUIRE(exec.num_rows() == 1);
        CHECK(bstr(exec, 0, "name") == "start");
    }

    TEST_CASE("a selective path prunes and a wide pattern does not") {
        TestEnvironment env(10);
        const auto gz = write_trace(env);
        const auto ix = dftracer::utils::index::Indexer::open({gz});

        const auto narrow = ix.explain(R"(resolved.fhash.path == "/data/f10")");
        REQUIRE(narrow.size() == 1);
        REQUIRE(narrow[0].chunks > 4);
        CHECK(narrow[0].read.size() < narrow[0].chunks);

        const std::string wide = R"(resolved.fhash.path like "/data/f%")";
        static_assert(FILES > dftracer::utils::index::plan::SEMI_JOIN_CAP);
        const auto e = ix.explain(wide);
        REQUIRE(e.size() == 1);
        CHECK(e[0].read.size() == e[0].chunks);
        for (const auto& x : e[0].extensions) {
            CAPTURE(x.name);
            CHECK_FALSE(x.file_ruled_out);
            CHECK(x.removed.empty());
        }
        CHECK(view_of(gz).query(wide).collect().get().num_rows() == EVENTS);
        CHECK(view_of(gz)
                  .query(R"(fhash like "k%")")
                  .collect()
                  .get()
                  .num_rows() == EVENTS);
    }

    TEST_CASE("an old resolved name fails and names the new ones") {
        TestEnvironment env(10);
        const auto gz = write_trace(env);
        try {
            view_of(gz)
                .query(R"(resolved.fpath == "/data/f10")")
                .collect()
                .get();
            FAIL("no error");
        } catch (const std::exception& e) {
            CAPTURE(e.what());
            CHECK(std::string(e.what()).find("resolved.fhash.path") !=
                  std::string::npos);
        }
    }
}
