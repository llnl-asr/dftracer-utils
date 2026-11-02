// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/algorithm.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <pthread.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <random>

using namespace dftracer::utils;

namespace {

struct Keyed {
    int key;
    int order;
};

}  // namespace

TEST_CASE("Thread runs its body, joins in its destructor and sets its name") {
    std::atomic<int> ran{0};
    char seen[16] = {};
    {
        dstd::Thread t(
            [&] {
                ran.fetch_add(1);
                pthread_getname_np(pthread_self(), seen, sizeof seen);
            },
            "dftu-test-thread-long-name");
        CHECK(t.joinable());
    }
    CHECK(ran.load() == 1);
    CHECK(dstd::StringView(seen) == "dftu-test-threa");
}

TEST_CASE("Thread move assignment joins the thread it held") {
    std::atomic<int> ran{0};
    dstd::Thread a([&] { ran.fetch_add(1); });
    a = dstd::Thread([&] { ran.fetch_add(10); });
    CHECK(ran.load() >= 1);
    a.join();
    CHECK(ran.load() == 11);
    CHECK_FALSE(a.joinable());
}

TEST_CASE("Thread ids tell threads apart") {
    dstd::Thread::Id inner;
    dstd::Thread t([&] { inner = dstd::Thread::current_id(); });
    const dstd::Thread::Id outer = t.id();
    t.join();
    CHECK(inner == outer);
    CHECK(inner != dstd::Thread::current_id());
    CHECK(t.id() == dstd::Thread::Id());
}

TEST_CASE("Promise hands its value to the future on another thread") {
    dstd::Promise<int> promise;
    dstd::Future<int> future = promise.get_future();
    dstd::Thread t([p = std::move(promise)]() mutable { p.set_value(42); });
    CHECK(future.get() == 42);
}

TEST_CASE("stable_sort sorts and keeps equal elements in order") {
    std::mt19937 rng(7);
    for (int n : {0, 1, 2, 31, 32, 33, 100, 1000, 4097}) {
        dstd::Vector<Keyed> v;
        for (int i = 0; i < n; ++i)
            v.push_back({static_cast<int>(rng() % 17), i});
        dstd::Vector<Keyed> expect = v;
        std::stable_sort(
            expect.begin(), expect.end(),
            [](const Keyed& a, const Keyed& b) { return a.key < b.key; });
        dstd::stable_sort(
            v.begin(), v.end(),
            [](const Keyed& a, const Keyed& b) { return a.key < b.key; });
        bool same = true;
        for (std::size_t i = 0; i < v.size(); ++i)
            same = same && v[i].key == expect[i].key &&
                   v[i].order == expect[i].order;
        CHECK(same);
    }
}

TEST_CASE("to_string matches std::to_string") {
    CHECK(dstd::to_string(0) == "0");
    CHECK(dstd::to_string(-42) == "-42");
    CHECK(dstd::to_string(std::uint64_t{18446744073709551615ULL}) ==
          "18446744073709551615");
    CHECK(dstd::to_string(1.5) == "1.500000");
}

TEST_CASE("from_chars takes only a whole number") {
    CHECK(dstd::from_chars<int>("123") == 123);
    CHECK(dstd::from_chars<int>("-7") == -7);
    CHECK(dstd::from_chars<double>("2.5") == 2.5);
    CHECK_FALSE(dstd::from_chars<int>("").has_value());
    CHECK_FALSE(dstd::from_chars<int>("12x").has_value());
    CHECK_FALSE(dstd::from_chars<int>(" 12").has_value());
    CHECK_FALSE(dstd::from_chars<std::uint8_t>("300").has_value());
}

TEST_CASE("write_file replaces and append_file extends") {
    dftu_utils_test::ScopedTestDir dir("std_extras");
    const dstd::Path file = dir.path() / dstd::StringView("f.txt");
    REQUIRE(dstd::fs::write_file(file.view(), "one\n"));
    REQUIRE(dstd::fs::append_file(file.view(), "two\n"));
    auto text = dstd::fs::read_file(file.view());
    REQUIRE(text);
    CHECK(*text == "one\ntwo\n");
    REQUIRE(dstd::fs::write_file(file.view(), "x"));
    CHECK(*dstd::fs::read_file(file.view()) == "x");
    CHECK_FALSE(dstd::fs::write_file(
        (dir.path() / dstd::StringView("no/such/f")).view(), "x"));
}
