// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/algorithm.h>
#include <dftracer/utils/dstd/concurrent_queue.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>
#include <pthread.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <random>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_set>

using namespace dftracer::utils;

namespace {

struct Keyed {
    int key;
    int order;
};

struct Base {
    virtual ~Base() = default;
    std::int64_t base_value = 1;
};

struct Other {
    virtual ~Other() = default;
    std::int64_t other_value = 2;
};

// Other is a second base, so an Other* points past the start of the object.
struct Derived : Base, Other {
    explicit Derived(std::atomic<int>* d) : destroyed(d) {}
    ~Derived() override { destroyed->fetch_add(1); }
    std::atomic<int>* destroyed;
    char payload[200] = {};
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
        std::stable_sort(   // keep std: compares dstd with its std twin
            expect.begin(), expect.end(),
            [](const Keyed& a, const Keyed& b) { return a.key < b.key; });
        dstd::stable_sort(  // keep std: compares dstd with its std twin
            v.begin(), v.end(),
            [](const Keyed& a, const Keyed& b) { return a.key < b.key; });
        bool same = true;
        for (std::size_t i = 0; i < v.size(); ++i)
            same = same && v[i].key == expect[i].key &&
                   v[i].order == expect[i].order;
        CHECK(same);
    }
}

// keep std: compares dstd with its std twin
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

TEST_CASE("UniquePtr of a derived type converts to its virtual bases") {
    std::atomic<int> destroyed{0};
    const std::size_t before = memory::Manager::get().general().blocks();
    {
        dstd::UniquePtr<Base> base = dstd::make_unique<Derived>(&destroyed);
        CHECK(base->base_value == 1);
        dstd::UniquePtr<Other> other = dstd::make_unique<Derived>(&destroyed);
        CHECK(other->other_value == 2);
        CHECK(static_cast<void*>(other.get()) !=
              dynamic_cast<void*>(other.get()));
    }
    CHECK(destroyed.load() == 2);
    CHECK(memory::Manager::get().general().blocks() == before);
}

TEST_CASE("Thread::try_start runs the body or reports an error") {
    std::atomic<int> ran{0};
    auto t = dstd::Thread::try_start([&] { ran.fetch_add(1); }, "dftu-try");
    REQUIRE(t);
    t->join();
    CHECK(ran.load() == 1);
}

TEST_CASE("Thread takes a stack size at start") {
    constexpr std::size_t WANT = 4 * 1024 * 1024;
    std::size_t got = 0;
    dstd::Thread t(
        [&] {
#if defined(__APPLE__)
            got = pthread_get_stacksize_np(pthread_self());
#else
            pthread_attr_t attr;
            if (pthread_getattr_np(pthread_self(), &attr) == 0) {
                (void)pthread_attr_getstacksize(&attr, &got);
                (void)pthread_attr_destroy(&attr);
            }
#endif
        },
        "dftu-stack", WANT);
    t.join();
    CHECK(got >= WANT);
    CHECK(got < 2 * WANT);
}

TEST_CASE("to_chars writes integers and the shortest float that reads back") {
    char buf[32];
    auto r = dstd::to_chars(buf, buf + sizeof(buf), -1234);
    REQUIRE(r.ec == std::errc{});
    CHECK(dstd::StringView(buf, static_cast<std::size_t>(r.ptr - buf)) ==
          "-1234");
    r = dstd::to_chars(buf, buf + sizeof(buf), 2.5);
    REQUIRE(r.ec == std::errc{});
    CHECK(dstd::StringView(buf, static_cast<std::size_t>(r.ptr - buf)) ==
          "2.5");
    r = dstd::to_chars(buf, buf + sizeof(buf), 0.1f);
    REQUIRE(r.ec == std::errc{});
    CHECK(dstd::StringView(buf, static_cast<std::size_t>(r.ptr - buf)) ==
          "0.1");
    char tiny[2];
    CHECK(dstd::to_chars(tiny, tiny + sizeof(tiny), 0.125).ec ==
          std::errc::value_too_large);
}

TEST_CASE("from_chars stops at the first byte that is not part of a number") {
    const dstd::StringView text = "42.5xyz";
    double d = 0;
    auto r = dstd::from_chars(text.data(), text.data() + text.size(), d);
    REQUIRE(r.ec == std::errc{});
    CHECK(d == 42.5);
    CHECK(r.ptr == text.data() + 4);
    int i = 0;
    r = dstd::from_chars(text.data(), text.data() + text.size(), i);
    REQUIRE(r.ec == std::errc{});
    CHECK(i == 42);
    float f = 0;
    const dstd::StringView huge = "1e300";
    CHECK(dstd::from_chars(huge.data(), huge.data() + huge.size(), f).ec ==
          std::errc::result_out_of_range);
    CHECK(dstd::from_chars<float>("0.25") == 0.25f);
    CHECK_FALSE(dstd::from_chars<float>("1e300").has_value());
}

TEST_CASE("ConcurrentQueue takes its blocks from the memory library") {
    const std::size_t before = memory::Manager::get().root().blocks();
    const std::size_t general_before =
        memory::Manager::get().general().blocks();
    {
        dstd::ConcurrentQueue<std::int64_t> queue;
        CHECK(memory::Manager::get().root().blocks() > before);
        CHECK(memory::Manager::get().general().blocks() == general_before);
        dstd::Thread producer([&] {
            for (std::int64_t i = 0; i < 10000; ++i) REQUIRE(queue.enqueue(i));
        });
        producer.join();
        std::int64_t sum = 0;
        std::int64_t item = 0;
        while (queue.try_dequeue(item)) sum += item;
        CHECK(sum == 10000LL * 9999 / 2);
    }
    CHECK(memory::Manager::get().root().blocks() == before);
}

