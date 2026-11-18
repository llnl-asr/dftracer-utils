// SPDX-License-Identifier: MIT
// dftu.svc.store end to end: a plugin writes and commits through the store C
// ABI as real C, a second host over the same index root reads the value back,
// two plugins stay apart under one namespace name, a crafted name cannot reach
// outside the plugin, a copy pinned to a file signature refuses a batch that
// reports a changed file, and a host with no trace folder answers NULL.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/build_host.h>
#include <dftracer/utils/plugins/host/cursor_host.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/plugin/store.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/views/coverage.h>
#include <dftracer/utils/trace/views/fold.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <plugins/abi_c_consumers.h>
#include <plugins/test_abi_ctx.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dftracer::utils;

namespace {

dstd::String text_of(dstd::ByteView v) {
    return dstd::String(reinterpret_cast<const char*>(v.data()), v.size());
}

struct NoopSlice {
    explicit NoopSlice(const plugins::Config&) {}
    void step(dataframe::FrameView, plugins::Host) {}
    void merge(NoopSlice&) {}
    void finalize(plugins::Host) {}
};

}  // namespace

// One plugin over one index root, stepped on one unit so the store service
// knows the root, driven from real C through plugins/store_abi_c_consumer.c.
struct StoreRun {
    dstd::StringIntern intern;
    dftu_plugin* plugin = plugins::make_plugin<NoopSlice>(nullptr);
    dstd::UniquePtr<plugins::host::PluginFold> fold;
    test_plugin_common::Ctx ctx;
    static inline test_plugin_common::Ctx* g_ctx = nullptr;
    static void expect(const char* what, int ok) { CHECK_MESSAGE(ok, what); }
    static inline dftu_test_harness harness{
        nullptr, expect, [](dftu_ctx*, char* msg, std::uint64_t cap) {
            const dstd::String text = g_ctx->message();
            std::snprintf(msg, static_cast<std::size_t>(cap), "%s",
                          text.c_str());
        }};

