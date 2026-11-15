// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/domains/server/http_request.h>
#include <dftracer/utils/domains/server/http_response.h>
#include <dftracer/utils/domains/server/router.h>
#include <dftracer/utils/domains/server/trace_index.h>
#include <dftracer/utils/domains/server/viz/summary_build.h>
#include <dftracer/utils/domains/server/viz_api.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdlib>

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

// A narrow window: the summary answers a whole-trace one, so only the live scan
// reaches the result cache.
constexpr const char* VIZ_RESULT_TARGET =
    "/api/viz/stats?begin=1000&end=1200&ts_normalize=0";

// One server over one trace folder: its index, its runtime and the viz routes,
// so several requests share one result cache. A destroyed server drops the
// index roots it held, as a restart does.
class FolderServer {
   public:
    FolderServer(const dstd::String& directory, const dstd::String& index_dir)
        : index_(directory, index_dir), rt_(4) {
        auto task = coro::with_scope(
            [&](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
                auto initialized = co_await index_.initialize();
                if (!initialized) co_return initialized.error();
                register_viz_api(router_, index_);
                co_return dstd::Result<void>{};
            });
        dftu_utils_test::ok(rt_.submit(std::move(task), "viz_folder").wait());
    }
    FolderServer(const FolderServer&) = delete;
    FolderServer& operator=(const FolderServer&) = delete;
    ~FolderServer() {
        index_.drain_viz_writes();
        rt_.shutdown();
        trace::index::release_all_roots();
    }

    dstd::String request(const dstd::String& target) {
        HttpRequest req;
        req.method = "GET";
        req.path = target;
        dstd::String body;
        Router* router = &router_;
        auto task = coro::with_scope(
            [router, &req, &body](coro::Scope&) -> coro::CoroTask<void> {
                HttpResponse response = co_await router->handle(req);
                CHECK(response.status_code == 200);
                body = std::move(response.body);
                co_return;
            });
        dftu_utils_test::ok(rt_.submit(std::move(task), "viz_request").wait());
        return body;
    }

    // All of `targets` at once, the bodies in order. The requests share the
    // index, so they miss together and put together.
    dstd::Vector<dstd::String> request_all(
        const dstd::Vector<dstd::String>& targets) {
        struct Slot {
            HttpRequest req;
            dstd::String body;
        };
        dstd::Vector<Slot> slots(targets.size());
        dstd::Vector<TaskHandle> handles;
        handles.reserve(targets.size());
        Router* router = &router_;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            slots[i].req.method = "GET";
            slots[i].req.path = targets[i];
            Slot* slot = &slots[i];
            auto task = coro::with_scope(
                [router, slot](coro::Scope&) -> coro::CoroTask<void> {
                    HttpResponse response = co_await router->handle(slot->req);
                    CHECK(response.status_code == 200);
                    slot->body = std::move(response.body);
                    co_return;
                });
            handles.push_back(
                rt_.submit(std::move(task), "viz_request_parallel"));
        }
        for (auto& handle : handles) dftu_utils_test::ok(handle.wait());
        dstd::Vector<dstd::String> out;
        out.reserve(slots.size());
        for (auto& slot : slots) out.push_back(std::move(slot.body));
        return out;
    }

    std::size_t computes() const { return index_.viz_result_compute_count(); }
    std::size_t summary_computes() const {
        return index_.viz_summary_compute_count();
    }
    std::size_t store_reads() const {
        return index_.viz_result_store_read_count();
    }
    std::size_t flush_count() const { return index_.viz_store_flush_count(); }

   private:
    TraceIndex index_;
    Runtime rt_;
    Router router_;
};

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

// One record of the index store, key and value.
struct StoreRecord {
    dstd::String key;
    dstd::String value;
};

constexpr dstd::StringView VIZ_RESULT_NAMESPACE = "server.viz_result";
constexpr dstd::StringView VIZ_SUMMARY_NAMESPACE = "server.viz_summary";