TEST_CASE("ConcurrentQueue takes its blocks from the memory library") {
    const std::size_t before = memory::Manager::get().root().blocks();
    const std::size_t general_before =
        memory::Manager::get().general().blocks();
    {
        dstd::ConcurrentQueue<std::int64_t> queue;
        CHECK(memory::Manager::get().root().blocks() > before);
        CHECK(memory::Manager::get().general().blocks() == general_before);
        dstd::Thread producer([&] {
            for (std::int64_t i = 0; i < 10000; ++i) (void)queue.enqueue(i);
        });
        producer.join();
        std::int64_t sum = 0;
        std::int64_t count = 0;
        std::int64_t item = 0;
        while (queue.try_dequeue(item)) {
            sum += item;
            ++count;
        }
        CHECK(count == 10000);
        CHECK(sum == 10000LL * 9999 / 2);
    }
    CHECK(memory::Manager::get().root().blocks() == before);
}

TEST_CASE("UniquePtr array form zeroes on make_unique and frees its block") {
    const std::size_t before = memory::Manager::get().general().blocks();
    {
        auto zeroed = dstd::make_unique<std::uint64_t[]>(4096);
        bool all_zero = true;
        for (std::size_t i = 0; i < 4096; ++i)
            all_zero = all_zero && zeroed[i] == 0;
        CHECK(all_zero);
        auto raw = dstd::make_unique_for_overwrite<char[]>(100);
        raw[99] = 'x';
        CHECK(raw[99] == 'x');
        dstd::UniquePtr<std::uint64_t[]> moved = std::move(zeroed);
        CHECK(zeroed == nullptr);
        CHECK(moved[4095] == 0);
        auto empty = dstd::make_unique<int[]>(0);
        CHECK(empty != nullptr);
    }
    CHECK(memory::Manager::get().general().blocks() == before);
}

TEST_CASE("share takes its count block from the memory library") {
    std::atomic<int> destroyed{0};
    const std::size_t before = memory::Manager::get().general().blocks();
    {
        dstd::SharedPtr<Base> base =
            dstd::to_shared_ptr(dstd::make_unique<Derived>(&destroyed));
        CHECK(base->base_value == 1);
        CHECK(memory::Manager::get().general().blocks() == before + 2);
        dstd::SharedPtr<Base> again = base;
        CHECK(base.use_count() == 2);
    }
    CHECK(destroyed.load() == 1);
    CHECK(memory::Manager::get().general().blocks() == before);
}

TEST_CASE("share with a deleter calls it once") {
    int calls = 0;
    int value = 7;
    {
        dstd::SharedPtr<int> p =
            dstd::adopt_shared_ptr(&value, [&calls](int*) { ++calls; });
        dstd::SharedPtr<int> q = p;
        CHECK(*q == 7);
        p.reset();
        CHECK(calls == 0);
    }
    CHECK(calls == 1);
}

static_assert(!std::is_constructible_v<dstd::SharedPtr<int>, int*>);
static_assert(
    !std::is_constructible_v<dstd::SharedPtr<int>, int*, void (*)(int*)>);
static_assert(
    // keep std: compares dstd with its std twin
    !std::is_constructible_v<dstd::SharedPtr<int>, std::unique_ptr<int>&&>);
static_assert(
    // keep std: compares dstd with its std twin
    std::is_constructible_v<dstd::SharedPtr<int>, std::shared_ptr<int>>);

TEST_CASE("SharedPtr array forms come from our heap and free everything") {
    const std::size_t before = memory::Manager::get().general().blocks();
    {
        dstd::SharedPtr<std::uint64_t[]> zeroed =
            dstd::make_shared<std::uint64_t[]>(512);
        bool all_zero = true;
        for (std::size_t i = 0; i < 512; ++i)
            all_zero = all_zero && zeroed[i] == 0;
        CHECK(all_zero);
        CHECK(memory::Manager::get().general().blocks() == before + 1);
        dstd::SharedPtr<char[]> raw =
            dstd::make_shared_for_overwrite<char[]>(64);
        raw[63] = 'x';
        CHECK(raw[63] == 'x');
        dstd::SharedPtr<int[]> moved =
            dstd::to_shared_ptr(dstd::make_unique<int[]>(8));
        dstd::SharedPtr<int[]> copy = moved;
        CHECK(copy.use_count() == 2);
        CHECK(copy[7] == 0);
    }
    CHECK(memory::Manager::get().general().blocks() == before);
}

// keep std: compares dstd with its std twin
TEST_CASE("std::hash of a dstd::String matches std::string") {
    const dstd::String text = "trace-000.pfw.gz";
    CHECK(std::hash<dstd::String>{}(text) ==
          // keep std: compares dstd with its std twin
          std::hash<std::string>{}(std::string("trace-000.pfw.gz")));
    std::unordered_set<dstd::String> seen;  // keep std: tests std::hash use
    seen.insert(text);
    CHECK(seen.count(dstd::String("trace-000.pfw.gz")) == 1);
}
