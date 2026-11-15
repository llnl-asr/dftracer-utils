// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/domains/server/trace_index.h>
#include <dftracer/utils/domains/server/viz/summary_build.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>

using namespace dftracer::utils;
using namespace dftracer::utils::domains::server;

namespace {

dstd::String make_trace(int n) {
    const char* names[] = {"read", "write", "open"};
    const char* cats[] = {"POSIX", "STDIO", "MPI"};
    const int durs[] = {100, 250, 40};
    dstd::String s = "[\n";
    for (int i = 0; i < n; ++i) {
        int k = i % 3;
        s += "{\"id\":" + dstd::to_string(i) + ",\"name\":\"" + names[k] +
             "\",\"cat\":\"" + cats[k] +
             "\",\"pid\":" + dstd::to_string(1 + k) +
             ",\"tid\":1,\"ts\":" + dstd::to_string(1000 + i) +
             ",\"dur\":" + dstd::to_string(durs[k]) + ",\"ph\":\"X\"}\n";
        if (i % 50 == 0) {
            s += "{\"id\":" + dstd::to_string(i) +
                 ",\"name\":\"cpu\",\"cat\":\"sys\",\"pid\":9,\"tid\":1,"
                 "\"ts\":" +
                 dstd::to_string(1000 + i) + ",\"dur\":0,\"ph\":\"C\"}\n";
        }
    }
    return s;
}

std::uint64_t summary_events(const VizSummary& summary) {
    std::uint64_t total = 0;
    for (const auto& row : summary.by_name) total += row.count;
    return total;
}

struct BuiltSummary {
    std::size_t computes = 0;
    std::uint64_t events = 0;
};

BuiltSummary build_and_ensure(const dstd::String& directory,
                              const dstd::String& index_dir) {
    TraceIndex index(directory, index_dir);
    BuiltSummary out;
    Runtime rt(4);
    auto task = coro::with_scope(
        [&](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
            auto initialized = co_await index.initialize();
            if (!initialized) co_return initialized.error();
            auto ensured = co_await ensure_viz_summary(index);
            if (!ensured) co_return ensured.error();
            out.computes = index.viz_summary_compute_count();
            out.events = summary_events(**ensured);
            co_return dstd::Result<void>{};
        });
    dftu_utils_test::ok(rt.submit(std::move(task), "viz_store_cache").wait());
    trace::index::release_all_roots();
    rt.shutdown();
    return out;
}

void write_trace(const dstd::String& path, int n) {
    dftu_utils_test::write_gz_trace(path, make_trace(n));
}

// The index root of `index_dir`, the way the server names it.
dstd::String index_root(const dstd::String& index_dir) {
    return trace::index::normalize_index_root(index_dir);
}

// Every path the index under `root` holds, in path order.
dstd::Vector<dstd::String> indexed_paths(const dstd::String& root) {
    auto index = dftu_utils_test::ok(trace::index::Index::open(root));
    auto snapshot = dftu_utils_test::ok(index.snapshot());
    auto files = dftu_utils_test::ok(snapshot.files());
    dstd::Vector<dstd::String> out;
    out.reserve(files.size());
    for (const auto& file : files) out.emplace_back(file.path());
    trace::index::release_all_roots();
    return out;
}

// Changes the working directory for one test case, so a folder can be named
// relatively. Each test file is a process of its own and doctest runs one case
// at a time, so no other case sees the change.
class WorkingDirectory {
   public:
    explicit WorkingDirectory(const dstd::String& to) {
        const auto here = dstd::fs::current_path();
        REQUIRE(here.has_value());
        here_ = dstd::String(here->str());
        REQUIRE(::chdir(to.c_str()) == 0);
    }
    WorkingDirectory(const WorkingDirectory&) = delete;
    WorkingDirectory& operator=(const WorkingDirectory&) = delete;
    ~WorkingDirectory() { CHECK(::chdir(here_.c_str()) == 0); }

