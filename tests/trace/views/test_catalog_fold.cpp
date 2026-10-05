#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "test_view_common.h"

namespace {

namespace ix = dftracer::utils::index;
namespace st = dftracer::utils::index::store;

constexpr int RECORDS = 400;

using Catalog = std::map<std::string, std::tuple<int, int, std::uint64_t>>;

std::string write_gz(TestEnvironment& env, const std::string& dir,
                     const std::vector<std::string>& lines) {
    const std::string d = env.get_dir() + "/" + dir;
    fs::create_directories(d);
    const std::string plain = d + "/t.ndjson";
    {
        std::ofstream out(plain);
        for (const auto& l : lines) out << l << "\n";
    }
    const std::string gz = plain + ".gz";
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, 2048));
    fs::remove(plain);
    return gz;
}

std::vector<std::string> dftracer_lines() {
    std::vector<std::string> out;
    for (int i = 0; i < RECORDS; ++i) {
        std::string l =
            R"({"ph":"X","name":")" + std::string(i % 2 ? "read" : "write") +
            R"(","cat":"POSIX","pid":1,"tid":)" + std::to_string(1 + i % 3) +
            R"(,"ts":)" + std::to_string(1000 + i * 10) +
            R"(,"dur":5,"args":{"fhash":"f)" + std::to_string(i % 4) +
            R"(","size":)" + std::to_string(i) + R"(,"opt":)" +
            (i % 5 ? "null" : "1.5") + R"(,"nest":{"a":)" +
            std::to_string(i % 7) + R"(,"b":"x"},"arr":[)" + std::to_string(i) +
            "," + std::to_string(i + 1) + R"(],"hhash":"h","e":{},"z":[]}})";
        if (i % 11 == 0)
            l = l.substr(0, l.find(R"("args":)")) + R"("args":{}})";
        out.push_back(std::move(l));
    }
    return out;
}

std::vector<std::string> log_lines() {
    std::vector<std::string> out;
    for (int i = 0; i < RECORDS; ++i)
        out.push_back(R"({"op":")" + std::string(i % 2 ? "read" : "write") +
                      R"(","took":)" + std::to_string(i % 7) +
                      R"(.5,"worker":"w)" + std::to_string(i % 3) +
                      R"(","status":)" + (i % 11 ? "200" : R"("n/a")") +
                      R"(,"extra":{"k":)" + std::to_string(i) + "}}");
    return out;
}

std::vector<std::string> genesis_lines() {
    std::vector<std::string> out;
    out.push_back(R"({"gtype":"run","run":"ab","app":"laghos","nodes":4})");
    for (int i = 0; i < RECORDS; ++i)
        out.push_back(R"({"gtype":"func","run":"ab","ts":)" +
                      std::to_string(i) + R"(,"v":{"p50":)" +
                      std::to_string(i / 30) + R"(,"p99":)" +
                      std::to_string(i / 30 + 5) + R"(},"count":)" +
                      std::to_string(i % 7) + "}");
    return out;
}

void register_log_schema() {
    ix::register_schema(
        "id: cf_log\n"
        "fields:\n"
        "  op: {type: string}\n"
        "  took: {type: float, role: duration, unit: s}\n"
        "  worker: {type: string, role: entity}\n"
        "  status: {type: json, optional: true}\n",
        "test_catalog_fold");
}

bool has_catalog(const std::string& gz) {
    st::IndexDatabase db(determine_index_path(gz, ""),
                         st::IndexOpenMode::ReadOnly);
    const int fid = db.get_file_info_id(st::internal::get_logical_path(gz));
    return fid >= 0 && db.extension_current(fid, st::IndexExtension::CATALOG);
}

Catalog catalog_of(const std::string& gz) {
    st::IndexDatabase db(determine_index_path(gz, ""),
                         st::IndexOpenMode::ReadOnly);
    const int fid = db.get_file_info_id(st::internal::get_logical_path(gz));
    REQUIRE(fid >= 0);
    Catalog out;
    for (const auto& [path, stat] : db.catalog(fid))
        out[path] = {static_cast<int>(stat.type), stat.seen, stat.count};
    return out;
}