// Every record of `name` the store under `index_dir` holds, in key order: the
// keys and bytes a scan of the namespace reports.
dstd::Vector<StoreRecord> stored_records(const dstd::String& index_dir,
                                         dstd::StringView name) {
    dstd::Vector<StoreRecord> out;
    const dstd::String store_path =
        trace::index::store_of(index_root(index_dir));
    const auto exists = dstd::fs::exists(dstd::PathView(store_path));
    if (!exists.has_value() || !*exists) return out;
    auto store = dftu_utils_test::ok(store::Store::open(store_path));
    auto snapshot = dftu_utils_test::ok(store.snapshot());
    store::Namespace ns;
    ns.spec.name.assign(name.data(), name.size());
    auto cursor = snapshot.scan_all(ns, {}, {});
    for (; !cursor.at_end(); cursor.next()) {
        StoreRecord record;
        const auto key = cursor.key();
        record.key.assign(key.as<char>(), key.size());
        const auto value = cursor.value();
        REQUIRE(value.has_value());
        record.value.assign(value->as<char>(), value->size());
        out.push_back(std::move(record));
    }
    REQUIRE_FALSE(cursor.error().has_value());
    return out;
}

// The stored responses of the folder: every record of the result namespace but
// the folder's byte count, whose key starts with a NUL.
dstd::Vector<StoreRecord> result_records(const dstd::String& index_dir) {
    dstd::Vector<StoreRecord> out;
    for (auto& record : stored_records(index_dir, VIZ_RESULT_NAMESPACE)) {
        if (!record.key.empty() && record.key.front() == '\0') continue;
        out.push_back(std::move(record));
    }
    return out;
}

std::size_t stored_bytes(const dstd::Vector<StoreRecord>& records) {
    std::size_t total = 0;
    for (const auto& record : records) total += record.value.size();
    return total;
}

// A stats window of the trace: one cache key of its own.
dstd::String window_target(int i) {
    return dstd::fmt<"/api/viz/stats?begin={}&end={}&ts_normalize=0">(1000 + i,
                                                                      1100 + i);
}

dstd::Vector<dstd::String> window_targets(int from, int to) {
    dstd::Vector<dstd::String> out;
    out.reserve(static_cast<std::size_t>(to - from));
    for (int i = from; i < to; ++i) out.push_back(window_target(i));
    return out;
}

// Sets an environment variable for one test case.
class EnvVar {
   public:
    EnvVar(const char* name, const dstd::String& value) : name_(name) {
        REQUIRE(::setenv(name, value.c_str(), 1) == 0);
    }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;
    ~EnvVar() { CHECK(::unsetenv(name_) == 0); }

