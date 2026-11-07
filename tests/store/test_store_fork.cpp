// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <utility>

using namespace dftracer::utils;

namespace {

using namespace dftracer::utils::store;

#if defined(__SANITIZE_THREAD__)
constexpr bool TSAN_BUILD = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool TSAN_BUILD = true;
#else
constexpr bool TSAN_BUILD = false;
#endif
#else
constexpr bool TSAN_BUILD = false;
#endif

/// A child that hangs on a thread of the parent fails the test instead of
/// holding it.
constexpr unsigned CHILD_LIMIT_SECONDS = 120;

dstd::ByteView view_of(const dstd::String& bytes) {
    return {bytes.data(), bytes.size()};
}

NamespaceSpec spec() {
    NamespaceSpec made;
    made.name = "dftu.test.fork";
    return made;
}

NamespaceSpec shared_spec() {
    NamespaceSpec made;
    made.name = "dftu.test.shared";
    made.shared = true;
    return made;
}

ScopeKey scope_of(const dstd::String& path) {
    return ScopeKey::file(dstd::StringView(path.data(), path.size()));
}

dstd::Expected<void, dstd::Error> run(
    coro::CoroTask<dstd::Expected<void, dstd::Error>> task) {
    return default_runtime().submit(std::move(task)).get();
}

dstd::Expected<void, dstd::Error> put(Store& store, const Namespace& ns,
                                      const ScopeKey& scope,
                                      const dstd::String& key,
                                      const dstd::String& value) {
    Batch batch = store.batch(ns, scope);
    batch.put(view_of(key), view_of(value));
    return run(store.commit(std::move(batch)));
}

dstd::Expected<PackReport, dstd::Error> run_pack(Store& store) {
    return default_runtime().submit(store.pack()).get();
}

template <typename T>
bool is_forked(const dstd::Expected<T, dstd::Error>& result) {
    return !result.has_value() &&
           result.error().condition() == dstd::Condition::ForkedHandle;
}

/// Exit codes name the check that failed, for the parent's report.
int check_inherited(Store& inherited, const Namespace& ns,
                    const ScopeKey& scope) {
    if (!is_forked(inherited.ns(spec()))) return 10;
    if (!is_forked(inherited.snapshot())) return 11;
    if (!is_forked(inherited.refresh())) return 29;
    if (!is_forked(inherited.drop(ns, scope))) return 12;
    if (!is_forked(inherited.fail(ns, scope, 1))) return 13;
    const dstd::String key = "k";
    if (!is_forked(inherited.erase(ns, scope, view_of(key), {}))) return 14;
    if (!is_forked(run(inherited.flush()))) return 15;
    if (!is_forked(run(inherited.close()))) return 16;
    if (!is_forked(put(inherited, ns, scope, key, key))) return 17;
    if (!is_forked(run_pack(inherited))) return 18;
    Batch building = inherited.building_batch(ns, scope);
    if (!is_forked(run(inherited.commit(std::move(building))))) return 19;
    if (!is_forked(run(inherited.publish({})))) return 27;
    return 0;
}

int child_writes_own_copy(Store& inherited, const Namespace& ns,
                          const dstd::String& dir) {
    const ScopeKey parent_scope = scope_of("/traces/parent.pfw");
    if (const int failed = check_inherited(inherited, ns, parent_scope)) {
        return failed;
    }
    const dstd::String empty = dir + "/nothing_to_adopt";
    auto adopted = Store::adopt(dstd::StringView(empty.data(), empty.size()),
                                dstd::StringView(dir.data(), dir.size()));
    if (adopted && *adopted != 0) return 28;
    auto opened = Store::open(dstd::StringView(dir.data(), dir.size()));
    if (!opened) return 20;
    auto own_ns = opened->ns(spec());
    if (!own_ns) return 21;
    if (!put(*opened, *own_ns, scope_of("/traces/child.pfw"), "k", "child")) {
        return 22;
    }
    if (!run(opened->close())) return 23;
    auto view = opened->snapshot();
    if (!view) return 24;
    const dstd::String key = "k";
    auto own = view->get(*own_ns, scope_of("/traces/child.pfw"), view_of(key));
    if (!own || !own->has_value()) return 25;
    auto parents = view->get(*own_ns, parent_scope, view_of(key));
    if (!parents || !parents->has_value()) return 26;
    return 0;
}

int wait_for(pid_t pid) {
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    REQUIRE_MESSAGE(WIFEXITED(status), "the child died or hung: ", status);
    return WEXITSTATUS(status);
}

dstd::String key_of(int index) { return common::str_cat("artifact-", index); }

/// Writes its own share of the keys and each key that all processes share.
int child_writes_keys(const dstd::String& dir, const NamespaceSpec& made,
                      int process, int process_count, int key_count) {
    auto opened = Store::open(dstd::StringView(dir.data(), dir.size()));
    if (!opened) return 30;
    auto own_ns = opened->ns(made);
    if (!own_ns) return 31;
    for (int i = 0; i < key_count; ++i) {
        if (i % 5 != 0 && i % process_count != process) continue;
        const dstd::String key = key_of(i);
        if (!put(*opened, *own_ns, ScopeKey::root(), key, key)) return 32;
    }
    return run(opened->close()) ? 0 : 33;
}

constexpr int PROCESSES = 4;
constexpr int KEYS = 100;

void write_from_processes(const dstd::String& dir, const NamespaceSpec& made) {
    pid_t children[PROCESSES];
    for (int p = 0; p < PROCESSES; ++p) {
        children[p] = ::fork();
        REQUIRE(children[p] >= 0);
        if (children[p] == 0) {
            ::alarm(CHILD_LIMIT_SECONDS);
            ::_exit(child_writes_keys(dir, made, p, PROCESSES, KEYS));
        }
    }
    for (const pid_t child : children) CHECK(wait_for(child) == 0);
}

/// Reads with a handle of its own in a child. The exit code is the number of
/// keys found, or above KEYS on a failure.
int count_keys_in_child(const dstd::String& dir, const NamespaceSpec& made) {
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid != 0) return wait_for(pid);
    ::alarm(CHILD_LIMIT_SECONDS);
    auto opened = Store::open(dstd::StringView(dir.data(), dir.size()));
    if (!opened) ::_exit(KEYS + 1);
    auto ns = opened->ns(made);
    if (!ns) ::_exit(KEYS + 2);
    auto view = opened->snapshot();
    if (!view) ::_exit(KEYS + 3);
    int found_count = 0;
    for (int i = 0; i < KEYS; ++i) {
        const dstd::String key = key_of(i);
        auto found = view->get(*ns, ScopeKey::root(), view_of(key));
        if (!found) ::_exit(KEYS + 4);
        if (!found->has_value()) continue;
        const dstd::ByteView bytes = (*found)->bytes();
        if (dstd::String(reinterpret_cast<const char*>(bytes.data()),
                         bytes.size()) != key) {
            ::_exit(KEYS + 5);
        }
        ++found_count;
    }
    int scanned = 0;
    Cursor cursor =
        view->scan(*ns, ScopeKey::root(), dstd::ByteView{}, dstd::ByteView{});
    dstd::String last;
    for (; !cursor.at_end(); cursor.next()) {
        const dstd::ByteView at = cursor.key();
        dstd::String here(reinterpret_cast<const char*>(at.data()), at.size());
        if (scanned > 0 && !(last < here)) ::_exit(KEYS + 6);
        last = std::move(here);
        ++scanned;
    }
    if (scanned != found_count) ::_exit(KEYS + 7);
    ::_exit(found_count);
}

}  // namespace