    StoreRun(dstd::StringView plugin_name, dstd::StringView index_path)
        : fold(dstd::make_unique<plugins::host::PluginFold>(
              plugin, intern, nullptr, nullptr,
              dstd::String(plugin_name.data(), plugin_name.size()))) {
        trace::views::detail::ScanUnit unit;
        unit.index_path = dstd::String(index_path.data(), index_path.size());
        unit.file_path = unit.index_path + "/a.pfw";
        trace::views::detail::FoldBatch batch{{}, unit};
        REQUIRE(fold->step(batch).has_value());
    }
    ~StoreRun() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }

    const dftu_plugin_host* host() { return &fold->host(); }
    dftu_store_report write(dstd::StringView ns, dstd::StringView path,
                            dstd::StringView value,
                            const dftu_store_source_stat* sources = nullptr) {
        return write_at(host(), ns, path, "k", value, sources);
    }
    dftu_store_report write_at(
        const dftu_plugin_host* to, dstd::StringView ns, dstd::StringView path,
        dstd::StringView key, dstd::StringView value,
        const dftu_store_source_stat* sources = nullptr) {
        dftu_store_report report{};
        g_ctx = &ctx;
        dftu_test_store_write_at(ctx.get(), to, &harness, text(ns).c_str(),
                                 text(path).c_str(), text(key).c_str(),
                                 text(value).c_str(), sources, &report);
        return report;
    }
    // Every NULL-handle call of the service, counted.
    dftu_store_report nulls(dstd::StringView ns, dstd::StringView path) {
        dftu_store_report report{};
        g_ctx = &ctx;
        dftu_test_store_nulls(ctx.get(), host(), &harness, text(ns).c_str(),
                              text(path).c_str(), &report);
        return report;
    }
    // A batch of `ns` over `path`, for a commit the test refuses.
    dftu_store_batch_t* batch(dstd::StringView ns, dstd::StringView path) {
        dftu_store_report report{};
        g_ctx = &ctx;
        dftu_store_batch_t* made = nullptr;
        dftu_test_store_make_batch(ctx.get(), host(), &harness,
                                   text(ns).c_str(), text(path).c_str(), &made,
                                   &report);
        return made;
    }
    // Commit `b`; the service frees it either way.
    dftu_store_report commit(dftu_store_batch_t* b) {
        return commit_on(host(), b);
    }
    dftu_store_report commit_on(const dftu_plugin_host* to,
                                dftu_store_batch_t* b) {
        dftu_store_report report{};
        g_ctx = &ctx;
        dftu_test_store_commit(ctx.get(), to, &harness, b, &report);
        return report;
    }
    dftu_store_report read(dstd::StringView ns, dstd::StringView path) {
        dftu_store_report report{};
        g_ctx = &ctx;
        dftu_test_store_read(ctx.get(), host(), &harness, text(ns).c_str(),
                             text(path).c_str(), &report);
        return report;
    }
    // Reuse a namespace handle of `mine` with this fold's host.
    dftu_store_report cross(const dftu_plugin_host* mine, dstd::StringView ns,
                            dstd::StringView path) {
        dftu_store_report report{};
        g_ctx = &ctx;
        dftu_test_store_cross(ctx.get(), mine, host(), &harness,
                              text(ns).c_str(), text(path).c_str(), &report);
        return report;
    }
    dftu_store_report cross(StoreRun& mine, dstd::StringView ns,
                            dstd::StringView path) {
        return cross(mine.host(), ns, path);
    }
    // The namespace handle itself, which the caller must hand back with
    // plugins::host::store_ns_free through the service's ns_free.
    dftu_store_report probe_ns(dstd::StringView ns, dftu_store_ns_t** out) {
        dftu_store_report report{};
        g_ctx = &ctx;
        dftu_test_store_probe_ns(ctx.get(), host(), &harness, text(ns).c_str(),
                                 out, &report);
        return report;
    }
    // Use a namespace handle this fold did not make.
    dftu_store_report use(dftu_store_ns_t* ns, dstd::StringView path) {
        dftu_store_report report{};
        g_ctx = &ctx;
        dftu_test_store_use(ctx.get(), host(), &harness, ns, text(path).c_str(),
                            &report);
        return report;
    }
    // A slice of the fold, as a worker thread would hold, stepped on one unit.
    dstd::UniquePtr<trace::views::detail::Fold> slice(dstd::StringView path) {
        auto made = fold->slice();
        auto* worker = static_cast<plugins::host::PluginFold*>(made.get());
        trace::views::detail::ScanUnit unit;
        unit.index_path = text(path);
        unit.file_path = unit.index_path + "/a.pfw";
        trace::views::detail::FoldBatch step_batch{{}, unit};
        REQUIRE(worker->step(step_batch).has_value());
        return made;
    }
    // Hand the fold a unit of another index root.
    void second_root(dstd::StringView index_path) {
        trace::views::detail::ScanUnit unit;
        unit.index_path = dstd::String(index_path.data(), index_path.size());
        unit.file_path = unit.index_path + "/b.pfw";
        trace::views::detail::FoldBatch batch{{}, unit};
        REQUIRE(fold->step(batch).has_value());
    }
    // The fold's finalize, which flushes the store once.
    dstd::Result<bool> finalize() {
        return default_runtime().block_on(
            fold->finalize(trace::views::detail::CoverageSet{}));
    }
    dftu_store_report probe(dstd::StringView ns) {
        dftu_store_report report{};
        g_ctx = &ctx;
        dftu_test_store_probe(ctx.get(), host(), &harness, text(ns).c_str(),
                              &report);
        return report;
    }
    static dstd::String text(dstd::StringView s) {
        return dstd::String(s.data(), s.size());
    }
};

