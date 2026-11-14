// SPDX-License-Identifier: MIT
// dftu.svc.memory: a plugin's blocks and reservations live in a pool named
// after the plugin, a grow past the budget is refused cleanly, a reclaimer
// makes room, and a reservation the plugin leaves is released by the host.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/memory/detail/pool_access.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>
#include <dftracer/utils/plugins/plugin.h>
#include <doctest/doctest.h>
#include <plugins/test_abi_ctx.h>

#include <atomic>
#include <cstdint>

using namespace dftracer::utils;

namespace {

struct TrivialSlice {
    explicit TrivialSlice(const plugins::Config&) {}
    void step(const dftu_dataframe*, plugins::Host) {}
    void merge(TrivialSlice&) {}
    void finalize(plugins::Host) {}
};

// One loaded plugin as the host sees it: a fold with a name, a memory budget
// and the pool of that plugin.
struct PluginFixture {
    dstd::StringIntern intern;
    dstd::SharedPtr<plugins::host::PluginMemory> memory;
    dftu_plugin* plugin;
    dstd::UniquePtr<plugins::host::PluginFold> fold;

    PluginFixture(dstd::StringView name, std::uint64_t budget)
        : memory(dstd::make_shared<plugins::host::PluginMemory>(name)),
          plugin(plugins::make_plugin<TrivialSlice>(nullptr)),
          fold(dstd::make_unique<plugins::host::PluginFold>(
              plugin, intern, nullptr, nullptr, dstd::String(name), budget,
              nullptr, memory)) {}
    ~PluginFixture() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }

    dftu_plugin_host& host() { return fold->host(); }
    const dftu_svc_memory& svc() {
        return *static_cast<const dftu_svc_memory*>(
            host().get_service(host().h, DFTU_SVC_MEMORY));
    }
    memory::Pool& pool() { return memory->pool(); }
};

// The pool gauges as the manager reports them for the pool named `name`.
bool pool_gauge(dstd::StringView name, std::size_t& live) {
    const dstd::String suffix = dstd::fmt<"/{}">(name);
    for (const auto& s : memory::detail::pool_stats()) {
        if (dstd::StringView(s.path).ends_with(dstd::StringView(suffix))) {
            live = s.live;
            return true;
        }
    }
    return false;
}

constexpr std::uint64_t BUDGET = 4096;

}  // namespace

TEST_CASE("a plugin's alloc shows in its pool gauge and returns to zero") {
    PluginFixture fx("mem_gauge_plugin", 0);
    const dftu_svc_memory& m = fx.svc();
    CHECK(fx.pool().live() == 0);

    void* p = m.alloc(fx.host().h, 1 << 20, 64);
    REQUIRE(p != nullptr);
    CHECK(fx.pool().live() >= (1u << 20));
    std::size_t gauge = 0;
    REQUIRE(pool_gauge("mem_gauge_plugin", gauge));
    CHECK(gauge >= (1u << 20));

    void* q = m.realloc(fx.host().h, p, 1 << 20, 1 << 10, 64);
    REQUIRE(q != nullptr);
    CHECK(fx.pool().live() < (1u << 20));
    m.free(fx.host().h, q, 1 << 10, 64);
    CHECK(fx.pool().live() == 0);
    REQUIRE(pool_gauge("mem_gauge_plugin", gauge));
    CHECK(gauge == 0);
}

TEST_CASE("try_grow over the budget is refused and leaves the reservation") {
    PluginFixture fx("mem_budget_plugin", BUDGET);
    const dftu_svc_memory& m = fx.svc();
    test_plugin_common::Ctx c;
    dftu_reservation* r = nullptr;
    REQUIRE(m.reserve(c.get(), fx.host().h, &r, "budget") == DFTU_OK);
    REQUIRE(m.try_grow(c.get(), r, 3000) == DFTU_OK);
    const std::size_t held = fx.pool().reserved();
    CHECK(held >= 3000);

    CHECK(m.try_grow(c.get(), r, 2000) == DFTU_E_NO_MEMORY);
    CHECK(c.message().find("memory budget") != dstd::String::npos);
    CHECK(c.message().find("budget") != dstd::String::npos);
    CHECK(fx.pool().reserved() == held);

    // The reservation still grows by what fits.
    CHECK(m.try_grow(c.get(), r, 1000) == DFTU_OK);
    m.release(r);
    CHECK(fx.pool().reserved() == 0);
}