TEST_CASE(
    "an inherited handle refuses every call and the child opens its own") {
    if (TSAN_BUILD) return;
    dftu_utils_test::ScopedTestDir dir("store_fork_own");
    auto opened =
        Store::open(dstd::StringView(dir.str().data(), dir.str().size()));
    REQUIRE(opened.has_value());
    auto ns = opened->ns(spec());
    REQUIRE(ns.has_value());
    const ScopeKey parent_scope = scope_of("/traces/parent.pfw");
    REQUIRE(put(*opened, *ns, parent_scope, "k", "parent").has_value());
    REQUIRE(run(opened->flush()).has_value());

    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::alarm(CHILD_LIMIT_SECONDS);
        ::_exit(child_writes_own_copy(*opened, *ns, dir.str()));
    }
    CHECK(wait_for(pid) == 0);

    REQUIRE(put(*opened, *ns, parent_scope, "k2", "still").has_value());
    REQUIRE(run(opened->flush()).has_value());
    auto view = opened->snapshot();
    REQUIRE(view.has_value());
    const dstd::String mine = "k2", theirs = "k";
    auto kept = view->get(*ns, parent_scope, view_of(mine));
    REQUIRE(kept.has_value());
    CHECK(kept->has_value());
    auto child = view->get(*ns, scope_of("/traces/child.pfw"), view_of(theirs));
    REQUIRE(child.has_value());
    CHECK(child->has_value());
}