   private:
    const char* name_;
};

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

    TEST_CASE("a stored viz result is served by a new index") {
        auto dir = dftu_utils_test::make_unique_test_path("viz_result_restart");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String directory = dstd::String(dir.str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        write_trace(dstd::String((dir / "trace.pfw.gz").str()), 12);

        dstd::String first;
        {
            FolderServer server(directory, index_dir);
            first = server.request(VIZ_RESULT_TARGET);
            REQUIRE(!first.empty());
            REQUIRE(server.computes() == 1);
        }

        FolderServer server(directory, index_dir);
        const dstd::String second = server.request(VIZ_RESULT_TARGET);
        CHECK(second == first);
        CHECK(server.computes() == 0);
        CHECK(server.store_reads() == 1);
    }

    TEST_CASE("a touched trace file invalidates its stored viz results") {
        auto dir = dftu_utils_test::make_unique_test_path("viz_result_touch");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String directory = dstd::String(dir.str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        const dstd::String trace = dstd::String((dir / "trace.pfw.gz").str());
        write_trace(trace, 12);

        dstd::String first;
        {
            FolderServer server(directory, index_dir);
            first = server.request(VIZ_RESULT_TARGET);
            REQUIRE(server.computes() == 1);
        }

        // The rewrite changes the file size, so the folder is stale and its
        // stored results belong to another fingerprint.
        write_trace(trace, 13);

        FolderServer server(directory, index_dir);
        const dstd::String second = server.request(VIZ_RESULT_TARGET);
        CHECK(second != first);
        CHECK(server.computes() == 1);
    }

    TEST_CASE("one folder stale keeps another folder's stored results") {
        auto base =
            dftu_utils_test::make_unique_test_path("viz_result_two_folders");
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

        dstd::String first_a;
        dstd::String first_b;
        {
            FolderServer server_a(dir_a, index_dir);
            first_a = server_a.request(VIZ_RESULT_TARGET);
            REQUIRE(server_a.computes() == 1);
            FolderServer server_b(dir_b, index_dir);
            first_b = server_b.request(VIZ_RESULT_TARGET);
            REQUIRE(server_b.computes() == 1);
        }

        // Folder a is stale, so its stored results go with its index entries.
        write_trace(trace_a, 13);
        {
            FolderServer server_a(dir_a, index_dir);
            const dstd::String second_a = server_a.request(VIZ_RESULT_TARGET);
            CHECK(second_a != first_a);
            CHECK(server_a.computes() == 1);
        }

        FolderServer server_b(dir_b, index_dir);
        CHECK(server_b.request(VIZ_RESULT_TARGET) == first_b);
        CHECK(server_b.computes() == 0);
        CHECK(server_b.store_reads() == 1);
    }

    TEST_CASE("a second identical request reads no store") {
        auto dir = dftu_utils_test::make_unique_test_path("viz_result_memory");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String directory = dstd::String(dir.str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        write_trace(dstd::String((dir / "trace.pfw.gz").str()), 12);

        FolderServer server(directory, index_dir);
        const dstd::String first = server.request(VIZ_RESULT_TARGET);
        REQUIRE(server.computes() == 1);
        const std::size_t reads = server.store_reads();
        REQUIRE(reads == 1);

        const dstd::String second = server.request(VIZ_RESULT_TARGET);
        CHECK(second == first);
        CHECK(server.computes() == 1);
        CHECK(server.store_reads() == reads);
    }

    TEST_CASE(
        "a run after a stale rebuild serves the rebuilt summary and result") {
        auto dir = dftu_utils_test::make_unique_test_path("viz_result_rebuilt");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String directory = dstd::String(dir.str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        const dstd::String trace = dstd::String((dir / "trace.pfw.gz").str());
        write_trace(trace, 12);

        {
            FolderServer server(directory, index_dir);
            CHECK(!server.request(VIZ_RESULT_TARGET).empty());
            REQUIRE(server.computes() == 1);
            REQUIRE(server.summary_computes() == 1);
        }

        // The rewrite makes the folder stale, so the second run re-indexes and
        // computes both again, and stores what it computed.
        write_trace(trace, 13);
        dstd::String rebuilt;
        {
            FolderServer server(directory, index_dir);
            rebuilt = server.request(VIZ_RESULT_TARGET);
            CHECK(server.computes() == 1);
            CHECK(server.summary_computes() == 1);
        }

        FolderServer server(directory, index_dir);
        CHECK(server.request(VIZ_RESULT_TARGET) == rebuilt);
        CHECK(server.computes() == 0);
        CHECK(server.summary_computes() == 0);
        CHECK(server.store_reads() == 1);
    }

    TEST_CASE(
        "a file added to the folder drops its stored summary and results") {
        auto dir = dftu_utils_test::make_unique_test_path("viz_store_added");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String directory = dstd::String(dir.str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        write_trace(dstd::String((dir / "a.pfw.gz").str()), 12);

        {
            FolderServer server(directory, index_dir);
            CHECK(!server.request(VIZ_RESULT_TARGET).empty());
            REQUIRE(server.computes() == 1);
        }
        REQUIRE(result_records(index_dir).size() == 1);
        REQUIRE(stored_records(index_dir, VIZ_SUMMARY_NAMESPACE).size() == 1);

        // A new file makes the folder stale, and the folder scope it drops is
        // the only one either namespace has.
        write_trace(dstd::String((dir / "b.pfw.gz").str()), 12);
        {
            FolderServer server(directory, index_dir);
        }
        CHECK(result_records(index_dir).empty());
        CHECK(stored_records(index_dir, VIZ_SUMMARY_NAMESPACE).empty());
    }

    TEST_CASE("parallel distinct requests are all stored") {
        auto dir = dftu_utils_test::make_unique_test_path("viz_store_parallel");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String directory = dstd::String(dir.str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        write_trace(dstd::String((dir / "trace.pfw.gz").str()), 12);
        const auto targets = window_targets(0, 12);

        dstd::Vector<dstd::String> bodies;
        {
            FolderServer server(directory, index_dir);
            bodies = server.request_all(targets);
            CHECK(server.computes() == targets.size());
            CHECK(server.flush_count() == 0);
        }

        // Every write the requests started is committed, so a restart serves
        // all of them, the summary with them.
        REQUIRE(result_records(index_dir).size() == targets.size());
        FolderServer server(directory, index_dir);
        for (std::size_t i = 0; i < targets.size(); ++i)
            CHECK(server.request(targets[i]) == bodies[i]);
        CHECK(server.computes() == 0);
        CHECK(server.store_reads() == targets.size());
    }

    TEST_CASE(
        "a put does not flush the store and shutdown commits every write") {
        auto dir = dftu_utils_test::make_unique_test_path("viz_store_flush");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String directory = dstd::String(dir.str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        write_trace(dstd::String((dir / "trace.pfw.gz").str()), 12);
        const auto targets = window_targets(0, 4);

        {
            FolderServer server(directory, index_dir);
            for (const auto& target : targets)
                CHECK(!server.request(target).empty());
            REQUIRE(server.computes() == targets.size());
            // The requests waited for no store write and flushed none.
            CHECK(server.flush_count() == 0);
        }
        CHECK(result_records(index_dir).size() == targets.size());
    }

    TEST_CASE("the folder's stored results stay under the byte bound") {
        auto dir = dftu_utils_test::make_unique_test_path("viz_store_bound");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String directory = dstd::String(dir.str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        write_trace(dstd::String((dir / "trace.pfw.gz").str()), 12);

        // What ten windows cost to store, with no bound.
        {
            FolderServer server(directory, index_dir);
            server.request_all(window_targets(0, 10));
        }
        const auto full = result_records(index_dir);
        REQUIRE(full.size() == 10);
        REQUIRE(stored_bytes(full) > 0);

        // Ten more windows, with the bound at a third of that: the folder drops
        // its stored results as it passes it.
        const auto targets = window_targets(10, 20);
        const std::size_t limit = stored_bytes(full) / 3;
        REQUIRE(limit > 0);
        dstd::Vector<dstd::String> bodies;
        {
            const EnvVar bound("DFTRACER_UTILS_VIZ_RESULT_STORE_BYTES",
                               dstd::to_string(limit));
            FolderServer server(directory, index_dir);
            bodies = server.request_all(targets);
            CHECK(server.computes() == targets.size());
        }
        const auto kept = result_records(index_dir);
        CHECK(kept.size() < targets.size());
        CHECK(stored_bytes(kept) <= limit);

        // What it kept still serves from the store, with no bound set so a miss
        // cannot drop the rest.
        FolderServer server(directory, index_dir);
        std::size_t served = 0;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const std::size_t before = server.computes();
            const dstd::String body = server.request(targets[i]);
            if (server.computes() != before) continue;
            ++served;
            CHECK(body == bodies[i]);
        }
        CHECK(served == kept.size());
        CHECK(served > 0);
    }

    TEST_CASE("an untimed events request reads the store once") {
        auto dir = dftu_utils_test::make_unique_test_path("viz_store_events");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String directory = dstd::String(dir.str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());
        write_trace(dstd::String((dir / "trace.pfw.gz").str()), 12);
        const dstd::String target = "/api/viz/untimed?limit=5";

        dstd::String first;
        {
            FolderServer server(directory, index_dir);
            first = server.request(target);
            REQUIRE(!first.empty());
            // The whole text and the page are two keys of one read.
            CHECK(server.store_reads() == 1);
            REQUIRE(server.computes() == 1);
        }

        FolderServer server(directory, index_dir);
        CHECK(server.request(target) == first);
        CHECK(server.store_reads() == 1);
    }
}
