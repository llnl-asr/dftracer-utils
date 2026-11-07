// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/store/writer.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;

namespace {

using namespace dftracer::utils::store;

using Rows = dstd::Vector<dstd::Pair<dstd::String, dstd::String>>;

constexpr std::size_t BIG_VALUE_BYTES = 2 * 1024 * 1024;

dstd::ByteView view_of(const dstd::String& bytes) {
    return {bytes.data(), bytes.size()};
}

dstd::String to_string(dstd::ByteView bytes) {
    return dstd::String(reinterpret_cast<const char*>(bytes.data()),
                        bytes.size());
}

NamespaceSpec spec_of(const dstd::String& name,
                      FoldKind fold = FoldKind::None) {
    NamespaceSpec spec;
    spec.name = name;
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

template <typename T>
dstd::Expected<T, dstd::Error> run(
    coro::CoroTask<dstd::Expected<T, dstd::Error>> task) {
    return default_runtime().submit(std::move(task)).get();
}

void put_all(Store& store, const Namespace& ns, const ScopeKey& scope,
             const Rows& rows, std::uint64_t params = 0,
             const dstd::Vector<SourceStat>* sources = nullptr) {
    Batch batch = store.batch(ns, scope, params, sources);
    for (const auto& [key, value] : rows) {
        batch.put(view_of(key), view_of(value));
    }
    REQUIRE(run(store.commit(std::move(batch))).has_value());
}

void close_writer(Store& store) { REQUIRE(run(store.close()).has_value()); }

Rows rows_of(Store& store, const Namespace& ns, const ScopeKey& scope) {
    auto snapshot = store.snapshot();
    REQUIRE(snapshot.has_value());
    const dstd::String none;
    Cursor cursor = snapshot->scan(ns, scope, view_of(none), view_of(none));
    Rows rows;
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        rows.emplace_back(to_string(cursor.key()), to_string(*value));
        cursor.next();
    }
    REQUIRE_FALSE(cursor.error().has_value());
    return rows;
}

std::size_t data_files(const dftu_utils_test::ScopedTestDir& dir) {
    std::size_t count = 0;
    for (const auto& entry : dftu_utils_test::list_dir(dir.str())) {
        if (entry.path.view().extension() == ".dfs") ++count;
    }
    return count;
}

std::uint64_t count_of(const dstd::String& value) {
    REQUIRE(value.size() == sizeof(std::uint64_t));
    return dstd::read_le<std::uint64_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()));
}

PackOptions no_keep() {
    PackOptions options;
    options.keep_seconds = 0;
    return options;
}

}  // namespace

