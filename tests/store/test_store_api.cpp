// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/store/writer.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <thread>
#include <utility>

using namespace dftracer::utils;

namespace {

using namespace dftracer::utils::store;

dstd::ByteView view_of(const dstd::String& bytes) {
    return {bytes.data(), bytes.size()};
}

dstd::String to_std(dstd::ByteView bytes) {
    return dstd::String(reinterpret_cast<const char*>(bytes.data()),
                        bytes.size());
}

NamespaceSpec spec_of(const dstd::String& name,
                      FoldKind fold = FoldKind::None) {
    NamespaceSpec spec;
    spec.name = dstd::String(name.data(), name.size());
    spec.fold = fold;
    return spec;
}

ScopeKey scope_of(const dstd::String& path) {
    return ScopeKey::file(dstd::StringView(path.data(), path.size()));
}

Store open_store(const dftu_utils_test::ScopedTestDir& dir) {
    auto opened =
        Store::open(dstd::StringView(dir.str().data(), dir.str().size()));
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

Namespace register_ns(Store& store, const NamespaceSpec& spec) {
    auto added = store.ns(spec);
    REQUIRE(added.has_value());
    return *added;
}

dstd::Expected<void, dstd::Error> run(
    coro::CoroTask<dstd::Expected<void, dstd::Error>> task) {
    return dftracer::utils::default_runtime().submit(std::move(task)).get();
}

void put_all(Store& store, const Namespace& ns, const ScopeKey& scope,
             const dstd::Vector<dstd::Pair<dstd::String, dstd::String>>& rows,
             bool building = false) {
    Batch batch =
        building ? store.building_batch(ns, scope) : store.batch(ns, scope);
    for (const auto& [key, value] : rows) {
        batch.put(view_of(key), view_of(value));
    }
    REQUIRE(run(store.commit(std::move(batch))).has_value());
}

dstd::Optional<dstd::String> get_value(Store& store, const Namespace& ns,
                                       const ScopeKey& scope,
                                       const dstd::String& key) {
    auto snapshot = store.snapshot();
    REQUIRE(snapshot.has_value());
    auto found = snapshot->get(ns, scope, view_of(key));
    REQUIRE(found.has_value());
    if (!found->has_value()) return dstd::nullopt;
    return to_std((*found)->bytes());
}

dstd::Vector<dstd::Pair<dstd::String, dstd::String>> collect(Cursor cursor) {
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> rows;
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        rows.emplace_back(to_std(cursor.key()), to_std(*value));
        cursor.next();
    }
    CHECK_FALSE(cursor.error().has_value());
    return rows;
}

using Rows = dstd::Vector<dstd::Pair<dstd::String, dstd::String>>;

// Read once by the first snapshot of the process, so set before main.
const int REFRESH_EVERY_SNAPSHOT = [] {
    ::setenv(
        dftracer::utils::common::constants::indexer::INDEX_STORE_REFRESH_MS_ENV,
        "0", 1);
    return 0;
}();

}  // namespace