// A fresh process reads the plugin namespace: no handle is shared with or
// inherited from the process under test. It prints VALUE=<bytes> on stderr.
TEST_CASE("store: probes a namespace in a fresh process" * doctest::skip()) {
    fresh_process::announce();
    const char* root = std::getenv("DFTRACER_UTILS_STORE_PROBE_ROOT");
    const char* trace = std::getenv("DFTRACER_UTILS_STORE_PROBE_TRACE");
    REQUIRE(root != nullptr);
    REQUIRE(trace != nullptr);
    const dstd::String dir = trace::index::store_of(root);
    const dstd::String path(trace);
    const char* paths[1] = {path.c_str()};
    dftu_store_scope scope = {DFTU_STORE_SCOPE_FILE, paths, 1};
    dftu_error err = DFTU_ERROR_INIT;
    dftu_store_t* store = nullptr;
    dftu_store_ns_t* ns = nullptr;
    dftu_store_snapshot_t* snap = nullptr;
    dftu_store_cursor_t* cursor = nullptr;
    dftu_store_bytes value{nullptr, 0};
    REQUIRE(dftu_store_open(dir.c_str(), &store, &err) == DFTU_OK);
    REQUIRE(dftu_store_ns(store, "plugin.toy_store.counters", 1,
                          DFTU_STORE_FOLD_NONE, 0, 0, &ns, &err) == DFTU_OK);
    REQUIRE(dftu_store_snapshot(store, &snap, &err) == DFTU_OK);
    REQUIRE(dftu_store_snapshot_scan_prefix(snap, ns, &scope, nullptr, 0,
                                            &cursor, &err) == DFTU_OK);
    /* Every value in key order, joined by '|': one line per namespace state. */
    bool any = false;
    while (dftu_store_cursor_next(cursor, &err) == DFTU_OK) {
        REQUIRE(dftu_store_cursor_value(cursor, &value, &err) == DFTU_OK);
        std::fprintf(stderr, "%s%.*s",
                     any ? "|" : "VALUE=", static_cast<int>(value.size),
                     reinterpret_cast<const char*>(value.data));
        any = true;
    }
    if (any) std::fputc('\n', stderr);
    dftu_store_cursor_free(cursor);
    dftu_store_snapshot_free(snap);
    dftu_store_ns_free(ns);
    dftu_store_free(store);
}

namespace {

// The value a fresh process reads out of the plugin namespace, or "".
dstd::String probe_in_fresh_process(dstd::StringView root,
                                    dstd::StringView trace) {
    ::setenv("DFTRACER_UTILS_STORE_PROBE_ROOT", StoreRun::text(root).c_str(),
             1);
    ::setenv("DFTRACER_UTILS_STORE_PROBE_TRACE", StoreRun::text(trace).c_str(),
             1);
    const fresh_process::Result ran = fresh_process::run(
        "store: probes a namespace in a fresh process", 60000);
    const char* const output = ran.output.c_str();
    CHECK_MESSAGE(std::strstr(output, "ERROR") == nullptr,
                  "the probe process failed: ", output);
    const char* const at = std::strstr(output, "VALUE=");
    if (at == nullptr) return dstd::String();
    const char* const end = std::strchr(at, '\n');
    const std::size_t size = end != nullptr
                                 ? static_cast<std::size_t>(end - at - 6)
                                 : std::strlen(at + 6);
    return dstd::String(at + 6, size);
}

}  // namespace

TEST_CASE("store: a plugin's committed value survives into a later run") {
    const dstd::String root = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_run").str());
    const dstd::String trace = root + "/a.pfw";
    REQUIRE(dstd::fs::create_directories(dstd::PathView(root)).has_value());
    REQUIRE(dstd::fs::write_file(dstd::PathView(trace), "one").has_value());
    {
        StoreRun run("toy_store", root);
        const auto report = run.write("counters", trace, "first");
        REQUIRE(report.has_service == 1);
        REQUIRE(report.namespace_registered == 1);
        REQUIRE(report.committed == 1);
        REQUIRE(run.finalize().has_value());
    }
    // A new host over the same index root, as a later run would make it.
    StoreRun again("toy_store", root);
    const auto back = again.read("counters", trace);
    REQUIRE(back.namespace_registered == 1);
    REQUIRE(back.found == 1);
    CHECK(std::strcmp(back.value, "first") == 0);
}

