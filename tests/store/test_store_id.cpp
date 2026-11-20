// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/store_id.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>

using namespace dftracer::utils;
using namespace dftracer::utils::store::detail;

namespace {

dstd::Vector<dstd::String> names_in(const dstd::Path& dir) {
    dstd::Vector<dstd::String> names;
    auto opened = dstd::fs::Dir::open(dir.view());
    REQUIRE(opened.has_value());
    auto listing = opened->entries();
    REQUIRE(listing.has_value());
    for (const dstd::fs::DirEntry& entry : *listing)
        names.emplace_back(entry.name);
    return names;
}

/// One id, one marker for it, and nothing left over from a repair.
void check_one_store(const dstd::Path& dir, std::uint64_t id) {
    std::size_t marks = 0;
    for (const dstd::String& name : names_in(dir)) {
        if (dstd::PathView(name).extension() == ".mark") {
            ++marks;
            CHECK(name == marker_file_name(id));
        } else {
            CHECK(name == "store.id");
        }
    }
    CHECK(marks == 1);
    const StoreIdRead read = read_store_id(dir);
    CHECK(read.state == IdFile::Found);
    CHECK(read.id == id);
}

constexpr int THREADS = 8;

dstd::Vector<std::uint64_t> race_in_threads(const dstd::Path& dir) {
    dstd::Vector<std::uint64_t> ids(THREADS, 0);
    std::atomic<bool> go{false};
    dstd::Vector<dstd::Thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) {
            }
            ids[static_cast<std::size_t>(t)] = ensure_store_id(dir);
        });
    }
    go.store(true, std::memory_order_release);
    for (dstd::Thread& thread : threads) thread.join();
    return ids;
}

}  // namespace

TEST_CASE("threads that make one store at once agree on one id") {
    for (int round = 0; round < 20; ++round) {
        dftu_utils_test::ScopedTestDir dir("store_id_threads");
        const auto ids = race_in_threads(dir.path());
        REQUIRE(ids[0] != 0);
        for (const std::uint64_t id : ids) CHECK(id == ids[0]);
        check_one_store(dir.path(), ids[0]);
    }
}

TEST_CASE("processes and threads that make one store at once agree") {
    for (int round = 0; round < 5; ++round) {
        dftu_utils_test::ScopedTestDir dir("store_id_processes");
        constexpr int CHILDREN = 2;
        pid_t children[CHILDREN];
        for (int c = 0; c < CHILDREN; ++c) {
            children[c] = ::fork();
            REQUIRE(children[c] >= 0);
            if (children[c] == 0) {
                ::alarm(60);
                const std::uint64_t id = ensure_store_id(dir.path());
                const dstd::String out =
                    dir.file(dstd::String("child") + dstd::to_string(c));
                if (id == 0 || !dstd::fs::write_file(dstd::PathView(out),
                                                     marker_file_name(id))
                                    .has_value()) {
                    ::_exit(1);
                }
                ::_exit(0);
            }
        }
        const auto ids = race_in_threads(dir.path());
        for (const pid_t child : children) {
            int status = 0;
            REQUIRE(::waitpid(child, &status, 0) == child);
            REQUIRE(WIFEXITED(status));
            CHECK(WEXITSTATUS(status) == 0);
        }
        REQUIRE(ids[0] != 0);
        for (const std::uint64_t id : ids) CHECK(id == ids[0]);
        for (int c = 0; c < CHILDREN; ++c) {
            auto said = dstd::fs::read_file(dstd::PathView(
                dir.file(dstd::String("child") + dstd::to_string(c))));
            REQUIRE(said.has_value());
            CHECK(*said == marker_file_name(ids[0]));
        }
        // The children's note files are the only extras.
        std::size_t marks = 0;
        for (const dstd::String& name : names_in(dir.path())) {
            marks += dstd::PathView(name).extension() == ".mark";
        }
        CHECK(marks == 1);
    }
}

TEST_CASE("a store.id that is not an id is replaced by a whole file") {
    dftu_utils_test::ScopedTestDir dir("store_id_invalid");
    REQUIRE(dstd::fs::write_file(dstd::PathView(dir.file("store.id")),
                                 "not an id at all")
                .has_value());
    const std::uint64_t id = ensure_store_id(dir.path());
    REQUIRE(id != 0);
    check_one_store(dir.path(), id);
    CHECK(ensure_store_id(dir.path()) == id);
}

TEST_CASE("a short store.id of a dead creator is waited for, then replaced") {
    dftu_utils_test::ScopedTestDir dir("store_id_short");
    REQUIRE(dstd::fs::write_file(dstd::PathView(dir.file("store.id")), "ab")
                .has_value());
    // The file is young, so the first call waits for its creator and gives up
    // when no creator finishes it.
    const std::uint64_t id = ensure_store_id(dir.path());
    REQUIRE(id != 0);
    check_one_store(dir.path(), id);
}

TEST_CASE("a store.id that is not the start of an id is invalid at once") {
    for (const char* junk : {"zz", "not an id", "0123456789abcdef0", "ab cd"}) {
        dftu_utils_test::ScopedTestDir dir("store_id_junk");
        REQUIRE(dstd::fs::write_file(dstd::PathView(dir.file("store.id")), junk)
                    .has_value());
        const std::uint64_t started = dstd::steady_now_ns();
        const StoreIdRead read = read_store_id(dir.path());
        const std::uint64_t waited = dstd::steady_now_ns() - started;
        CHECK(read.state == IdFile::Invalid);
        CHECK(waited < 500'000'000ULL);
    }
}

TEST_CASE("a reader waits for a creator that is writing store.id") {
    dftu_utils_test::ScopedTestDir dir("store_id_wait");
    REQUIRE(dstd::fs::write_file(dstd::PathView(dir.file("store.id")), "")
                .has_value());
    dstd::Thread creator([&] {
        ::usleep(30'000);
        dstd::fs::write_file(dstd::PathView(dir.file("store.id")),
                             "00000000000000ab")
            .discard();
    });
    const StoreIdRead read = read_store_id(dir.path());
    creator.join();
    CHECK(read.state == IdFile::Found);
    CHECK(read.id == 0xab);
}

TEST_CASE("a missing directory and a reader of an older store make no id") {
    dftu_utils_test::ScopedTestDir dir("store_id_none");
    const dstd::Path nowhere = dir.path() / "absent";
    CHECK(ensure_store_id(nowhere) == 0);
    CHECK(existing_store_id(dir.path()) == 0);
    CHECK(names_in(dir.path()).empty());
    const std::uint64_t made = ensure_store_id(dir.path());
    CHECK(made != 0);
    CHECK(existing_store_id(dir.path()) == made);
}
