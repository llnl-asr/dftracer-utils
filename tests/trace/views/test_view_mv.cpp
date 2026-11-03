// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <doctest/doctest.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

std::string views_root_of(const std::string& gz) {
    return std::string(dftracer::utils::index::views_dir(
        std::string(determine_index_path(gz, ""))));
}

// Every part file of the single MV, including the rank subdirs below parts/.
void collect_parts(const std::string& dir, std::vector<std::string>& parts) {
    for (const auto& e : dftu_utils_test::list_dir(dir)) {
        if (e.kind == dftracer::utils::dstd::fs::FileKind::Directory) {
            collect_parts(std::string(e.path.str()), parts);
            continue;
        }
        const std::string name(e.path.view().filename());
        if (name.size() >= 7 &&
            name.compare(name.size() - 7, 7, ".pfw.gz") == 0)
            parts.push_back(std::string(e.path.str()));
    }
}

// The part files inside the single MV directory under the views root.
std::vector<std::string> mv_parts(const std::string& gz) {
    std::vector<std::string> parts;
    const std::string root = views_root_of(gz);
    for (const auto& e : dftu_utils_test::list_dir(root)) {
        if (e.kind != dftracer::utils::dstd::fs::FileKind::Directory) continue;
        collect_parts(std::string(dftracer::utils::index::view_parts_dir(
                          std::string(e.path.str()))),
                      parts);
    }
    std::sort(parts.begin(), parts.end());
    return parts;
}

// The one MV directory (slug dir) under the views root.
std::string mv_root(const std::string& gz) {
    for (const auto& e : dftu_utils_test::list_dir(views_root_of(gz)))
        if (e.kind == dftracer::utils::dstd::fs::FileKind::Directory)
            return std::string(e.path.str());
    return {};
}

// Size and time of every part file, to tell a rebuild from a reuse.
std::map<std::string, std::string> part_stamps(
    const std::vector<std::string>& parts) {
    std::map<std::string, std::string> out;
    for (const auto& p : parts) {
        const auto stamp = dftracer::utils::dstd::fs::mtime_ns(p).value();
        out[p] =
            std::to_string(dftracer::utils::dstd::fs::file_size(p).value()) +
            ":" + std::to_string(stamp);
    }
    return out;
}

// Data files in the store of one index root.
std::size_t store_files(const std::string& root) {
    std::size_t n = 0;
    for (const auto& e :
         dftu_utils_test::walk_dir(dftracer::utils::index::store_dir(root)))
        if (e.path.view().extension() == ".dfs") ++n;
    return n;
}

std::vector<std::string> sorted_lines(StringSink& s) {
    auto l = s.lines();
    std::sort(l.begin(), l.end());
    return l;
}

// Number of MV directories (slug dirs) under the views root.
std::size_t mv_dir_count(const std::string& gz) {
    std::size_t n = 0;
    for (const auto& e : dftu_utils_test::list_dir(views_root_of(gz)))
        if (e.kind == dftracer::utils::dstd::fs::FileKind::Directory) ++n;
    return n;
}

}  // namespace