TEST_CASE("store: a reader in a fresh process sees a batch after the flush") {
    const dstd::String root = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_visible").str());
    const dstd::String trace = root + "/a.pfw";
    StoreRun run("toy_store", root);
    REQUIRE(run.write("counters", trace, "seen").committed == 1);
    // No shared or inherited handle and no flush yet: this says whether a
    // commit alone reaches another process.
    const dstd::String before = probe_in_fresh_process(root, trace);
    CHECK(before.empty());
    // The fold's finalize flushes the writer once.
    const auto finished = default_runtime().block_on(
        run.fold->finalize(trace::views::detail::CoverageSet{}));
    REQUIRE(finished.has_value());
    CHECK(probe_in_fresh_process(root, trace) == dstd::String("seen"));
}

TEST_CASE("store: a namespace handle of another host's store is refused") {
    const dstd::String root_a = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_cross_a").str());
    const dstd::String root_b = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_cross_b").str());
    StoreRun a("toy_a", root_a);
    StoreRun b("toy_a", root_b);
    const auto refused = b.cross(a, "counters", root_a + "/a.pfw");
    CHECK(refused.namespace_registered == 1);
    CHECK(refused.status == DFTU_E_INVALID);
    CHECK_MESSAGE(
        dstd::StringView(refused.message).find("another host's store") !=
            dstd::StringView::npos,
        refused.message);
    CHECK(refused.refused_read == 1);
    // The same handle on the host that made it is fine.
    CHECK(a.probe("counters").namespace_registered == 1);
}

TEST_CASE("store: a scan over two index roots is refused") {
    const dstd::String root_a = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_two_roots_a")
            .str());
    const dstd::String root_b = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_two_roots_b")
            .str());
    StoreRun run("toy_a", root_a);
    run.second_root(root_b);
    const auto report = run.write("counters", root_a + "/a.pfw", "x");
    CHECK(report.committed == 0);
    CHECK(report.status == DFTU_E_INVALID);
    const dstd::StringView message(report.message);
    CHECK_MESSAGE(message.find(root_a) != dstd::StringView::npos, message);
    CHECK_MESSAGE(message.find(root_b) != dstd::StringView::npos, message);
}

TEST_CASE("store: a failed fold-end flush reaches the run's result") {
    const dstd::String root = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_flush_fail")
            .str());
    StoreRun run("toy_store", root);
    REQUIRE(run.write("counters", root + "/a.pfw", "x").committed == 1);
    plugins::host::detail::fail_next_store_flush_for_test();
    const auto finished = run.finalize();
    REQUIRE_FALSE(finished.has_value());
    CHECK(finished.error().message().find("store flush") !=
          dstd::StringView::npos);
}

