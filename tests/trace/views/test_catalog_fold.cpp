// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/tuple.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <tuple>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;

namespace {

constexpr int RECORDS = 400;

using Catalog = dstd::Map<dstd::String, dstd::Tuple<int, int, std::uint64_t>>;

dstd::String write_gz(TestEnvironment& env, const dstd::String& dir,
                      const dstd::Vector<dstd::String>& lines) {
    const dstd::String d = env.dir() + "/" + dir;
    REQUIRE(dstd::fs::create_directories(d).has_value());
    const dstd::String plain = d + "/t.ndjson";
    {
        dftu_utils_test::FileOut out(plain);
        for (const auto& l : lines) out << l << "\n";
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, 2048));
    dstd::fs::remove(plain).discard();
    return gz;
}

dstd::Vector<dstd::String> dftracer_lines() {
    dstd::Vector<dstd::String> out;
    for (int i = 0; i < RECORDS; ++i) {
        dstd::String l =
            R"({"ph":"X","name":")" + dstd::String(i % 2 ? "read" : "write") +
            R"(","cat":"POSIX","pid":1,"tid":)" + dstd::to_string(1 + i % 3) +
            R"(,"ts":)" + dstd::to_string(1000 + i * 10) +
            R"(,"dur":5,"args":{"fhash":"f)" + dstd::to_string(i % 4) +
            R"(","size":)" + dstd::to_string(i) + R"(,"opt":)" +
            (i % 5 ? "null" : "1.5") + R"(,"nest":{"a":)" +
            dstd::to_string(i % 7) + R"(,"b":"x"},"arr":[)" +
            dstd::to_string(i) + "," + dstd::to_string(i + 1) +
            R"(],"hhash":"h","e":{},"z":[]}})";
        if (i % 11 == 0)
            l = l.substr(0, l.find(R"("args":)")) + R"("args":{}})";
        out.push_back(std::move(l));
    }
    return out;
}

dstd::Vector<dstd::String> log_lines() {
    dstd::Vector<dstd::String> out;
    for (int i = 0; i < RECORDS; ++i)
        out.push_back(R"({"op":")" + dstd::String(i % 2 ? "read" : "write") +
                      R"(","took":)" + dstd::to_string(i % 7) +
                      R"(.5,"worker":"w)" + dstd::to_string(i % 3) +
                      R"(","status":)" + (i % 11 ? "200" : R"("n/a")") +
                      R"(,"extra":{"k":)" + dstd::to_string(i) + "}}");
    return out;
}

dstd::Vector<dstd::String> genesis_lines() {
    dstd::Vector<dstd::String> out;
    out.push_back(R"({"gtype":"run","run":"ab","app":"laghos","nodes":4})");
    for (int i = 0; i < RECORDS; ++i)
        out.push_back(R"({"gtype":"func","run":"ab","ts":)" +
                      dstd::to_string(i) + R"(,"v":{"p50":)" +
                      dstd::to_string(i / 30) + R"(,"p99":)" +
                      dstd::to_string(i / 30 + 5) + R"(},"count":)" +
                      dstd::to_string(i % 7) + "}");
    return out;
}

void register_log_schema() {
    dftu_utils_test::ok_ref(index::register_schema(
        "id: cf_log\n"
        "fields:\n"
        "  op: {type: string}\n"
        "  took: {type: float, role: duration, unit: s}\n"
        "  worker: {type: string, role: entity}\n"
        "  status: {type: json, optional: true}\n",
        "test_catalog_fold"));
}

bool has_catalog(const dstd::String& gz) {
    const auto snapshot = dftu_utils_test::index_snapshot(
        dstd::String(determine_index_path(gz, "")));
    const auto file = dftu_utils_test::find_indexed_file(snapshot, gz);
    return file && index::extension_current(
                       snapshot, *file, index::store::IndexExtension::Catalog);
}

Catalog catalog_of(const dstd::String& gz) {
    const auto snapshot = dftu_utils_test::index_snapshot(
        dstd::String(determine_index_path(gz, "")));
    const auto file = dftu_utils_test::require_indexed_file(snapshot, gz);
    Catalog out;
    for (const auto& [path, stat] :
         dftu_utils_test::index_take(index::catalog(snapshot, file)))
        out[dstd::String(path.data(), path.size())] = {
            static_cast<int>(stat.type), stat.seen, stat.count};
    return out;
}

View view_of(const dstd::String& gz, const dstd::String& schema = "") {
    View v = View::from_file(
        dstd::String(gz),
        dstd::String(dstd::String(determine_index_path(gz, ""))));
    return schema.empty() ? v : v.record_schema(dstd::String(schema));
}

void build_full(const dstd::String& gz, const dstd::String& schema = "") {
    index::IndexerOptions o;
    o.checkpoint_size = 2048;
    o.schema = schema;
    dftu_utils_test::ok(
        dftu_utils_test::ok(
            index::Indexer::open({dstd::String(dstd::StringView(gz))}, o))
            .build());
}

void expect_scan_catalog_equals_build(const dstd::Vector<dstd::String>& lines,
                                      const dstd::String& schema) {
    TestEnvironment env(10);
    const dstd::String scanned = write_gz(env, "scanned", lines);
    const dstd::String built = write_gz(env, "built", lines);
    build_full(built, schema);
    REQUIRE(has_catalog(built));

    (void)view_of(scanned, schema).collect().get();
    REQUIRE(has_catalog(scanned));
    const Catalog want = catalog_of(built);
    CHECK(want.size() >= 5);
    CHECK(catalog_of(scanned) == want);
}