TEST_CASE("a reclaimer runs when a grow needs room and the grow then passes") {
    struct Holder {
        std::uint64_t held = 0;
        std::uint64_t wanted = 0;
        int calls = 0;
        std::uint64_t reclaim(std::uint64_t want) {
            ++calls;
            wanted = want;
            const std::uint64_t freed = want < held ? want : held;
            held -= freed;
            return freed;
        }
    };
    PluginFixture fx("mem_reclaim_plugin", BUDGET);
    test_plugin_common::Ctx c;
    plugins::Host host(&fx.host(), c.get());
    plugins::Memory mem = host.memory();

    auto res = mem.reserve("reclaim");
    REQUIRE(res.has_value());
    Holder holder;
    res->set_reclaimer(&holder);
    REQUIRE(res->try_grow(host, 3000).has_value());
    holder.held = 3000;

    REQUIRE(res->try_grow(host, 2000).has_value());
    holder.held += 2000;
    CHECK(holder.calls == 1);
    // 3000 held and 2000 asked on a 4096 budget: 904 bytes were missing.
    CHECK(holder.wanted == 3000 + 2000 - BUDGET);
    CHECK(holder.held == 5000 - holder.wanted);
    CHECK(fx.pool().reserved() >= BUDGET);

    // A reclaimer that frees nothing leaves the refusal.
    holder.held = 0;
    auto refused = res->try_grow(host, 9000);
    REQUIRE(!refused.has_value());
    CHECK(plugins::status_of(refused.error()) == DFTU_E_NO_MEMORY);
    CHECK(holder.calls == 2);

    res->release();
    CHECK(fx.pool().reserved() == 0);
}

TEST_CASE("a reservation gives its bytes back on release and on destruction") {
    PluginFixture fx("mem_release_plugin", 0);
    test_plugin_common::Ctx c;
    plugins::Host host(&fx.host(), c.get());
    {
        auto res = host.memory().reserve("raii");
        REQUIRE(res.has_value());
        REQUIRE(res->try_grow(host, 10000).has_value());
        CHECK(fx.pool().reserved() >= 10000);
        res->shrink(4000);
        CHECK(fx.pool().reserved() >= 6000);
        CHECK(fx.pool().reserved() < 10000);
    }
    CHECK(fx.pool().reserved() == 0);
}

TEST_CASE(
    "a reservation the plugin leaves is released and reported at unload") {
    auto memory =
        dstd::make_shared<plugins::host::PluginMemory>("mem_leak_plugin");
    const std::size_t before = memory::root_pool().reserved();
    plugins::host::PluginReservation* leaked = memory->reserve("leaked", 0);
    REQUIRE(leaked != nullptr);
    dstd::String why;
    REQUIRE(memory->try_grow(*leaked, 1 << 20, why));
    CHECK(memory::root_pool().reserved() >= before + (1u << 20));
    plugins::host::PluginReservation* kept = memory->reserve("kept", 0);
    REQUIRE(kept != nullptr);
    memory->release(kept);

    CHECK(memory->close() == 1);
    CHECK(memory::root_pool().reserved() == before);
    CHECK(memory->close() == 0);
    CHECK(memory->reserve("late", 0) == nullptr);
}

TEST_CASE("two plugins have separate pools") {
    PluginFixture a("mem_pool_a", BUDGET);
    PluginFixture b("mem_pool_b", BUDGET);
    CHECK(&a.pool() != &b.pool());

    void* p = a.svc().alloc(a.host().h, 8192, 16);
    REQUIRE(p != nullptr);
    test_plugin_common::Ctx c;
    dftu_reservation* r = nullptr;
    REQUIRE(b.svc().reserve(c.get(), b.host().h, &r, "b") == DFTU_OK);
    REQUIRE(b.svc().try_grow(c.get(), r, 2048) == DFTU_OK);

    std::size_t a_live = 0, b_live = 0;
    REQUIRE(pool_gauge("mem_pool_a", a_live));
    REQUIRE(pool_gauge("mem_pool_b", b_live));
    CHECK(a_live >= 8192);
    CHECK(b_live == 0);
    CHECK(a.pool().reserved() == 0);
    CHECK(b.pool().reserved() >= 2048);

    // The budget of one plugin does not count the other's reservations.
    dftu_reservation* ra = nullptr;
    REQUIRE(a.svc().reserve(c.get(), a.host().h, &ra, "a") == DFTU_OK);
    CHECK(a.svc().try_grow(c.get(), ra, BUDGET) == DFTU_OK);

    a.svc().release(ra);
    b.svc().release(r);
    a.svc().free(a.host().h, p, 8192, 16);
}

TEST_CASE("the budget is shared by every reservation of one plugin") {
    PluginFixture fx("mem_shared_plugin", BUDGET);
    const dftu_svc_memory& m = fx.svc();
    test_plugin_common::Ctx c;
    dftu_reservation *a = nullptr, *b = nullptr;
    REQUIRE(m.reserve(c.get(), fx.host().h, &a, "a") == DFTU_OK);
    REQUIRE(m.reserve(c.get(), fx.host().h, &b, "b") == DFTU_OK);
    REQUIRE(m.try_grow(c.get(), a, 3000) == DFTU_OK);

    CHECK(m.try_grow(c.get(), b, 2000) == DFTU_E_NO_MEMORY);
    CHECK(m.try_grow(c.get(), b, 1000) == DFTU_OK);
    CHECK(m.try_grow(c.get(), a, 200) == DFTU_E_NO_MEMORY);
    m.release(a);
    CHECK(m.try_grow(c.get(), b, 3000) == DFTU_OK);
    m.release(b);
}