TEST_CASE("store: the C++ SDK writes and reads through Host::store()") {
    const dstd::String root = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_sdk").str());
    const dstd::String trace = root + "/a.pfw";
    const dstd::ByteView key{"k", 1};
    const dstd::ByteView value{"sdk", 3};
    {
        StoreRun writer("toy_store", root);
        plugins::Host host =
            plugins::detail::host_from_raw(writer.host(), writer.ctx.get());
        const plugins::Store store = host.store();
        const auto ns = store.ns("counters");
        REQUIRE(ns.has_value());
        auto batch = store.batch(*ns, plugins::StoreScope::file(trace));
        REQUIRE(batch.has_value());
        REQUIRE(batch->put(key, value).has_value());
        REQUIRE(batch->commit().has_value());
        // The fold's end flush is what another reader sees.
        REQUIRE(writer.finalize().has_value());
    }
    StoreRun reader("toy_store", root);
    plugins::Host host =
        plugins::detail::host_from_raw(reader.host(), reader.ctx.get());
    const plugins::Store store = host.store();
    const auto ns = store.ns("counters");
    REQUIRE(ns.has_value());
    const plugins::StoreScope scope = plugins::StoreScope::file(trace);
    auto snapshot = store.snapshot();
    REQUIRE(snapshot.has_value());

    const auto found = snapshot->get(*ns, scope, key);
    REQUIRE(found.has_value());
    REQUIRE(found->has_value());
    const dstd::ByteView read = (*found)->bytes();
    CHECK(text_of(read) == "sdk");

    auto cursor = snapshot->scan_prefix(*ns, scope, dstd::ByteView{});
    REQUIRE(cursor.has_value());
    // A moved cursor keeps the ctx of the fold it reads through.
    plugins::StoreCursor moved;
    moved = std::move(*cursor);
    const auto more = moved.next();
    if (!more.has_value()) MESSAGE(more.error().message());
    REQUIRE(more.has_value());
    REQUIRE(*more);
    CHECK(text_of(moved.key()) == "k");
    const auto scanned = moved.value();
    REQUIRE(scanned.has_value());
    CHECK(text_of(*scanned) == "sdk");
    const auto end = moved.next();
    REQUIRE(end.has_value());
    CHECK_FALSE(*end);
    // Reading past the end fails, and the failure lands on the fold's ctx.
    const auto past = moved.value();
    CHECK_FALSE(past.has_value());
    CHECK_MESSAGE(
        reader.ctx.message().find("at its end") != dstd::StringView::npos,
        reader.ctx.message());
}

TEST_CASE("store: a value a slice commits is flushed by the master") {
    const dstd::String root = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_slices").str());
    const dstd::String trace = root + "/a.pfw";
    REQUIRE(dstd::fs::create_directories(dstd::PathView(root)).has_value());
    REQUIRE(dstd::fs::write_file(dstd::PathView(trace), "one").has_value());

    static const char* const KEYS[] = {"s0", "s1", "s2"};
    static const char* const VALUES[] = {"v0", "v1", "v2"};
    StoreRun run("toy_store", root);
    // Three workers, each writing through its own store handle in its step and
    // nothing in on_finalize.
    dstd::Vector<dstd::UniquePtr<trace::views::detail::Fold>> slices;
    for (int i = 0; i < 3; ++i) {
        auto worker = run.slice(trace);
        auto* w = static_cast<plugins::host::PluginFold*>(worker.get());
        const auto report =
            run.write_at(&w->host(), "counters", trace, KEYS[i], VALUES[i]);
        REQUIRE(report.committed == 1);
        slices.push_back(std::move(worker));
    }
    for (auto& worker : slices) {
        REQUIRE(run.fold->merge(*worker).has_value());
    }
    // The master's own handle is closed: finalize has to open the shared state
    // of the directory and flush what the slices committed.
    const auto finished = run.finalize();
    if (!finished.has_value()) MESSAGE(finished.error().message());
    REQUIRE(finished.has_value());
    CHECK(probe_in_fresh_process(root, trace) == dstd::String("v0|v1|v2"));
}

TEST_CASE("store: a namespace handle of a slice is accepted by the master") {
    const dstd::String root = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_slice_ns").str());
    const dstd::String trace = root + "/a.pfw";
    StoreRun run("toy_a", root);
    auto worker = run.slice(trace);
    auto* w = static_cast<plugins::host::PluginFold*>(worker.get());
    // Both folds lend the same directory, so a handle one made is the other's.
    const auto accepted = run.cross(&w->host(), "counters", trace);
    CHECK(accepted.namespace_registered == 1);
    CHECK(accepted.status == DFTU_OK);
    CHECK(accepted.refused_read == 0);
    REQUIRE(run.fold->merge(*worker).has_value());
}