TEST_SUITE("store pack") {
    TEST_CASE("pack merges small files and a reader sees the same data") {
        dftu_utils_test::ScopedTestDir dir("store_pack_merge");
        Store store = open_store(dir);
        const Namespace plain = register_ns(store, spec_of("dftu.pack.plain"));
        const Namespace counts =
            register_ns(store, spec_of("dftu.pack.count", FoldKind::Count));
        const Namespace sums =
            register_ns(store, spec_of("dftu.pack.sum", FoldKind::SumI64));
        const ScopeKey erased = scope_of("/t/erased.pfw");
        const ScopeKey big = scope_of("/t/big.pfw");
        const ScopeKey empty = scope_of("/t/empty.pfw");
        const ScopeKey dropped = scope_of("/t/dropped.pfw");
        const ScopeKey failed = scope_of("/t/failed.pfw");
        const ScopeKey rewritten = scope_of("/t/rewritten.pfw");
        const ScopeKey folded = scope_of("/t/folded.pfw");
        const dstd::Vector<SourceStat> sources{SourceStat{1, 2, 3}};

        put_all(store, plain, erased, {{"a", "1"}, {"b", "2"}, {"c", "3"}}, 7,
                &sources);
        Batch erase = store.batch(plain, erased, 7, &sources);
        const dstd::String b = "b";
        erase.erase(view_of(b));
        REQUIRE(run(store.commit(std::move(erase))).has_value());
        put_all(store, plain, dropped, {{"a", "gone"}});
        put_all(store, plain, rewritten, {{"old", "1"}});
        close_writer(store);

        const dstd::String big_value(BIG_VALUE_BYTES, 'x');
        put_all(store, plain, big, {{"big", big_value}, {"small", "s"}});
        put_all(store, plain, empty, {});
        REQUIRE(store.drop(plain, dropped).has_value());
        REQUIRE(store.fail(plain, failed, 11).has_value());
        close_writer(store);

        REQUIRE(store.drop(plain, rewritten).has_value());
        put_all(store, plain, rewritten, {{"new", "2"}});
        const dstd::String key = "k", other = "o";
        std::uint64_t amount = 5;
        for (int round = 0; round < 3; ++round) {
            Batch count = store.batch(counts, folded);
            count.fold(view_of(key), view_of(key));
            REQUIRE(run(store.commit(std::move(count))).has_value());
            Batch sum = store.batch(sums, folded);
            sum.fold(view_of(other),
                     dstd::ByteView(reinterpret_cast<const char*>(&amount),
                                    sizeof(amount)));
            REQUIRE(run(store.commit(std::move(sum))).has_value());
        }
        close_writer(store);
        CHECK(data_files(dir) == 3);

        const Rows erased_rows = rows_of(store, plain, erased);
        const Rows big_rows = rows_of(store, plain, big);
        const Rows rewritten_rows = rows_of(store, plain, rewritten);
        const Rows count_rows = rows_of(store, counts, folded);
        const Rows sum_rows = rows_of(store, sums, folded);
        CHECK(erased_rows.size() == 2);
        CHECK(count_rows.size() == 1);

        auto before = store.snapshot();
        REQUIRE(before.has_value());
        const std::uint64_t old_generation =
            before->copy_info(plain, erased).generation;

        auto report = run(store.pack(no_keep()));
        REQUIRE(report.has_value());
        CHECK(report->files_packed == 3);
        CHECK(report->copies_skipped == 0);
        CHECK(report->files_deleted == 3);
        CHECK(report->bytes_deleted > 0);
        CHECK(data_files(dir) == 1);

        CHECK(rows_of(store, plain, erased) == erased_rows);
        CHECK(rows_of(store, plain, big) == big_rows);
        CHECK(rows_of(store, plain, rewritten) == rewritten_rows);
        CHECK(rows_of(store, counts, folded) == count_rows);
        CHECK(rows_of(store, sums, folded) == sum_rows);
        CHECK(rows_of(store, plain, empty).empty());

        auto after = store.snapshot();
        REQUIRE(after.has_value());
        const CopyInfo info = after->copy_info(plain, erased);
        CHECK(info.state == ScopeState::Ready);
        CHECK(info.generation > old_generation);
        CHECK(info.params_hash == 7);
        REQUIRE(info.sources.size() == 1);
        CHECK(info.sources[0].hash == 3);
        CHECK(after->copy_info(plain, dropped).state == ScopeState::Absent);
        CHECK(after->copy_info(plain, empty).state == ScopeState::Absent);
        const CopyInfo failed_info = after->copy_info(plain, failed);
        CHECK(failed_info.state == ScopeState::Failed);
        CHECK(failed_info.params_hash == 11);

        const std::uint64_t packed_generation = info.generation;
        Batch later = store.batch(plain, scope_of("/t/later.pfw"));
        later.put(view_of(key), view_of(key));
        REQUIRE(run(store.commit(std::move(later))).has_value());
        REQUIRE(store.drop(plain, erased).has_value());
        put_all(store, plain, erased, {{"z", "9"}});
        close_writer(store);
        auto next = store.snapshot();
        REQUIRE(next.has_value());
        CHECK(next->copy_info(plain, erased).generation > packed_generation);
        CHECK(rows_of(store, plain, erased) == Rows{{"z", "9"}});
    }

    TEST_CASE("a counted fold keeps its count through a pack") {
        dftu_utils_test::ScopedTestDir dir("store_pack_fold");
        Store store = open_store(dir);
        const Namespace counts =
            register_ns(store, spec_of("dftu.pack.c", FoldKind::Count));
        const ScopeKey scope = scope_of("/t/a.pfw");
        const dstd::String key = "k";
        for (int round = 0; round < 2; ++round) {
            Batch batch = store.batch(counts, scope);
            batch.fold(view_of(key), view_of(key));
            REQUIRE(run(store.commit(std::move(batch))).has_value());
        }
        close_writer(store);
        put_all(store, register_ns(store, spec_of("dftu.pack.p")),
                scope_of("/t/b.pfw"), {{"a", "1"}});
        close_writer(store);
        auto report = run(store.pack(no_keep()));
        REQUIRE(report.has_value());
        REQUIRE(report->copies_written == 2);
        const Rows after = rows_of(store, counts, scope);
        REQUIRE(after.size() == 1);
        CHECK(count_of(after[0].second) == 2);
    }

    TEST_CASE("a replaced file stays for the keep time") {
        dftu_utils_test::ScopedTestDir dir("store_pack_keep");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.pack.keep"));
        const ScopeKey a = scope_of("/t/a.pfw");
        const ScopeKey c = scope_of("/t/c.pfw");
        put_all(store, ns, a, {{"k", "1"}});
        close_writer(store);
        put_all(store, ns, c, {{"k", "2"}});
        close_writer(store);

        auto held = store.snapshot();
        REQUIRE(held.has_value());
        auto first = run(store.pack());
        REQUIRE(first.has_value());
        CHECK(first->files_packed == 2);
        CHECK(first->files_deleted == 0);
        CHECK(data_files(dir) == 3);

        const dstd::String key = "k";
        auto old_read = held->get(ns, a, view_of(key));
        REQUIRE(old_read.has_value());
        REQUIRE(old_read->has_value());
        CHECK(to_string((*old_read)->bytes()) == "1");
        CHECK(rows_of(store, ns, a) == Rows{{"k", "1"}});

        auto second = run(store.pack(no_keep()));
        REQUIRE(second.has_value());
        CHECK(second->files_packed == 0);
        CHECK(second->copies_written == 0);
        CHECK(second->files_deleted == 2);
        CHECK(data_files(dir) == 1);
        CHECK(rows_of(store, ns, a) == Rows{{"k", "1"}});
        CHECK(rows_of(store, ns, c) == Rows{{"k", "2"}});
    }

    TEST_CASE("one small file alone stays as it is") {
        dftu_utils_test::ScopedTestDir dir("store_pack_one");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.pack.one"));
        put_all(store, ns, scope_of("/t/a.pfw"), {{"k", "1"}});
        close_writer(store);
        auto report = run(store.pack(no_keep()));
        REQUIRE(report.has_value());
        CHECK(report->files_packed == 0);
        CHECK(report->files_deleted == 0);
        CHECK(data_files(dir) == 1);
    }

    TEST_CASE("a file without a footer stays and keeps its scope") {
        dftu_utils_test::ScopedTestDir dir("store_pack_open");
        Store store = open_store(dir);
        const NamespaceSpec spec = spec_of("dftu.pack.open");
        const Namespace ns = register_ns(store, spec);
        const ScopeKey closed_a = scope_of("/t/a.pfw");
        const ScopeKey closed_b = scope_of("/t/b.pfw");
        const ScopeKey still_open = scope_of("/t/open.pfw");
        put_all(store, ns, closed_a, {{"k", "1"}});
        close_writer(store);
        put_all(store, ns, closed_b, {{"k", "2"}});
        close_writer(store);

        auto other =
            engine::Writer::create(dir.str(), *default_runtime().executor());
        REQUIRE(other.has_value());
        auto id = (*other)->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = (*other)->batch(*id, still_open, 0, nullptr);
        const dstd::String key = "k", value = "live";
        batch.put(view_of(key), view_of(value));
        REQUIRE((*other)->commit(std::move(batch)).has_value());
        REQUIRE(run((*other)->flush()).has_value());
        const std::size_t before = data_files(dir);

        auto report = run(store.pack(no_keep()));
        REQUIRE(report.has_value());
        CHECK(report->files_packed == 2);
        CHECK(report->files_deleted == 2);
        CHECK(data_files(dir) == before - 1);
        CHECK(rows_of(store, ns, still_open) == Rows{{"k", "live"}});
        CHECK(rows_of(store, ns, closed_a) == Rows{{"k", "1"}});

        REQUIRE(run((*other)->close()).has_value());
    }

    TEST_CASE("a file without a footer does not replace a closed file") {
        dftu_utils_test::ScopedTestDir dir("store_pack_crashed");
        Store store = open_store(dir);
        const NamespaceSpec spec = spec_of("dftu.pack.crash");
        const Namespace ns = register_ns(store, spec);
        const ScopeKey scope = scope_of("/t/a.pfw");
        put_all(store, ns, scope, {{"k", "complete"}});
        close_writer(store);

        auto other =
            engine::Writer::create(dir.str(), *default_runtime().executor());
        REQUIRE(other.has_value());
        auto id = (*other)->add_namespace(spec);
        REQUIRE(id.has_value());
        REQUIRE((*other)->drop(*id, scope).has_value());
        Batch newer = (*other)->batch(*id, scope, 0, nullptr);
        const dstd::String key = "k", value = "newer";
        newer.put(view_of(key), view_of(value));
        REQUIRE((*other)->commit(std::move(newer)).has_value());
        REQUIRE(run((*other)->flush()).has_value());
        const std::size_t before = data_files(dir);

        auto held = run(store.pack(no_keep()));
        REQUIRE(held.has_value());
        CHECK(held->files_deleted == 0);
        CHECK(data_files(dir) == before);
        CHECK(rows_of(store, ns, scope) == Rows{{"k", "newer"}});

        REQUIRE(run((*other)->close()).has_value());
        auto freed = run(store.pack(no_keep()));
        REQUIRE(freed.has_value());
        CHECK(freed->files_deleted == 1);
        CHECK(data_files(dir) == before - 1);
        CHECK(rows_of(store, ns, scope) == Rows{{"k", "newer"}});
    }

    TEST_CASE("a namespace this process registered differently is left alone") {
        dftu_utils_test::ScopedTestDir dir("store_pack_skip");
        const ScopeKey old_scope = scope_of("/t/old.pfw");
        NamespaceSpec old_spec = spec_of("dftu.pack.skip");
        old_spec.format_version = 1;
        {
            Store first = open_store(dir);
            put_all(first, register_ns(first, old_spec), old_scope,
                    {{"k", "v1"}});
            close_writer(first);
        }
        Store store = open_store(dir);
        NamespaceSpec new_spec = spec_of("dftu.pack.skip");
        new_spec.format_version = 2;
        const Namespace ns = register_ns(store, new_spec);
        put_all(store, ns, scope_of("/t/new.pfw"), {{"k", "v2"}});
        close_writer(store);

        auto report = run(store.pack(no_keep()));
        REQUIRE(report.has_value());
        CHECK(report->copies_skipped == 1);
        CHECK(report->copies_written == 1);
        CHECK(report->files_deleted == 1);
        CHECK(data_files(dir) == 2);
        auto view = store.snapshot();
        REQUIRE(view.has_value());
        const Namespace old_handle{old_spec, ns.id};
        const dstd::String key = "k";
        auto kept = view->get(old_handle, old_scope, view_of(key));
        REQUIRE(kept.has_value());
        REQUIRE(kept->has_value());
        CHECK(to_string((*kept)->bytes()) == "v1");
    }

    TEST_CASE("a large file is rewritten only once it is mostly dead") {
        dftu_utils_test::ScopedTestDir dir("store_pack_dead");
        Store store = open_store(dir);
        const Namespace ns = register_ns(store, spec_of("dftu.pack.dead"));
        const dstd::String a_value(4096, 'a'), b_value(4096, 'b');
        const dstd::String c_value(4096, 'c');
        const ScopeKey a = scope_of("/t/a.pfw");
        const ScopeKey b = scope_of("/t/b.pfw");
        const ScopeKey c = scope_of("/t/c.pfw");
        put_all(store, ns, a, {{"k", a_value}});
        put_all(store, ns, b, {{"k", b_value}});
        put_all(store, ns, c, {{"k", c_value}});
        close_writer(store);

        PackOptions all_large = no_keep();
        all_large.small_file_bytes = 1;
        auto untouched = run(store.pack(all_large));
        REQUIRE(untouched.has_value());
        CHECK(untouched->files_packed == 0);
        CHECK(data_files(dir) == 1);

        REQUIRE(store.drop(ns, a).has_value());
        put_all(store, ns, a, {{"k", "new"}});
        close_writer(store);
        auto one_third_dead = run(store.pack(all_large));
        REQUIRE(one_third_dead.has_value());
        CHECK(one_third_dead->files_packed == 0);
        CHECK(data_files(dir) == 2);

        REQUIRE(store.drop(ns, b).has_value());
        put_all(store, ns, b, {{"k", "new"}});
        close_writer(store);
        auto mostly_dead = run(store.pack(all_large));
        REQUIRE(mostly_dead.has_value());
        CHECK(mostly_dead->files_packed >= 1);
        CHECK(mostly_dead->files_deleted >= 1);
        CHECK(rows_of(store, ns, a) == Rows{{"k", "new"}});
        CHECK(rows_of(store, ns, b) == Rows{{"k", "new"}});
        CHECK(rows_of(store, ns, c) == Rows{{"k", c_value}});
    }
}