TEST_CASE("a child skips an inherited handle at close and when it drops it") {
    if (TSAN_BUILD) return;
    dftu_utils_test::ScopedTestDir dir("store_fork_exit");
    auto opened =
        Store::open(dstd::StringView(dir.str().data(), dir.str().size()));
    REQUIRE(opened.has_value());
    auto ns = opened->ns(spec());
    REQUIRE(ns.has_value());
    REQUIRE(put(*opened, *ns, scope_of("/traces/parent.pfw"), "k", "v")
                .has_value());

    const pid_t exits = ::fork();
    REQUIRE(exits >= 0);
    if (exits == 0) {
        ::alarm(CHILD_LIMIT_SECONDS);
        ::_exit(Store::close_all().has_value() ? 0 : 1);
    }
    CHECK(wait_for(exits) == 0);

    const pid_t drops = ::fork();
    REQUIRE(drops >= 0);
    if (drops == 0) {
        ::alarm(CHILD_LIMIT_SECONDS);
        {
            Store gone = std::move(*opened);
        }
        ::_exit(0);
    }
    CHECK(wait_for(drops) == 0);

    REQUIRE(run(opened->close()).has_value());
    auto view = opened->snapshot();
    REQUIRE(view.has_value());
    const dstd::String key = "k";
    auto found = view->get(*ns, scope_of("/traces/parent.pfw"), view_of(key));
    REQUIRE(found.has_value());
    CHECK(found->has_value());
}

TEST_CASE("a shared namespace keeps the keys of every process") {
    if (TSAN_BUILD) return;
    dftu_utils_test::ScopedTestDir dir("store_fork_shared");
    write_from_processes(dir.str(), shared_spec());
    CHECK(count_keys_in_child(dir.str(), shared_spec()) == KEYS);
}

TEST_CASE("a plain namespace keeps the newest writer of a scope") {
    if (TSAN_BUILD) return;
    dftu_utils_test::ScopedTestDir dir("store_fork_plain");
    write_from_processes(dir.str(), spec());
    CHECK(count_keys_in_child(dir.str(), spec()) < KEYS);
}

TEST_CASE("pack keeps a shared scope whole and a later writer adds to it") {
    if (TSAN_BUILD) return;
    dftu_utils_test::ScopedTestDir dir("store_fork_shared_pack");
    write_from_processes(dir.str(), shared_spec());

    const pid_t packer = ::fork();
    REQUIRE(packer >= 0);
    if (packer == 0) {
        ::alarm(CHILD_LIMIT_SECONDS);
        auto opened =
            Store::open(dstd::StringView(dir.str().data(), dir.str().size()));
        if (!opened) ::_exit(40);
        if (!opened->ns(shared_spec())) ::_exit(41);
        PackOptions options;
        options.keep_seconds = 0;
        auto report = default_runtime().submit(opened->pack(options)).get();
        if (!report) ::_exit(42);
        if (report->files_packed != PROCESSES) ::_exit(43);
        if (report->copies_written != 1) ::_exit(44);
        ::_exit(run(opened->close()) ? 0 : 45);
    }
    CHECK(wait_for(packer) == 0);
    CHECK(count_keys_in_child(dir.str(), shared_spec()) == KEYS);

    const pid_t later = ::fork();
    REQUIRE(later >= 0);
    if (later == 0) {
        ::alarm(CHILD_LIMIT_SECONDS);
        auto opened =
            Store::open(dstd::StringView(dir.str().data(), dir.str().size()));
        if (!opened) ::_exit(50);
        auto ns = opened->ns(shared_spec());
        if (!ns) ::_exit(51);
        if (!put(*opened, *ns, ScopeKey::root(), "later", "later")) ::_exit(52);
        if (!run(opened->close())) ::_exit(53);
        auto view = opened->snapshot();
        if (!view) ::_exit(54);
        const dstd::String added = "later", old = key_of(1);
        auto new_key = view->get(*ns, ScopeKey::root(), view_of(added));
        auto old_key = view->get(*ns, ScopeKey::root(), view_of(old));
        if (!new_key || !new_key->has_value()) ::_exit(55);
        if (!old_key || !old_key->has_value()) ::_exit(56);
        ::_exit(0);
    }
    CHECK(wait_for(later) == 0);
}