   private:
    dstd::String here_;
};

struct RemovalCase {
    dstd::Path base;
    dstd::Path folder;
    dstd::String index_dir;
    dstd::Path first;
    dstd::Path second;
};

RemovalCase make_removal_case(const dstd::String& tag) {
    RemovalCase c;
    c.base = dftu_utils_test::make_unique_test_path(tag);
    dftu_utils_test::require_ok(dstd::fs::create_directories(c.base.view()),
                                "create_directories");
    c.folder = c.base / "traces";
    dftu_utils_test::require_ok(dstd::fs::create_directories(c.folder.view()),
                                "create_directories");
    c.index_dir = (c.base / "idx").str();
    c.first = c.folder / "a.pfw.gz";
    c.second = c.folder / "b.pfw.gz";
    write_trace(dstd::String(c.first.str()), 12);
    write_trace(dstd::String(c.second.str()), 13);
    return c;
}

// Two traces in `directory` (one root), the second removed, then two restarts.
// The restart after the removal drops the removed trace from the root, so the
// restart after that finds the folder current and re-indexes nothing.
void check_removal(const RemovalCase& c, const dstd::String& directory) {
    const dstd::String root = index_root(c.index_dir);
    const BuiltSummary first = build_and_ensure(directory, c.index_dir);
    REQUIRE(first.computes == 1);
    const auto before = indexed_paths(root);
    REQUIRE(before.size() == 2);
    // The key the first trace already has, whichever way the folder was named.
    dstd::String first_key;
    for (const auto& path : before)
        if (dstd::PathView(path).filename() == "a.pfw.gz") first_key = path;
    REQUIRE(!first_key.empty());

    dftu_utils_test::require_ok(dstd::fs::remove(c.second.view()), "remove");

    const BuiltSummary after_removal = build_and_ensure(directory, c.index_dir);
    CHECK(after_removal.computes == 1);
    const auto kept = indexed_paths(root);
    REQUIRE(kept.size() == 1);
    CHECK(kept.front() == first_key);

    const BuiltSummary second = build_and_ensure(directory, c.index_dir);
    CHECK(second.computes == 0);
    CHECK(second.events == after_removal.events);
    CHECK(indexed_paths(root).size() == 1);
}

}  // namespace