TEST_SUITE("ViewMV") {
    TEST_CASE("MV - materialize a row query then serve it on repeat") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);  // 30 read, 20 fwrite
        std::string idx = std::string(determine_index_path(gz, ""));

        auto reads = [&]() {
            return View::from_file(gz, idx).duql(R"(name == "read")");
        };

        StringSink base;
        ExportStats bstats = reads().sink_json(base).get();
        auto want = sorted_lines(base);
        REQUIRE(want.size() == 30);
        CHECK(bstats.events_scanned == 50);  // whole trace scanned

        reads().materialize().get();         // build the filtered-trace MV
        REQUIRE(mv_parts(gz).size() >= 1);

        StringSink again;
        ExportStats astats = reads().sink_json(again).get();
        CHECK(sorted_lines(again) == want);  // same rows
        CHECK(astats.events_scanned == 30);  // only the MV's rows scanned
    }

    TEST_CASE("MV - a narrower query is served by re-filtering the MV") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);
        std::string idx = std::string(determine_index_path(gz, ""));

        // Baseline for the narrow query, before any MV exists.
        StringSink base;
        View::from_file(gz, idx)
            .duql(R"((name == "read") and (dur > 20))")
            .sink_json(base)
            .get();
        auto want = sorted_lines(base);
        REQUIRE(!want.empty());

        // Materialize the broad query, then ask the narrow one.
        View::from_file(gz, idx).duql(R"(name == "read")").materialize().get();

        StringSink narrow;
        ExportStats st = View::from_file(gz, idx)
                             .duql(R"((name == "read") and (dur > 20))")
                             .sink_json(narrow)
                             .get();
        CHECK(sorted_lines(narrow) == want);
        CHECK(st.events_scanned == 30);  // scanned the MV (30 reads), not 50
    }

    TEST_CASE("MV - a non-subsuming query still reads the base") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);
        std::string idx = std::string(determine_index_path(gz, ""));

        View::from_file(gz, idx).duql(R"(name == "read")").materialize().get();

        // fwrite is not in the read-only MV, so this must fall back to the
        // base.
        StringSink other;
        ExportStats st = View::from_file(gz, idx)
                             .duql(R"(name == "fwrite")")
                             .sink_json(other)
                             .get();
        CHECK(other.lines().size() == 20);
        CHECK(st.events_scanned == 50);  // whole base, not the MV
    }

    TEST_CASE("MV - true split rolls into multiple part files") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 300, 0);  // ~300 read events
        std::string idx = std::string(determine_index_path(gz, ""));

        // Tiny checkpoint + part size so the write rolls to many parts.
        View::from_file(gz, idx)
            .duql(R"(name == "read")")
            .materialize(/*checkpoint_size=*/2048, /*part_size=*/2048)
            .get();
        CHECK(mv_parts(gz).size() > 1);

        // All events remain readable across the parts via the shared index.
        StringSink all;
        View::from_file(gz, idx).duql(R"(name == "read")").sink_json(all).get();
        CHECK(all.lines().size() == 300);
    }

    TEST_CASE("MV - a stale view is evicted on read") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);
        std::string idx = std::string(determine_index_path(gz, ""));

        View::from_file(gz, idx).duql(R"(name == "read")").materialize().get();
        REQUIRE(mv_dir_count(gz) == 1);

        // Rewrite the base with clearly different content (new size), so the
        // MV's recorded base identity no longer matches.
        create_mixed_trace(env, 100, 0);

        // A read triggers discovery, which GCs the now-unservable MV.
        StringSink s;
        View::from_file(gz, idx).duql(R"(name == "read")").sink_json(s).get();
        CHECK(mv_dir_count(gz) == 0);
    }

    TEST_CASE("MV - distributed materialize across shards read as one") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        // Two files in their own dirs (so each gets its own index), each with
        // 30 read + 20 fwrite events at a distinct ts range.
        auto write_mixed = [&](const char* sub, int ts0) {
            std::string d = env.get_dir() + "/" + sub;
            REQUIRE(
                dftracer::utils::dstd::fs::create_directories(d).has_value());
            std::string pfw = d + "/trace.pfw";
            std::ofstream ofs(pfw.c_str());
            for (int i = 0; i < 30; ++i)
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,)"
                       R"("tid":1,"ts":)"
                    << (ts0 + i) << R"(,"dur":10,"args":{}})" << "\n";
            for (int i = 0; i < 20; ++i)
                ofs << R"({"ph":"X","name":"fwrite","cat":"STDIO","pid":1,)"
                       R"("tid":1,"ts":)"
                    << (ts0 + 1000 + i) << R"(,"dur":10,"args":{}})" << "\n";
            ofs.close();
            std::string gz = pfw + ".gz";
            dftu_utils_test::compress_file_to_gzip(pfw, gz);
            (void)dftracer::utils::dstd::fs::remove(pfw);
            return gz;
        };
        std::string a = write_mixed("da", 1000);
        std::string b = write_mixed("db", 900000);
        const std::vector<ViewFile> both{
            {a, std::string(determine_index_path(a, ""))},
            {b, std::string(determine_index_path(b, ""))}};
        const std::string vroot = env.get_dir() + "/mv";

        auto full = [&]() {
            return View::from_files(both)
                .duql(R"(name == "read")")
                .views_root(vroot);
        };

        StringSink base;
        ExportStats bstats = full().sink_json(base).get();
        REQUIRE(base.lines().size() == 60);   // 30 read per file
        CHECK(bstats.events_scanned == 100);  // 50 events per file

        // Coordinator picks the shared MV dir; each shard materializes its own
        // file into its own subdir; the coordinator writes the manifest.
        const std::string dir = full().materialize_dir();
        REQUIRE(!dir.empty());
        int r = 0;
        for (const auto& f : both) {
            const std::string sub =
                std::string(dftracer::utils::index::view_parts_dir(dir)) +
                "/shard-" + std::to_string(r++);
            REQUIRE(
                dftracer::utils::dstd::fs::create_directories(sub).has_value());
            TraceWriteOptions opts;
            opts.output_path = sub + "/part.pfw.gz";
            // The view sits outside .dftindex, so each rank indexes its own
            // subdir.
            opts.build_index = true;
            View::from_file(f.file_path, f.index_path)
                .duql(R"(name == "read")")
                .sink_trace(opts)
                .get();
        }
        full().register_materialized(dir);
        // The parts of both ranks sit under the one view root.
        std::vector<std::string> parts;
        collect_parts(std::string(dftracer::utils::index::view_parts_dir(dir)),
                      parts);
        CHECK(parts.size() == 2);

        // The full query now reads the two shard parts, not the base.
        StringSink again;
        ExportStats astats = full().sink_json(again).get();
        CHECK(again.lines().size() == 60);
        CHECK(astats.events_scanned == 60);  // only the shards' read events
        for (int k = 0; k < r; ++k)
            CHECK(dftracer::utils::dstd::fs::exists(
                      dftracer::utils::index::store_dir(
                          std::string(determine_index_path(
                              std::string(
                                  dftracer::utils::index::view_parts_dir(dir)) +
                                  "/shard-" + std::to_string(k),
                              ""))))
                      .value());
    }

    TEST_CASE("MV - a view is its own index root and a repeat query is no-op") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);
        std::string idx = std::string(determine_index_path(gz, ""));

        auto reads = [&]() {
            return View::from_file(gz, idx).duql(R"(name == "read")");
        };
        reads().materialize().get();

        const std::string root = mv_root(gz);
        REQUIRE(!root.empty());
        // <dir>/.dftindex/views/<slug>/{parts,store}
        CHECK(
            dftracer::utils::dstd::PathView(views_root_of(gz)).parent().str() ==
            dftracer::utils::dstd::PathView(idx).str());
        CHECK(dftracer::utils::dstd::PathView(root).parent().str() ==
              dftracer::utils::dstd::PathView(views_root_of(gz)).str());
        CHECK(dftracer::utils::dstd::fs::exists(
                  dftracer::utils::dstd::Path(root) / "parts")
                  .value());
        // The index of the view's parts is the view root itself.
        CHECK(dftracer::utils::dstd::fs::exists(
                  dftracer::utils::dstd::Path(root) / "store")
                  .value());
        CHECK(std::string(determine_index_path(root, "")) == root);
        const auto parts = mv_parts(gz);
        REQUIRE(parts.size() == 1);
        CHECK(dftracer::utils::dstd::PathView(parts.front()).parent().str() ==
              (dftracer::utils::dstd::Path(root) / "parts").str());

        const auto parts_before = part_stamps(parts);
        const std::size_t files_before = store_files(root);
        for (int query = 0; query < 2; ++query) {
            StringSink s;
            const auto st = reads().sink_json(s).get();
            CHECK(s.lines().size() == 30);
            CHECK(st.events_scanned == 30);  // served by the view
        }
        CHECK(part_stamps(parts) == parts_before);
        CHECK(store_files(root) == files_before);
    }
}