// Rewrites the file's catalog with one more path, as the build wrote it.
void plant_sentinel(const dstd::String& gz) {
    const auto index = dftu_utils_test::open_test_index(
        dstd::String(determine_index_path(gz, "")));
    const auto snapshot = dftu_utils_test::index_take(index.snapshot());
    const auto file = dftu_utils_test::require_indexed_file(snapshot, gz);
    const auto state = index::extension_state(
        snapshot, file, index::store::IndexExtension::Catalog);
    REQUIRE(state.has_value());
    auto build = dftu_utils_test::rebuild_test_file(index, gz);
    for (const auto& [path, stat] :
         dftu_utils_test::index_take(index::catalog(snapshot, file)))
        index::put_catalog_path(build, path, stat);
    index::put_catalog_path(build, "zz.sentinel", index::store::PathStat{});
    build.finish(index::store::IndexExtension::Catalog, state->params_hash);
    dftu_utils_test::commit_test_build(build);
}

bool has_sentinel(const dstd::String& gz) {
    return catalog_of(gz).count("zz.sentinel") == 1;
}

}  // namespace

TEST_SUITE("CatalogFold") {
    TEST_CASE("one collect of a dftracer trace writes the full build catalog") {
        expect_scan_catalog_equals_build(dftracer_lines(), "");
    }

    TEST_CASE("one collect of a path trace writes the full build catalog") {
        register_log_schema();
        expect_scan_catalog_equals_build(log_lines(), "cf_log");
    }

    TEST_CASE("a scan catalog lists the paths of lines a DOM parse refuses") {
        register_log_schema();
        TestEnvironment env(10);
        auto lines = log_lines();
        lines.push_back(
            R"({"op":"x\q","took":1,"worker":"w","extra":{"onlyA":1}})");
        lines.push_back(
            R"({"op":"y","took":1e400,"worker":"w","extra":{"onlyB":1}})");
        lines.push_back(
            dstd::String("{\"op\":\"z\xff\",\"took\":1,\"worker\":\"w\","
                         "\"extra\":{\"onlyC\":1}}"));
        const dstd::String gz = write_gz(env, "r", lines);
        (void)view_of(gz, "cf_log").duql(R"(op == "read")").collect().get();
        REQUIRE_FALSE(has_catalog(gz));
        (void)view_of(gz, "cf_log").collect().get();
        REQUIRE(has_catalog(gz));
        const Catalog got = catalog_of(gz);
        for (const char* path : {"extra.onlyA", "extra.onlyB", "extra.onlyC"}) {
            CAPTURE(path);
            CHECK(got.count(path) == 1);
        }
    }

    TEST_CASE("a filtered first query writes no catalog until a full scan") {
        TestEnvironment env(10);
        const dstd::String gz = write_gz(env, "f", dftracer_lines());
        (void)view_of(gz).duql(R"(name == "read")").collect().get();
        CHECK_FALSE(has_catalog(gz));
        (void)view_of(gz).collect().get();
        CHECK(has_catalog(gz));
    }

    TEST_CASE("a second collect leaves the catalog alone") {
        TestEnvironment env(10);
        const dstd::String gz = write_gz(env, "s", dftracer_lines());
        (void)view_of(gz).collect().get();
        REQUIRE(has_catalog(gz));
        plant_sentinel(gz);
        (void)view_of(gz).collect().get();
        CHECK(catalog_of(gz).count("zz.sentinel") == 1);
    }

    TEST_CASE("columns on a fresh trace list every path and scan once") {
        TestEnvironment env(10);
        const dstd::String gz = write_gz(env, "c", dftracer_lines());
        {
            // A query-built index holds no catalog.
            (void)view_of(gz).duql(R"(name == "read")").collect().get();
            REQUIRE_FALSE(has_catalog(gz));
        }
        const auto cols = view_of(gz).columns();
        REQUIRE(has_catalog(gz));
        const dstd::String built = write_gz(env, "cb", dftracer_lines());
        build_full(built);
        CHECK(cols.size() > 7);
        CHECK(cols == view_of(built).columns());

        // A rewrite would drop the sentinel.
        plant_sentinel(gz);
        CHECK(view_of(gz).columns() == view_of(gz).columns());
        CHECK(has_sentinel(gz));
    }

    TEST_CASE("only the file without a catalog is scanned") {
        TestEnvironment env(10);
        const dstd::String have = write_gz(env, "have", dftracer_lines());
        const dstd::String fresh = write_gz(env, "fresh", dftracer_lines());
        (void)view_of(have).collect().get();
        REQUIRE(has_catalog(have));
        plant_sentinel(have);
        (void)view_of(fresh).duql(R"(name == "read")").collect().get();
        REQUIRE_FALSE(has_catalog(fresh));

        dstd::Vector<ViewFile> files;
        files.push_back({dstd::String(have), determine_index_path(have, "")});
        files.push_back({dstd::String(fresh), determine_index_path(fresh, "")});
        (void)View::from_files(std::move(files)).columns();
        CHECK(has_catalog(fresh));
        CHECK(has_sentinel(have));
    }

    TEST_CASE("columns on a fresh genesis trace list its paths") {
        TestEnvironment env(10);
        const dstd::String gz = write_gz(env, "g", genesis_lines());
        const dstd::String built = write_gz(env, "gb", genesis_lines());
        build_full(built);
        REQUIRE(has_catalog(built));
        const auto cols = view_of(gz).columns();
        REQUIRE(has_catalog(gz));
        for (const char* want : {"v.p50", "v.p99", "count"})
            CHECK(std::find(cols.begin(), cols.end(), want) != cols.end());
        CHECK(cols == view_of(built).columns());
    }
}