TEST_SUITE("viz_store_cache") {
    TEST_CASE("summary is served from the store on restart without compute") {
        auto dir = dftu_utils_test::make_unique_test_path("viz_store_restart");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        const dstd::String trace = dstd::String((dir / "trace.pfw.gz").str());
        write_trace(trace, 12);

        const BuiltSummary first =
            build_and_ensure(dstd::String(dir.str()), index_dir);
        REQUIRE(first.computes == 1);
        REQUIRE(first.events > 0);

        const BuiltSummary second =
            build_and_ensure(dstd::String(dir.str()), index_dir);
        CHECK(second.computes == 0);
        CHECK(second.events == first.events);
    }

    TEST_CASE("a touched trace file invalidates the stored summary") {
        auto dir = dftu_utils_test::make_unique_test_path("viz_store_touch");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        const dstd::String trace = dstd::String((dir / "trace.pfw.gz").str());
        write_trace(trace, 12);

        const BuiltSummary first =
            build_and_ensure(dstd::String(dir.str()), index_dir);
        REQUIRE(first.computes == 1);

        // The rewrite changes the file size, and the size alone makes the
        // folder stale.
        write_trace(trace, 13);

        const BuiltSummary second =
            build_and_ensure(dstd::String(dir.str()), index_dir);
        CHECK(second.computes == 1);
        CHECK(second.events > first.events);
    }

    TEST_CASE("a removed trace file leaves the index") {
        SUBCASE("the folder is named absolutely") {
            const RemovalCase c = make_removal_case("viz_store_removed_abs");
            check_removal(c, dstd::String(c.folder.str()));
        }
        SUBCASE("the folder is named relatively") {
            const RemovalCase c = make_removal_case("viz_store_removed_rel");
            const WorkingDirectory cwd(dstd::String(c.base.str()));
            check_removal(c, "traces");
        }
    }

    TEST_CASE("one folder stale keeps another folder's summary") {
        auto base =
            dftu_utils_test::make_unique_test_path("viz_store_two_folders");
        REQUIRE(dstd::fs::create_directories(base).has_value());
        const dstd::String dir_a = dstd::String((base / "a").str());
        const dstd::String dir_b = dstd::String((base / "b").str());
        REQUIRE(
            dstd::fs::create_directories(dstd::PathView(dir_a)).has_value());
        REQUIRE(
            dstd::fs::create_directories(dstd::PathView(dir_b)).has_value());
        const dstd::String index_dir = dstd::String((base / "idx").str());
        const dstd::String trace_a =
            dstd::String((dstd::PathView(dir_a) / "trace.pfw.gz").str());
        const dstd::String trace_b =
            dstd::String((dstd::PathView(dir_b) / "trace.pfw.gz").str());
        write_trace(trace_a, 12);
        write_trace(trace_b, 12);

        trace::index::IndexerOptions options;
        options.index_dir = index_dir;
        auto indexer = dftu_utils_test::ok(
            trace::index::Indexer::open({trace_a, trace_b}, options));
        dftu_utils_test::ok(indexer.build());
        trace::index::release_all_roots();

        const BuiltSummary first_a = build_and_ensure(dir_a, index_dir);
        const BuiltSummary first_b = build_and_ensure(dir_b, index_dir);
        REQUIRE(first_a.computes == 1);
        REQUIRE(first_b.computes == 1);
        REQUIRE(first_a.events == first_b.events);

        // The rewrite changes the file size, and the size alone makes folder
        // a stale.
        write_trace(trace_a, 13);

        const BuiltSummary second_a = build_and_ensure(dir_a, index_dir);
        CHECK(second_a.computes == 1);
        CHECK(second_a.events > first_a.events);

        const BuiltSummary second_b = build_and_ensure(dir_b, index_dir);
        CHECK(second_b.computes == 0);
        CHECK(second_b.events == first_b.events);
    }

    TEST_CASE("a subfolder is a trace folder of its own") {
        auto base =
            dftu_utils_test::make_unique_test_path("viz_store_subfolder");
        REQUIRE(dstd::fs::create_directories(base).has_value());
        const dstd::String parent = dstd::String(base.str());
        const dstd::String sub = dstd::String((base / "sub").str());
        REQUIRE(dstd::fs::create_directories(dstd::PathView(sub)).has_value());
        const dstd::String index_dir = dstd::String((base / "idx").str());
        const dstd::String parent_trace =
            dstd::String((base / "trace.pfw.gz").str());
        const dstd::String sub_trace =
            dstd::String((dstd::PathView(sub) / "trace.pfw.gz").str());
        write_trace(parent_trace, 12);
        write_trace(sub_trace, 21);

        // One root holds both folders, and the parent scan does not recurse.
        trace::index::IndexerOptions options;
        options.index_dir = index_dir;
        auto indexer = dftu_utils_test::ok(
            trace::index::Indexer::open({parent_trace, sub_trace}, options));
        dftu_utils_test::ok(indexer.build());
        trace::index::release_all_roots();

        const BuiltSummary first_sub = build_and_ensure(sub, index_dir);
        REQUIRE(first_sub.computes == 1);

        // The subfolder's file is not the parent's, so the parent is not stale
        // and the subfolder keeps its index entry and its summary.
        const BuiltSummary parent_run = build_and_ensure(parent, index_dir);
        CHECK(parent_run.computes == 1);
        CHECK(indexed_paths(index_root(index_dir)).size() == 2);

        const BuiltSummary second_sub = build_and_ensure(sub, index_dir);
        CHECK(second_sub.computes == 0);
        CHECK(second_sub.events == first_sub.events);
    }
}