TEST_CASE("store: a refused commit consumes the batch") {
    const dstd::String root_a = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_leak_a").str());
    const dstd::String root_b = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_leak_b").str());
    const dstd::String trace = root_a + "/a.pfw";
    // A batch the host forgot is still live at exit. The tests preset arms the
    // memory exit check, so a commit that returns before freeing fails this
    // run, and the store error paths above are the ones that hold a batch.
    {
        // A second index root arrives after the batches were made, so both
        // commits are refused; the host still owns and frees each batch.
        StoreRun run("toy_a", root_a);
        REQUIRE(run.write("counters", trace, "warm").committed == 1);
        dftu_store_batch_t* first = run.batch("counters", trace);
        dftu_store_batch_t* second = run.batch("counters", trace);
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        run.second_root(root_b);
        const auto refused = run.commit(first);
        CHECK(refused.committed == 0);
        CHECK(refused.status == DFTU_E_INVALID);
        CHECK(dstd::StringView(refused.message).find("two index roots") !=
              dstd::StringView::npos);
        CHECK(run.commit(second).status == DFTU_E_INVALID);
    }
    {
        // A batch of one store committed through another: refused and freed.
        StoreRun a("toy_a", root_a);
        StoreRun b("toy_a", root_b);
        REQUIRE(a.write("counters", trace, "warm").committed == 1);
        dftu_store_batch_t* first = a.batch("counters", trace);
        dftu_store_batch_t* second = a.batch("counters", trace);
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        const auto refused = b.commit(first);
        CHECK(refused.status == DFTU_E_INVALID);
        CHECK(dstd::StringView(refused.message).find("another host's store") !=
              dstd::StringView::npos);
        CHECK(b.commit(second).status == DFTU_E_INVALID);
    }
}

TEST_CASE("store: a NULL handle is refused by every call") {
    const dstd::String root = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_nulls").str());
    const dstd::String trace = root + "/a.pfw";
    StoreRun run("toy_a", root);
    const auto report = run.nulls("counters", trace);
    CHECK(report.null_refused == 13);
    CHECK(report.status == DFTU_OK);
    CHECK(dstd::StringView(report.message).find("handle is NULL") !=
          dstd::StringView::npos);
}

TEST_CASE("store: a handle of a freed fold is refused by another root") {
    const dstd::String root_a = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_freed_a").str());
    const dstd::String root_b = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_freed_b").str());
    dftu_store_ns_t* stale = nullptr;
    {
        // Take a namespace handle, then let its fold and store die. The handle
        // keeps the store's identity by value, not the store's address, so an
        // address the allocator hands out again cannot pass it off as its own.
        StoreRun run("toy_a", root_a);
        const auto report = run.probe_ns("counters", &stale);
        REQUIRE(report.namespace_registered == 1);
    }
    StoreRun other("toy_a", root_b);
    const auto refused = other.use(stale, root_b + "/a.pfw");
    CHECK(refused.namespace_registered == 0);
    CHECK(refused.status == DFTU_E_INVALID);
    CHECK(dstd::StringView(refused.message).find("another host's store") !=
          dstd::StringView::npos);
    // The owning plugin hands a handle back through ns_free, which takes no
    // host: the handle carries its own identity and inner pointer.
    other.host()->get_service(other.host()->h, DFTU_SVC_STORE);
    auto* svc = static_cast<const dftu_svc_store*>(
        other.host()->get_service(other.host()->h, DFTU_SVC_STORE));
    REQUIRE(svc != nullptr);
    svc->ns_free(stale);
}