TEST_SUITE("store api") {
    TEST_CASE("two opens of one directory share one state") {
        dftu_utils_test::ScopedTestDir dir("store_api_shared");
        Store first = open_store(dir);
        auto second_open = Store::open(
            dstd::StringView((dir.str() + "/").data(), dir.str().size() + 1));
        REQUIRE(second_open.has_value());
        Store second = std::move(*second_open);

        const NamespaceSpec spec = spec_of("dftu.test.shared");
        const Namespace a = register_ns(first, spec);
        const Namespace b = register_ns(second, spec);
        CHECK(a.id == b.id);

        const Namespace c = register_ns(second, spec_of("dftu.test.other"));
        const Namespace d = register_ns(first, spec_of("dftu.test.other"));
        CHECK(c.id == d.id);
        CHECK(c.id != a.id);

        const ScopeKey scope = scope_of("/traces/a.pfw");
        put_all(first, a, scope, {{"k", "v"}});
        REQUIRE(run(first.flush()).has_value());
        CHECK(get_value(second, b, scope, "k") ==
              dstd::Optional<dstd::String>("v"));
    }

    TEST_CASE("a namespace registered twice gives one handle") {
        dftu_utils_test::ScopedTestDir dir("store_api_ns");
        Store store = open_store(dir);
        const Namespace a = register_ns(store, spec_of("dftu.test.one"));
        const Namespace b = register_ns(store, spec_of("dftu.test.one"));
        const Namespace c = register_ns(store, spec_of("dftu.test.two"));
        CHECK(a.id == b.id);
        CHECK(a.id != c.id);

        NamespaceSpec other = spec_of("dftu.test.one");
        other.format_version = 3;
        CHECK_FALSE(store.ns(other).has_value());
        CHECK_FALSE(store.ns(spec_of("")).has_value());
    }

    TEST_CASE("a commit is visible to the next snapshot after a flush") {
        dftu_utils_test::ScopedTestDir dir("store_api_commit");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.put"));
        const ScopeKey scope = scope_of("/traces/a.pfw");

        CHECK_FALSE(get_value(store, ns, scope, "k").has_value());
        put_all(store, ns, scope, {{"k", "v1"}, {"j", "v2"}});
        REQUIRE(run(store.flush()).has_value());
        CHECK(get_value(store, ns, scope, "k") ==
              dstd::Optional<dstd::String>("v1"));
        CHECK(get_value(store, ns, scope, "j") ==
              dstd::Optional<dstd::String>("v2"));
        CHECK_FALSE(get_value(store, ns, scope, "x").has_value());

        auto snapshot = store.snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->copy_info(ns, scope).state == ScopeState::Ready);
    }

    TEST_CASE("multi_get answers in key order and leaves a hole for a miss") {
        dftu_utils_test::ScopedTestDir dir("store_api_multi");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.multi"));
        const ScopeKey scope = scope_of("/traces/a.pfw");
        put_all(store, ns, scope, {{"a", "1"}, {"b", "2"}, {"c", "3"}});
        REQUIRE(run(store.flush()).has_value());

        const dstd::String c = "c", missing = "zz", a = "a";
        const dstd::Vector<dstd::ByteView> keys{view_of(c), view_of(missing),
                                                view_of(a)};
        auto snapshot = store.snapshot();
        REQUIRE(snapshot.has_value());
        auto values = snapshot->multi_get(
            ns, scope,
            dstd::Span<const dstd::ByteView>(keys.data(), keys.size()));
        REQUIRE(values.has_value());
        REQUIRE(values->size() == 3);
        REQUIRE((*values)[0].has_value());
        CHECK(to_std((*values)[0]->bytes()) == "3");
        CHECK_FALSE((*values)[1].has_value());
        REQUIRE((*values)[2].has_value());
        CHECK(to_std((*values)[2]->bytes()) == "1");
    }

    TEST_CASE("scans read a prefix, a range and every scope") {
        dftu_utils_test::ScopedTestDir dir("store_api_scan");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.scan"));
        const ScopeKey first = scope_of("/traces/a.pfw");
        const ScopeKey second = scope_of("/traces/b.pfw");
        put_all(store, ns, first,
                {{"ab1", "x"}, {"ab2", "y"}, {"ac1", "z"}, {"b1", "w"}});
        put_all(store, ns, second, {{"ab9", "q"}});
        REQUIRE(run(store.flush()).has_value());

        auto snapshot = store.snapshot();
        REQUIRE(snapshot.has_value());
        const dstd::String prefix = "ab";
        CHECK(collect(snapshot->scan(ns, first, view_of(prefix))) ==
              Rows{{"ab1", "x"}, {"ab2", "y"}});
        const dstd::String begin = "ab2", end = "b1";
        CHECK(
            collect(snapshot->scan(ns, first, view_of(begin), view_of(end))) ==
            Rows{{"ab2", "y"}, {"ac1", "z"}});
        const dstd::String low, high;
        CHECK(collect(snapshot->scan_all(ns, view_of(low), view_of(high))) ==
              Rows{{"ab1", "x"},
                   {"ab2", "y"},
                   {"ac1", "z"},
                   {"b1", "w"},
                   {"ab9", "q"}});
        const dstd::Vector<ScopeKey> both{second, first};
        CHECK(collect(snapshot->scan(
                  ns, dstd::Span<const ScopeKey>(both.data(), both.size()),
                  view_of(low), view_of(high))) == Rows{{"ab9", "q"},
                                                        {"ab1", "x"},
                                                        {"ab2", "y"},
                                                        {"ac1", "z"},
                                                        {"b1", "w"}});

        auto scopes = snapshot->scopes(ns);
        REQUIRE(scopes.has_value());
        CHECK(scopes->size() == 2);
        auto declared = snapshot->namespaces();
        REQUIRE(declared.has_value());
        REQUIRE(declared->size() == 1);
        CHECK(to_std(dstd::ByteView{declared->front().name.data(),
                                    declared->front().name.size()}) ==
              "dftu.test.scan");
    }

    TEST_CASE("a cursor keeps its snapshot alive") {
        dftu_utils_test::ScopedTestDir dir("store_api_cursor");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.cursor"));
        const ScopeKey scope = scope_of("/traces/a.pfw");
        put_all(store, ns, scope, {{"k1", "v1"}, {"k2", "v2"}});
        REQUIRE(run(store.flush()).has_value());

        const dstd::String low, high;
        Cursor cursor = [&] {
            auto snapshot = store.snapshot();
            REQUIRE(snapshot.has_value());
            return snapshot->scan(ns, scope, view_of(low), view_of(high));
        }();
        put_all(store, ns, scope, {{"k3", "v3"}});
        REQUIRE(run(store.flush()).has_value());
        CHECK(collect(std::move(cursor)) == Rows{{"k1", "v1"}, {"k2", "v2"}});
    }

    TEST_CASE("a building copy stays hidden until publish") {
        dftu_utils_test::ScopedTestDir dir("store_api_building");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.building"));
        const ScopeKey scope = scope_of("/traces/a.pfw");

        put_all(store, ns, scope, {{"k", "old"}});
        put_all(store, ns, scope, {{"k", "new"}, {"n", "added"}}, true);
        REQUIRE(run(store.flush()).has_value());
        CHECK(get_value(store, ns, scope, "k") ==
              dstd::Optional<dstd::String>("old"));
        CHECK_FALSE(get_value(store, ns, scope, "n").has_value());

        const dstd::Vector<SourceStat> sources{SourceStat{1, 2, 3}};
        const Publish target{&ns, &scope, 7, &sources};
        REQUIRE(run(store.publish({&target, 1})).has_value());
        CHECK(get_value(store, ns, scope, "k") ==
              dstd::Optional<dstd::String>("new"));
        CHECK(get_value(store, ns, scope, "n") ==
              dstd::Optional<dstd::String>("added"));
        auto snapshot = store.snapshot();
        REQUIRE(snapshot.has_value());
        const store::CopyInfo info = snapshot->copy_info(ns, scope);
        CHECK(info.params_hash == 7);
        REQUIRE(info.sources.size() == 1);
        CHECK(info.sources[0].hash == 3);
    }

    TEST_CASE("a publish of copies in two namespaces is all or nothing") {
        dftu_utils_test::ScopedTestDir dir("store_api_publish");
        Store store = open_store(dir);
        const Namespace left = register_ns(store, spec_of("dftu.test.left"));
        const Namespace right = register_ns(store, spec_of("dftu.test.right"));
        const ScopeKey scope = scope_of("/traces/a.pfw");
        const ScopeKey unbuilt = scope_of("/traces/never.pfw");

        put_all(store, left, scope, {{"k", "l"}}, true);
        put_all(store, right, scope, {{"k", "r"}}, true);
        REQUIRE(run(store.flush()).has_value());

        const Publish bad[] = {{&left, &scope, 0, nullptr},
                               {&right, &unbuilt, 0, nullptr}};
        CHECK_FALSE(run(store.publish(bad)).has_value());
        CHECK_FALSE(get_value(store, left, scope, "k").has_value());
        CHECK_FALSE(get_value(store, right, scope, "k").has_value());

        const Publish null_target[] = {{&left, nullptr, 0, nullptr}};
        CHECK_FALSE(run(store.publish(null_target)).has_value());

        const Publish good[] = {{&left, &scope, 0, nullptr},
                                {&right, &scope, 0, nullptr}};
        REQUIRE(run(store.publish(good)).has_value());
        CHECK(get_value(store, left, scope, "k") ==
              dstd::Optional<dstd::String>("l"));
        CHECK(get_value(store, right, scope, "k") ==
              dstd::Optional<dstd::String>("r"));
    }

    TEST_CASE("a publish applies a drop, a fail and a new copy together") {
        dftu_utils_test::ScopedTestDir dir("store_api_changes");
        Store store = open_store(dir);
        const Namespace gone = register_ns(store, spec_of("dftu.test.gone"));
        const Namespace bad = register_ns(store, spec_of("dftu.test.bad"));
        const Namespace fresh = register_ns(store, spec_of("dftu.test.fresh"));
        const ScopeKey scope = scope_of("/traces/a.pfw");
        const ScopeKey unbuilt = scope_of("/traces/never.pfw");

        put_all(store, gone, scope, {{"k", "old"}});
        put_all(store, bad, scope, {{"k", "old"}});
        put_all(store, fresh, scope, {{"k", "new"}}, true);
        REQUIRE(run(store.flush()).has_value());
        auto before = store.snapshot();
        REQUIRE(before.has_value());

        const Publish refused[] = {
            {&gone, &scope, 0, nullptr, Publish::Change::Dropped},
            {&fresh, &unbuilt, 0, nullptr, Publish::Change::Ready}};
        CHECK_FALSE(run(store.publish(refused)).has_value());
        CHECK(get_value(store, gone, scope, "k") ==
              dstd::Optional<dstd::String>("old"));

        const Publish changes[] = {
            {&gone, &scope, 0, nullptr, Publish::Change::Dropped},
            {&bad, &scope, 5, nullptr, Publish::Change::Failed},
            {&fresh, &scope, 9, nullptr, Publish::Change::Ready}};
        REQUIRE(run(store.publish(changes)).has_value());

        auto after = store.snapshot();
        REQUIRE(after.has_value());
        CHECK(after->copy_info(gone, scope).state == ScopeState::Absent);
        CHECK(after->copy_info(bad, scope).state == ScopeState::Failed);
        CHECK(after->copy_info(bad, scope).params_hash == 5);
        CHECK(after->copy_info(fresh, scope).state == ScopeState::Ready);
        CHECK(after->copy_info(fresh, scope).params_hash == 9);
        CHECK(get_value(store, fresh, scope, "k") ==
              dstd::Optional<dstd::String>("new"));
        CHECK_FALSE(get_value(store, gone, scope, "k").has_value());

        CHECK(before->copy_info(gone, scope).state == ScopeState::Ready);
        CHECK(before->copy_info(bad, scope).state == ScopeState::Ready);
        CHECK(before->copy_info(fresh, scope).state != ScopeState::Ready);
    }

    TEST_CASE("a copy failed while building takes plain batches again") {
        dftu_utils_test::ScopedTestDir dir("store_api_fail_building");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.fb"));
        const ScopeKey scope = scope_of("/traces/a.pfw");

        put_all(store, ns, scope, {{"k", "half"}}, true);
        const Publish failed[] = {
            {&ns, &scope, 3, nullptr, Publish::Change::Failed}};
        REQUIRE(run(store.publish(failed)).has_value());
        auto snapshot = store.snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->copy_info(ns, scope).state == ScopeState::Failed);
        CHECK_FALSE(get_value(store, ns, scope, "k").has_value());

        put_all(store, ns, scope, {{"k", "whole"}});
        REQUIRE(run(store.flush()).has_value());
        CHECK(get_value(store, ns, scope, "k") ==
              dstd::Optional<dstd::String>("whole"));
    }

    TEST_CASE("fold namespaces fold, and a plain put on one is refused") {
        dftu_utils_test::ScopedTestDir dir("store_api_fold");
        Store store = open_store(dir);
        const Namespace ns =
            register_ns(store, spec_of("dftu.test.sum", FoldKind::SumI64));
        const ScopeKey scope = scope_of("/traces/a.pfw");

        const auto eight = [](std::int64_t value) {
            dstd::String bytes(8, '\0');
            dstd::write_u64_le(reinterpret_cast<std::uint8_t*>(bytes.data()),
                               static_cast<std::uint64_t>(value));
            return bytes;
        };
        for (const std::int64_t amount : {2, 5}) {
            Batch batch = store.batch(ns, scope);
            batch.fold(view_of("k"), view_of(eight(amount)));
            REQUIRE(run(store.commit(std::move(batch))).has_value());
        }
        REQUIRE(run(store.flush()).has_value());
        CHECK(get_value(store, ns, scope, "k") ==
              dstd::Optional<dstd::String>(eight(7)));

        Batch refused = store.batch(ns, scope);
        refused.put(view_of("k"), view_of(eight(1)));
        CHECK_FALSE(run(store.commit(std::move(refused))).has_value());
    }

    TEST_CASE("drop, erase and fail change what a reader sees") {
        dftu_utils_test::ScopedTestDir dir("store_api_drop");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.drop"));
        const ScopeKey erased = scope_of("/traces/erase.pfw");
        const ScopeKey dropped = scope_of("/traces/drop.pfw");
        const ScopeKey failed = scope_of("/traces/fail.pfw");
        put_all(store, ns, erased, {{"a", "1"}, {"b", "2"}, {"c", "3"}});
        put_all(store, ns, dropped, {{"a", "1"}});
        REQUIRE(run(store.flush()).has_value());
        CHECK(get_value(store, ns, dropped, "a").has_value());

        const dstd::String begin = "b", end = "c";
        REQUIRE(
            store.erase(ns, erased, view_of(begin), view_of(end)).has_value());
        REQUIRE(store.drop(ns, dropped).has_value());
        REQUIRE(store.fail(ns, failed, 11).has_value());
        REQUIRE(run(store.flush()).has_value());

        CHECK(get_value(store, ns, erased, "a").has_value());
        CHECK_FALSE(get_value(store, ns, erased, "b").has_value());
        CHECK(get_value(store, ns, erased, "c").has_value());
        CHECK_FALSE(get_value(store, ns, dropped, "a").has_value());

        auto snapshot = store.snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->copy_info(ns, dropped).state == ScopeState::Absent);
        const store::CopyInfo info = snapshot->copy_info(ns, failed);
        CHECK(info.state == ScopeState::Failed);
        CHECK(info.params_hash == 11);
    }

    TEST_CASE("erase refuses a copy another writer wrote") {
        dftu_utils_test::ScopedTestDir dir("store_api_erase_foreign");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.erase"));
        const ScopeKey written = scope_of("/traces/written.pfw");
        const ScopeKey absent = scope_of("/traces/absent.pfw");
        put_all(store, ns, written, {{"a", "1"}, {"b", "2"}});
        REQUIRE(run(store.close()).has_value());

        const Namespace again = register_ns(store, spec_of("dftu.test.erase"));
        const dstd::String begin = "a", end = "c";
        auto refused =
            store.erase(again, written, view_of(begin), view_of(end));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().condition() == dstd::Condition::Unsupported);
        CHECK(store.erase(again, absent, view_of(begin), view_of(end))
                  .has_value());
        REQUIRE(run(store.flush()).has_value());
        CHECK(get_value(store, again, written, "a") ==
              dstd::Optional<dstd::String>("1"));
        CHECK(get_value(store, again, written, "b") ==
              dstd::Optional<dstd::String>("2"));
    }

    TEST_CASE("a write after close opens a new writer") {
        dftu_utils_test::ScopedTestDir dir("store_api_close");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.close"));
        const ScopeKey scope = scope_of("/traces/a.pfw");

        const ScopeKey kept = scope_of("/traces/kept.pfw");

        put_all(store, ns, kept, {{"before", "1"}});
        REQUIRE(run(store.close()).has_value());
        CHECK(get_value(store, ns, kept, "before") ==
              dstd::Optional<dstd::String>("1"));

        put_all(store, ns, scope, {{"after", "2"}});
        REQUIRE(run(store.flush()).has_value());
        CHECK(get_value(store, ns, kept, "before") ==
              dstd::Optional<dstd::String>("1"));
        CHECK(get_value(store, ns, scope, "after") ==
              dstd::Optional<dstd::String>("2"));

        const Namespace later = register_ns(store, spec_of("dftu.test.later"));
        put_all(store, later, scope, {{"x", "y"}});
        REQUIRE(run(store.flush()).has_value());
        CHECK(get_value(store, later, scope, "x") ==
              dstd::Optional<dstd::String>("y"));

        REQUIRE(Store::close_all().has_value());
        put_all(store, ns, scope, {{"again", "3"}});
        REQUIRE(run(store.flush()).has_value());
        CHECK(get_value(store, ns, scope, "again") ==
              dstd::Optional<dstd::String>("3"));
        REQUIRE(run(store.close()).has_value());
    }

    TEST_CASE("a snapshot follows the group commit of the commit interval") {
        dftu_utils_test::ScopedTestDir dir("store_api_interval");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.interval"));
        const ScopeKey scope = scope_of("/traces/a.pfw");

        // Enough bytes to seal extents on their own, so the committer records
        // them at its interval with no flush.
        Rows rows;
        for (int i = 0; i < 512; ++i) {
            const dstd::String key = "k" + dstd::to_string(1000 + i);
            rows.emplace_back(
                key,
                dstd::String(16 * 1024, static_cast<char>('a' + (i % 26))));
        }
        put_all(store, ns, scope, rows);
        dstd::Optional<dstd::String> seen =
            get_value(store, ns, scope, "k1000");
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!seen && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            seen = get_value(store, ns, scope, "k1000");
        }
        CHECK(seen == dstd::Optional<dstd::String>(rows[0].second));
    }

    TEST_CASE("a batch made before a close is refused after it") {
        dftu_utils_test::ScopedTestDir dir("store_api_stale_batch");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.stale"));
        const ScopeKey scope = scope_of("/traces/a.pfw");

        Batch stale = store.batch(ns, scope);
        stale.put(view_of("k"), view_of("v"));
        REQUIRE(run(store.close()).has_value());
        CHECK_FALSE(run(store.commit(std::move(stale))).has_value());

        put_all(store, ns, scope, {{"k", "fresh"}});
        REQUIRE(run(store.flush()).has_value());
        CHECK(get_value(store, ns, scope, "k") ==
              dstd::Optional<dstd::String>("fresh"));
    }

    TEST_CASE("a directory reopens after every handle is gone") {
        dftu_utils_test::ScopedTestDir dir("store_api_reopen");
        const NamespaceSpec spec = spec_of("dftu.test.reopen");
        const ScopeKey scope = scope_of("/traces/a.pfw");
        {
            Store store = open_store(dir);
            const Namespace ns = register_ns(store, spec);
            put_all(store, ns, scope, {{"k", "v"}});
            REQUIRE(run(store.flush()).has_value());
        }
        Store again = open_store(dir);
        const Namespace ns = register_ns(again, spec);
        CHECK(get_value(again, ns, scope, "k") ==
              dstd::Optional<dstd::String>("v"));
        put_all(again, ns, scope, {{"k2", "v2"}});
        REQUIRE(run(again.flush()).has_value());
        CHECK(get_value(again, ns, scope, "k2") ==
              dstd::Optional<dstd::String>("v2"));
    }

    TEST_CASE("a data file another writer adds is picked up") {
        dftu_utils_test::ScopedTestDir dir("store_api_foreign");
        Store store = open_store(dir);
        const NamespaceSpec spec = spec_of("dftu.test.foreign");
        const Namespace ns = register_ns(store, spec);
        const ScopeKey own = scope_of("/traces/own.pfw");
        const ScopeKey other = scope_of("/traces/other.pfw");

        put_all(store, ns, own, {{"k", "own"}});
        REQUIRE(run(store.flush()).has_value());
        CHECK_FALSE(get_value(store, ns, other, "k").has_value());

        auto created = store::engine::Writer::create(
            dstd::StringView(dir.str().data(), dir.str().size()),
            *dftracer::utils::default_runtime().executor());
        REQUIRE(created.has_value());
        auto& writer = **created;
        auto id = writer.add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer.batch(*id, other, 0);
        batch.put(view_of("k"), view_of("foreign"));
        REQUIRE(writer.commit(std::move(batch)).has_value());
        REQUIRE(dftracer::utils::default_runtime()
                    .submit(writer.close())
                    .get()
                    .has_value());

        CHECK(get_value(store, ns, other, "k") ==
              dstd::Optional<dstd::String>("foreign"));
        CHECK(get_value(store, ns, own, "k") ==
              dstd::Optional<dstd::String>("own"));
    }

    TEST_CASE("flush and close finish when the caller drives them itself") {
        // get() on a thread with no executor drives the task on a RunLoop,
        // which only waits when the store's waits register as parked.
        dftu_utils_test::ScopedTestDir dir("store_api_run_loop");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.test.loop"));
        const ScopeKey scope = ScopeKey::file("/traces/loop.pfw");
        for (int round = 0; round < 4; ++round) {
            Batch batch = store.batch(ns, scope);
            for (int i = 0; i < 256; ++i) {
                const dstd::String key =
                    "k" + dstd::to_string(round * 1000 + i);
                const dstd::String value(4096, static_cast<char>('a' + round));
                batch.put(view_of(key), view_of(value));
            }
            REQUIRE(store.commit(std::move(batch)).get().has_value());
            REQUIRE(store.flush().get().has_value());
        }
        CHECK(get_value(store, ns, scope, "k3000") ==
              dstd::Optional<dstd::String>(dstd::String(4096, 'd')));
        REQUIRE(store.close().get().has_value());
    }
}