TEST_CASE(
    "a shared namespace refuses a fold and erases, and a drop clears it") {
    dftu_utils_test::ScopedTestDir dir("store_shared_rules");
    auto opened =
        Store::open(dstd::StringView(dir.str().data(), dir.str().size()));
    REQUIRE(opened.has_value());
    NamespaceSpec folded = shared_spec();
    folded.name = "dftu.test.shared_fold";
    folded.fold = FoldKind::Last;
    CHECK(opened->ns(folded).error().is(dstd::Condition::InvalidArgument));

    auto ns = opened->ns(shared_spec());
    REQUIRE(ns.has_value());
    REQUIRE(put(*opened, *ns, ScopeKey::root(), "k", "v").has_value());
    const dstd::String begin = "a", end = "z";
    CHECK(opened->erase(*ns, ScopeKey::root(), view_of(begin), view_of(end))
              .error()
              .is(dstd::Condition::Unsupported));
    Batch batch = opened->batch(*ns, ScopeKey::root());
    const dstd::String key = "k";
    batch.erase(view_of(key));
    CHECK(run(opened->commit(std::move(batch)))
              .error()
              .is(dstd::Condition::Unsupported));

    REQUIRE(opened->drop(*ns, ScopeKey::root()).has_value());
    REQUIRE(run(opened->flush()).has_value());
    auto view = opened->snapshot();
    REQUIRE(view.has_value());
    auto gone = view->get(*ns, ScopeKey::root(), view_of(key));
    REQUIRE(gone.has_value());
    CHECK(!gone->has_value());
}

namespace {

/// One writer process, which closes its handle before it ends.
void write_in_child(const dstd::String& dir, const ScopeKey& scope,
                    const dstd::String& key, const dstd::String& value) {
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::alarm(CHILD_LIMIT_SECONDS);
        auto opened = Store::open(dstd::StringView(dir.data(), dir.size()));
        if (!opened) ::_exit(60);
        auto ns = opened->ns(shared_spec());
        if (!ns) ::_exit(61);
        if (!put(*opened, *ns, scope, key, value)) ::_exit(62);
        ::_exit(run(opened->close()) ? 0 : 63);
    }
    REQUIRE(wait_for(pid) == 0);
}

bool holds(const Snapshot& view, const Namespace& ns, const ScopeKey& scope,
           const dstd::String& key, const char* value) {
    auto found = view.get(ns, scope, view_of(key));
    if (!found || !found->has_value()) return false;
    const dstd::ByteView bytes = (*found)->bytes();
    return dstd::String(reinterpret_cast<const char*>(bytes.data()),
                        bytes.size()) == value;
}

dstd::String rows_of(Cursor cursor) {
    dstd::String rows;
    for (; !cursor.at_end(); cursor.next()) {
        const dstd::ByteView key = cursor.key();
        auto value = cursor.value();
        if (!value) return "error";
        rows.append(reinterpret_cast<const char*>(key.data()), key.size());
        rows.push_back('=');
        rows.append(reinterpret_cast<const char*>(value->data()),
                    value->size());
        rows.push_back(';');
    }
    return rows;
}

