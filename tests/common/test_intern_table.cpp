#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/intern_table.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/coro/yield.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::coro::CoroTask;
namespace memory = dftracer::utils::memory;

namespace {

constexpr int HOPS = 8;
constexpr int TASKS = 64;
constexpr int STRINGS = 300;

std::string name_of(int i) { return "string_" + std::to_string(i); }

CoroTask<void> hop_and_check(InternTable* expected, std::atomic<int>* bad) {
    if (current_intern() != expected) ++*bad;
    const InternId id = expected->intern("seen from a task");
    for (int i = 0; i < HOPS; ++i) {
        co_await coro::yield();
        if (current_intern() != expected) ++*bad;
        if (expected->get(id) != "seen from a task") ++*bad;
    }
}

CoroTask<void> intern_and_read(InternTable* table, int seed,
                               std::atomic<int>* bad) {
    for (int round = 0; round < STRINGS; ++round) {
        const int k = (round * 7 + seed) % STRINGS;
        const std::string text = name_of(k);
        const InternId id = table->intern(text);
        if (id == InternId::NONE) ++*bad;
        if (round % 16 == 0) co_await coro::yield();
        if (table->get(id) != text) ++*bad;
        if (table->intern(text) != id) ++*bad;
        if (current_intern() != table) ++*bad;
    }
}

CoroTask<void> sees_no_table(std::atomic<int>* bad) {
    if (current_intern() != nullptr) ++*bad;
    co_await coro::yield();
    if (current_intern() != nullptr) ++*bad;
}

}  // namespace

TEST_CASE("the last holder frees the table") {
    InternRef first = InternTable::create();
    memory::Pool& pool = first->pool();
    const InternId alpha = first->intern("alpha");
    const InternId beta = first->intern("beta");
    CHECK(pool.live() > 0);

    InternRef second = first;
    CHECK(first->use_count() == 2);
    first = InternRef{};
    CHECK(second->use_count() == 1);
    CHECK(second->get(alpha) == "alpha");
    CHECK(second->get(beta) == "beta");
    CHECK(pool.live() > 0);

    second = InternRef{};
    CHECK(pool.live() == 0);
    CHECK(pool.blocks() == 0);
}

TEST_CASE("a big string and many strings are freed with the table") {
    InternRef table = InternTable::create();
    memory::Pool& pool = table->pool();
    for (int i = 0; i < 20000; ++i) table->intern(name_of(i));
    const std::string big(dstd::StringIntern::CHUNK_BYTES + 1000, 'x');
    const InternId id = table->intern(big);
    CHECK(table->get(id) == big);
    CHECK(pool.live() > dstd::StringIntern::CHUNK_BYTES);
    table = InternRef{};
    CHECK(pool.live() == 0);
    CHECK(pool.blocks() == 0);
}

TEST_CASE("InternId::NONE has no string") {
    InternRef table = InternTable::create();
    CHECK(InternId{} == InternId::NONE);
    CHECK(table->get(InternId::NONE).empty());
    const InternId id = table->intern("first");
    CHECK(id != InternId::NONE);
    CHECK(table->intern("") != InternId::NONE);
    CHECK(table->get(id) == "first");
}

TEST_CASE("InternScope nests and restores the current table") {
    CHECK(current_intern() == nullptr);
    InternRef outer = InternTable::create();
    InternRef inner = InternTable::create();
    {
        InternScope a{outer};
        CHECK(current_intern() == outer.get());
        {
            InternScope b{inner};
            CHECK(current_intern() == inner.get());
        }
        CHECK(current_intern() == outer.get());
    }
    CHECK(current_intern() == nullptr);
}

TEST_CASE(
    "a memory Scope keeps the current table, and one with a pointer sets it") {
    InternRef table = InternTable::create();
    InternRef other = InternTable::create();
    InternScope scope{table};
    memory::Pool& general = memory::Manager::get().general();
    {
        memory::Scope plain{general};
        CHECK(current_intern() == table.get());
    }
    CHECK(current_intern() == table.get());
    {
        memory::Scope cleared{general, nullptr};
        CHECK(current_intern() == nullptr);
        {
            memory::Scope set{general, other.get()};
            CHECK(current_intern() == other.get());
        }
        CHECK(current_intern() == nullptr);
    }
    CHECK(current_intern() == table.get());
}

TEST_CASE("ids from the scope survive a co_await on another thread") {
    InternRef table = InternTable::create();
    memory::Pool& pool = table->pool();
    std::atomic<int> bad{0};
    {
        Runtime rt(4);
        {
            InternScope scope{table};
            rt.submit(hop_and_check(table.get(), &bad)).get();
        }
        std::atomic<int> stale{0};
        rt.submit(sees_no_table(&stale)).get();
        CHECK(stale.load() == 0);
    }
    CHECK(bad.load() == 0);
    CHECK(table->get(table->intern("seen from a task")) == "seen from a task");
    table = InternRef{};
    CHECK(pool.live() == 0);
}

TEST_CASE("tasks on many threads intern and read one table") {
    InternRef table = InternTable::create();
    memory::Pool& pool = table->pool();
    std::atomic<int> bad{0};
    {
        Runtime rt(8);
        std::vector<TaskHandle> handles;
        {
            InternScope scope{table};
            for (int i = 0; i < TASKS; ++i) {
                handles.push_back(
                    rt.submit(intern_and_read(table.get(), i, &bad)));
            }
        }
        for (auto& h : handles) h.get();
    }
    CHECK(bad.load() == 0);
    for (int k = 0; k < STRINGS; ++k) {
        const InternId id = table->intern(name_of(k));
        CHECK(table->get(id) == name_of(k));
    }
    CHECK(table->strings().size() == std::size_t{STRINGS});
    table = InternRef{};
    CHECK(pool.live() == 0);
}

TEST_CASE("threads share and drop references to one table") {
    InternRef table = InternTable::create();
    memory::Pool& pool = table->pool();
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, copy = table, t]() mutable {
            for (int i = 0; i < 200; ++i) {
                InternRef extra = copy;
                const InternId id = extra->intern(name_of((i + t) % 50));
                if (extra->get(id) != name_of((i + t) % 50)) ++bad;
            }
        });
    }
    table = InternRef{};
    for (auto& th : threads) th.join();
    CHECK(bad.load() == 0);
    CHECK(pool.live() == 0);
}
