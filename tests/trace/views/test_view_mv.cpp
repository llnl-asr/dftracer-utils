#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace {

std::string views_root_of(const std::string& gz) {
    return fs::path(gz).parent_path() / ".dftindex-views";
}

// The part files inside the single MV directory under the views root.
std::vector<std::string> mv_parts(const std::string& gz) {
    std::vector<std::string> parts;
    std::error_code ec;
    const std::string root = views_root_of(gz);
    for (fs::directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        for (fs::directory_iterator f(it->path(), ec), fend; !ec && f != fend;
             f.increment(ec)) {
            const std::string name = f->path().filename().string();
            if (name.size() >= 7 &&
                name.compare(name.size() - 7, 7, ".pfw.gz") == 0)
                parts.push_back(f->path().string());
        }
    }
    std::sort(parts.begin(), parts.end());
    return parts;
}

std::vector<std::string> sorted_lines(StringSink& s) {
    auto l = s.lines();
    std::sort(l.begin(), l.end());
    return l;
}

// Number of MV directories (slug dirs) under the views root.
std::size_t mv_dir_count(const std::string& gz) {
    std::size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(views_root_of(gz), ec), end;
         !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) ++n;
    return n;
}

}  // namespace

TEST_SUITE("ViewMV") {
    TEST_CASE("MV - materialize a row query then serve it on repeat") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);  // 30 read, 20 fwrite
        std::string idx = determine_index_path(gz, "");

        auto reads = [&]() {
            return View::from_file(gz, idx).query(R"(name == "read")");
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
        std::string idx = determine_index_path(gz, "");

        // Baseline for the narrow query, before any MV exists.
        StringSink base;
        View::from_file(gz, idx)
            .query(R"((name == "read") and (dur > 20))")
            .sink_json(base)
            .get();
        auto want = sorted_lines(base);
        REQUIRE(!want.empty());

        // Materialize the broad query, then ask the narrow one.
        View::from_file(gz, idx).query(R"(name == "read")").materialize().get();

        StringSink narrow;
        ExportStats st = View::from_file(gz, idx)
                             .query(R"((name == "read") and (dur > 20))")
                             .sink_json(narrow)
                             .get();
        CHECK(sorted_lines(narrow) == want);
        CHECK(st.events_scanned == 30);  // scanned the MV (30 reads), not 50
    }

    TEST_CASE("MV - a non-subsuming query still reads the base") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);
        std::string idx = determine_index_path(gz, "");

        View::from_file(gz, idx).query(R"(name == "read")").materialize().get();

        // fwrite is not in the read-only MV, so this must fall back to the
        // base.
        StringSink other;
        ExportStats st = View::from_file(gz, idx)
                             .query(R"(name == "fwrite")")
                             .sink_json(other)
                             .get();
        CHECK(other.lines().size() == 20);
        CHECK(st.events_scanned == 50);  // whole base, not the MV
    }

    TEST_CASE("MV - true split rolls into multiple part files") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 300, 0);  // ~300 read events
        std::string idx = determine_index_path(gz, "");

        // Tiny checkpoint + part size so the write rolls to many parts.
        View::from_file(gz, idx)
            .query(R"(name == "read")")
            .materialize(/*checkpoint_size=*/2048, /*part_size=*/2048)
            .get();
        CHECK(mv_parts(gz).size() > 1);

        // All events remain readable across the parts via the shared index.
        StringSink all;
        View::from_file(gz, idx)
            .query(R"(name == "read")")
            .sink_json(all)
            .get();
        CHECK(all.lines().size() == 300);
    }

    TEST_CASE("MV - a stale view is evicted on read") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string gz = create_mixed_trace(env, 30, 20);
        std::string idx = determine_index_path(gz, "");

        View::from_file(gz, idx).query(R"(name == "read")").materialize().get();
        REQUIRE(mv_dir_count(gz) == 1);

        // Rewrite the base with clearly different content (new size), so the
        // MV's recorded base identity no longer matches.
        create_mixed_trace(env, 100, 0);

        // A read triggers discovery, which GCs the now-unservable MV.
        StringSink s;
        View::from_file(gz, idx).query(R"(name == "read")").sink_json(s).get();
        CHECK(mv_dir_count(gz) == 0);
    }

    TEST_CASE("MV - distributed materialize across shards read as one") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        // Two files in their own dirs (so each gets its own index), each with
        // 30 read + 20 fwrite events at a distinct ts range.
        auto write_mixed = [&](const char* sub, int ts0) {
            std::string d = env.get_dir() + "/" + sub;
            fs::create_directories(d);
            std::string pfw = d + "/trace.pfw";
            std::ofstream ofs(pfw);
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
            fs::remove(pfw);
            return gz;
        };
        std::string a = write_mixed("da", 1000);
        std::string b = write_mixed("db", 900000);
        const std::vector<ViewFile> both{{a, determine_index_path(a, "")},
                                         {b, determine_index_path(b, "")}};
        const std::string vroot = env.get_dir() + "/mv";

        auto full = [&]() {
            return View::from_files(both)
                .query(R"(name == "read")")
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
            const std::string sub = dir + "/shard-" + std::to_string(r++);
            fs::create_directories(sub);
            TraceWriteOptions opts;
            opts.output_path = sub + "/part.pfw.gz";
            opts.build_index = true;
            View::from_file(f.file_path, f.index_path)
                .query(R"(name == "read")")
                .sink_trace(opts)
                .get();
        }
        full().register_materialized(dir);

        // The full query now reads the two shard parts, not the base.
        StringSink again;
        ExportStats astats = full().sink_json(again).get();
        CHECK(again.lines().size() == 60);
        CHECK(astats.events_scanned == 60);  // only the shards' read events
    }
}