View view_of(const std::string& gz, const std::string& schema = "") {
    View v = View::from_file(gz, determine_index_path(gz, ""));
    return schema.empty() ? v : v.record_schema(schema);
}

void build_full(const std::string& gz, const std::string& schema = "") {
    ix::IndexerOptions o;
    o.checkpoint_size = 2048;
    o.schema = schema;
    ix::Indexer::open({gz}, o).build();
}

void expect_scan_catalog_equals_build(const std::vector<std::string>& lines,
                                      const std::string& schema) {
    TestEnvironment env(10);
    const std::string scanned = write_gz(env, "scanned", lines);
    const std::string built = write_gz(env, "built", lines);
    build_full(built, schema);
    REQUIRE(has_catalog(built));

    (void)view_of(scanned, schema).collect().get();
    REQUIRE(has_catalog(scanned));
    const Catalog want = catalog_of(built);
    CHECK(want.size() >= 5);
    CHECK(catalog_of(scanned) == want);
}

void plant_sentinel(const std::string& gz) {
    st::IndexDatabase db(determine_index_path(gz, ""));
    const int fid = db.get_file_info_id(st::internal::get_logical_path(gz));
    auto w = db.begin_write();
    st::records::put_catalog_path(*w, fid, "zz.sentinel", st::PathStat{});
    w->commit();
}

bool has_sentinel(const std::string& gz) {
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

    TEST_CASE("a filtered first query writes no catalog until a full scan") {
        TestEnvironment env(10);
        const std::string gz = write_gz(env, "f", dftracer_lines());
        (void)view_of(gz).duql(R"(name == "read")").collect().get();
        CHECK_FALSE(has_catalog(gz));
        (void)view_of(gz).collect().get();
        CHECK(has_catalog(gz));
    }

    TEST_CASE("a second collect leaves the catalog alone") {
        TestEnvironment env(10);
        const std::string gz = write_gz(env, "s", dftracer_lines());
        (void)view_of(gz).collect().get();
        REQUIRE(has_catalog(gz));
        {
            st::IndexDatabase db(determine_index_path(gz, ""));
            const int fid =
                db.get_file_info_id(st::internal::get_logical_path(gz));
            auto w = db.begin_write();
            st::records::put_catalog_path(*w, fid, "zz.sentinel",
                                          st::PathStat{});
            w->commit();
        }
        (void)view_of(gz).collect().get();
        CHECK(catalog_of(gz).count("zz.sentinel") == 1);
    }

    TEST_CASE("columns on a fresh trace list every path and scan once") {
        TestEnvironment env(10);
        const std::string gz = write_gz(env, "c", dftracer_lines());
        {
            // A query-built index holds no catalog.
            (void)view_of(gz).duql(R"(name == "read")").collect().get();
            REQUIRE_FALSE(has_catalog(gz));
        }
        const auto cols = view_of(gz).columns();
        REQUIRE(has_catalog(gz));
        const std::string built = write_gz(env, "cb", dftracer_lines());
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
        const std::string have = write_gz(env, "have", dftracer_lines());
        const std::string fresh = write_gz(env, "fresh", dftracer_lines());
        (void)view_of(have).collect().get();
        REQUIRE(has_catalog(have));
        plant_sentinel(have);
        (void)view_of(fresh).duql(R"(name == "read")").collect().get();
        REQUIRE_FALSE(has_catalog(fresh));

        std::vector<ViewFile> files;
        files.push_back({have, determine_index_path(have, "")});
        files.push_back({fresh, determine_index_path(fresh, "")});
        (void)View::from_files(std::move(files)).columns();
        CHECK(has_catalog(fresh));
        CHECK(has_sentinel(have));
    }

    TEST_CASE("columns on a fresh genesis trace list its paths") {
        TestEnvironment env(10);
        const std::string gz = write_gz(env, "g", genesis_lines());
        const std::string built = write_gz(env, "gb", genesis_lines());
        build_full(built);
        REQUIRE(has_catalog(built));
        const auto cols = view_of(gz).columns();
        REQUIRE(has_catalog(gz));
        for (const char* want : {"v.p50", "v.p99", "count"})
            CHECK(std::find(cols.begin(), cols.end(), want) != cols.end());
        CHECK(cols == view_of(built).columns());
    }
}