TEST_CASE("store: two plugins keep separate data under one namespace name") {
    const dstd::String root = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_two").str());
    const dstd::String trace = root + "/a.pfw";
    {
        StoreRun a("toy_a", root);
        REQUIRE(a.write("counters", trace, "from-a").committed == 1);
    }
    {
        StoreRun b("toy_b", root);
        REQUIRE(b.write("counters", trace, "from-b").committed == 1);
    }
    // The same namespace name, one per plugin, read by a later fold of each.
    StoreRun a("toy_a", root);
    StoreRun b("toy_b", root);
    const auto back_a = a.read("counters", trace);
    const auto back_b = b.read("counters", trace);
    REQUIRE(back_a.found == 1);
    REQUIRE(back_b.found == 1);
    CHECK(std::strcmp(back_a.value, "from-a") == 0);
    CHECK(std::strcmp(back_b.value, "from-b") == 0);
}

TEST_CASE("store: a namespace name cannot escape the plugin's own names") {
    const dstd::String root = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_names").str());
    StoreRun run("toy_a", root);

    const dstd::Pair<const char*, const char*> cases[] = {
        {"", "is empty"},
        {".hidden", "starts with '.'"},
        {"..plugin.toy_b.counters", "contains '..'"},
        {"a..b", "contains '..'"},
        {"server.results", "the host keeps"},
        {"dftu.yaml", "the host keeps"},
        {"Upper", "outside a-z"},
        {"/abs", "outside a-z"},
    };
    for (const auto& [name, why] : cases) {
        const auto report = run.probe(name);
        CHECK_MESSAGE(report.status == DFTU_E_INVALID, name);
        CHECK_MESSAGE(report.namespace_registered == 0, name);
        CHECK_MESSAGE(dstd::StringView(report.message).find(why) !=
                          dstd::StringView::npos,
                      name, " <- ", report.message);
    }
    CHECK(run.probe("counters").namespace_registered == 1);
    CHECK(run.probe("a.b-c_1").namespace_registered == 1);
}

TEST_CASE("store: a copy pinned to a file signature refuses a changed file") {
    const dstd::String root = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_sources").str());
    const dstd::String trace = root + "/a.pfw";
    REQUIRE(dstd::fs::create_directories(dstd::PathView(root)).has_value());
    REQUIRE(dstd::fs::write_file(dstd::PathView(trace), "one").has_value());
    struct stat st{};
    REQUIRE(::stat(trace.c_str(), &st) == 0);
    const dftu_store_source_stat before{static_cast<std::uint64_t>(st.st_mtime),
                                        static_cast<std::uint64_t>(st.st_size),
                                        11};

    StoreRun run("toy_a", root);
    REQUIRE(run.write("pinned", trace, "pinned", &before).committed == 1);

    REQUIRE(dstd::fs::write_file(dstd::PathView(trace), "one and a longer tail")
                .has_value());
    REQUIRE(::stat(trace.c_str(), &st) == 0);
    const dftu_store_source_stat after{static_cast<std::uint64_t>(st.st_mtime),
                                       static_cast<std::uint64_t>(st.st_size),
                                       22};
    REQUIRE(after.size != before.size);
    const auto refused = run.write("pinned", trace, "stale", &after);
    CHECK(refused.committed == 0);
    CHECK(refused.message[0] != '\0');
}

TEST_CASE("store: a host with no trace folder answers NULL") {
    plugins::host::CursorHost cursor(nullptr, "toy_a", 0);
    const dftu_plugin_host* host = cursor.host();
    CHECK(host->get_service(host->h, DFTU_SVC_STORE) == nullptr);

    plugins::host::BuildHost build(dstd::String("toy_a"));
    dftu_plugin_host* load = build.host();
    CHECK(load->get_service(load->h, DFTU_SVC_STORE) == nullptr);
    CHECK(build.denied() == dstd::StringView(DFTU_SVC_STORE));

    const dstd::String root = StoreRun::text(
        dftu_utils_test::make_unique_test_path("plugin_store_none").str());
    StoreRun run("toy_a", root);
    CHECK(run.host()->get_service(run.host()->h, DFTU_SVC_STORE) != nullptr);
}