/// The newest of three writers wins for `k` in S1, S2 keeps its own `k`, and
/// `x` of the oldest writer stays. Exit code 0 when all that holds.
int check_newest_wins(const dstd::String& dir) {
    auto opened = Store::open(dstd::StringView(dir.data(), dir.size()));
    if (!opened) return 70;
    auto ns = opened->ns(shared_spec());
    if (!ns) return 71;
    auto view = opened->snapshot();
    if (!view) return 72;
    const ScopeKey s1 = scope_of("/s1");
    const ScopeKey s2 = scope_of("/s2");
    if (!holds(*view, *ns, s1, "k", "new")) return 73;
    if (!holds(*view, *ns, s1, "x", "kept")) return 74;
    if (!holds(*view, *ns, s2, "k", "other")) return 75;
    if (rows_of(view->scan(*ns, s1, dstd::ByteView{}, dstd::ByteView{})) !=
        "k=new;x=kept;") {
        return 76;
    }
    const ScopeKey both[] = {s1, s2};
    if (rows_of(view->scan(*ns, dstd::Span<const ScopeKey>(both, 2),
                           dstd::ByteView{}, dstd::ByteView{})) !=
        "k=new;x=kept;k=other;") {
        return 77;
    }
    return 0;
}

}  // namespace

TEST_CASE("refresh shows what another process just wrote") {
    if (TSAN_BUILD) return;
    dftu_utils_test::ScopedTestDir dir("store_fork_refresh");
    auto opened =
        Store::open(dstd::StringView(dir.str().data(), dir.str().size()));
    REQUIRE(opened.has_value());
    auto ns = opened->ns(shared_spec());
    REQUIRE(ns.has_value());
    const ScopeKey scope = scope_of("/s");
    const dstd::String key = "k";
    auto before = opened->snapshot();
    REQUIRE(before.has_value());
    auto missing = before->get(*ns, scope, view_of(key));
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->has_value());

    write_in_child(dir.str(), scope, "k", "from the child");

    REQUIRE(opened->refresh().has_value());
    auto after = opened->snapshot();
    REQUIRE(after.has_value());
    auto found = after->get(*ns, scope, view_of(key));
    REQUIRE(found.has_value());
    CHECK(found->has_value());
}

TEST_CASE("the newest copy wins and pack and a drop keep that order") {
    if (TSAN_BUILD) return;
    dftu_utils_test::ScopedTestDir dir("store_fork_shared_order");
    const ScopeKey s1 = scope_of("/s1");
    write_in_child(dir.str(), s1, "k", "old");
    write_in_child(dir.str(), s1, "x", "kept");
    write_in_child(dir.str(), s1, "k", "new");
    write_in_child(dir.str(), scope_of("/s2"), "k", "other");

    const auto in_child = [&](auto&& body) {
        const pid_t pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            ::alarm(CHILD_LIMIT_SECONDS);
            ::_exit(body());
        }
        return wait_for(pid);
    };
    CHECK(in_child([&] { return check_newest_wins(dir.str()); }) == 0);

    CHECK(in_child([&] {
              auto opened = Store::open(
                  dstd::StringView(dir.str().data(), dir.str().size()));
              if (!opened || !opened->ns(shared_spec())) return 80;
              PackOptions options;
              options.keep_seconds = 0;
              auto report =
                  default_runtime().submit(opened->pack(options)).get();
              if (!report || report->files_packed == 0) return 81;
              return run(opened->close()) ? 0 : 82;
          }) == 0);
    CHECK(in_child([&] { return check_newest_wins(dir.str()); }) == 0);

    CHECK(in_child([&] {
              auto opened = Store::open(
                  dstd::StringView(dir.str().data(), dir.str().size()));
              if (!opened) return 90;
              auto ns = opened->ns(shared_spec());
              if (!ns || !opened->drop(*ns, s1)) return 91;
              return run(opened->close()) ? 0 : 92;
          }) == 0);
    write_in_child(dir.str(), s1, "y", "after");
    CHECK(in_child([&] {
              auto opened = Store::open(
                  dstd::StringView(dir.str().data(), dir.str().size()));
              if (!opened) return 100;
              auto ns = opened->ns(shared_spec());
              if (!ns) return 101;
              auto view = opened->snapshot();
              if (!view) return 102;
              if (rows_of(view->scan(*ns, s1, dstd::ByteView{},
                                     dstd::ByteView{})) != "y=after;") {
                  return 103;
              }
              return 0;
          }) == 0);
}