TEST_CASE("another holder's request reaches the reclaimer at a boundary") {
    struct Holder {
        std::uint64_t held = 0;
        std::uint64_t wanted = 0;
        std::uint64_t reclaim(std::uint64_t want) {
            wanted = want;
            const std::uint64_t freed = want < held ? want : held;
            held -= freed;
            return freed;
        }
    };
    PluginFixture fx("mem_pressure_plugin", 0);
    test_plugin_common::Ctx c;
    plugins::Host host(&fx.host(), c.get());
    auto res = host.memory().reserve("pressure");
    REQUIRE(res.has_value());
    Holder holder;
    res->set_reclaimer(&holder);
    REQUIRE(res->try_grow(host, 1 << 20).has_value());
    holder.held = 1 << 20;

    // Nothing was asked, so a boundary does nothing.
    fx.fold->poll_reclaim();
    CHECK(holder.wanted == 0);

    auto& manager = memory::detail::Manager::get();
    const std::size_t used = memory::root_pool().reserved();
    manager.set_budget(used + (1 << 20) / 2);
    const std::size_t before = fx.pool().reserved();
    auto refused =
        memory::detail::PoolAccess::reserve(memory::root_pool(), 1 << 20);
    CHECK_FALSE(refused);
    CHECK(holder.wanted == 0);

    fx.fold->poll_reclaim();
    manager.set_budget(0);
    CHECK(holder.wanted > 0);
    CHECK(holder.held < (1u << 20));
    CHECK(fx.pool().reserved() < before);
}

TEST_CASE("parallel grows never pass the shared cap") {
    constexpr std::uint64_t CAP = 4096;
    constexpr int THREADS = 8;
    constexpr int ROUNDS = 300;
    constexpr int TRIES = 64;
    for (int round = 0; round < ROUNDS; ++round) {
        plugins::host::PluginMemory memory("mem_cap_race_plugin");
        dstd::Vector<plugins::host::PluginReservation*> mine;
        for (int t = 0; t < THREADS; ++t)
            mine.push_back(memory.reserve("race", CAP));
        std::atomic<bool> go{false};
        std::atomic<std::uint64_t> granted{0};
        dstd::Vector<dstd::Thread> workers;
        for (int t = 0; t < THREADS; ++t) {
            plugins::host::PluginReservation* r = mine[t];
            workers.emplace_back([&memory, &go, &granted, r] {
                while (!go.load()) {
                }
                dstd::String why;
                for (int i = 0; i < TRIES; ++i)
                    if (memory.try_grow(*r, 64, why)) granted.fetch_add(64);
            });
        }
        go.store(true);
        for (dstd::Thread& w : workers) w.join();
        REQUIRE(granted.load() <= CAP);
        CHECK(granted.load() == CAP);
    }
}

TEST_CASE("a reclaimer that releases a sibling reservation is safe") {
    struct Ctx {
        plugins::host::PluginMemory* memory;
        plugins::host::PluginReservation* sibling;
        int calls = 0;
    };
    plugins::host::PluginMemory memory("mem_sibling_plugin");
    int tag = 0;
    auto* a = memory.reserve("a", 0, &tag);
    auto* b = memory.reserve("b", 0, &tag);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    dstd::String why;
    REQUIRE(memory.try_grow(*a, 1000, why));
    REQUIRE(memory.try_grow(*b, 1000, why));
    Ctx ctx{&memory, b};
    a->reclaimer = [](void* ud, std::uint64_t) -> std::uint64_t {
        auto* c = static_cast<Ctx*>(ud);
        ++c->calls;
        c->memory->release(c->sibling);
        return 0;
    };
    a->reclaimer_ud = &ctx;
    b->reclaimer = [](void* ud, std::uint64_t) -> std::uint64_t {
        ++static_cast<Ctx*>(ud)->calls;
        return 0;
    };
    b->reclaimer_ud = &ctx;

    CHECK(memory.reclaim(&tag, 5000) == 0);
    CHECK(ctx.calls == 1);
    CHECK(memory.held_by(&tag) == 1000);
    memory.release(a);
}

TEST_CASE("only bytes with a reclaimer count as reclaimable") {
    plugins::host::PluginMemory memory("mem_reclaimable_plugin");
    int tag = 0;
    auto* plain = memory.reserve("plain", 0, &tag);
    auto* soft = memory.reserve("soft", 0, &tag);
    dstd::String why;
    REQUIRE(memory.try_grow(*plain, 1 << 20, why));
    REQUIRE(memory.try_grow(*soft, 4096, why));
    CHECK(memory.held_by(&tag) == 0);
    soft->reclaimer = [](void*, std::uint64_t want) -> std::uint64_t {
        return want;
    };
    CHECK(memory.held_by(&tag) == 4096);
    memory.release(plain);
    memory.release(soft);
}

TEST_CASE("a slice's leftover reservation is released when its fold dies") {
    PluginFixture fx("mem_fold_leak_plugin", 0);
    plugins::host::PluginReservation* left =
        fx.memory->reserve("slice", 0, fx.fold.get());
    REQUIRE(left != nullptr);
    dstd::String why;
    REQUIRE(fx.memory->try_grow(*left, 1 << 20, why));
    CHECK(fx.pool().reserved() >= (1u << 20));
    fx.fold.reset();
    CHECK(fx.pool().reserved() == 0);
}
