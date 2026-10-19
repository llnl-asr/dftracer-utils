#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils::memory;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;

struct Settings {
    explicit Settings(std::size_t budget, std::size_t hard_cap = 0) {
        Config c;
        c.budget = budget;
        c.hard_cap = hard_cap;
        configure(c);
    }
    ~Settings() { configure(Config{}); }
};

Pool& root() { return Manager::get().root(); }

void touch(char* p, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; i += 4096)
        p[i] = static_cast<char>(i | 1);
    p[bytes - 1] = 1;
}

// The near-limit signal reads the machine. A test of the budget signal needs
// it off, so it runs only where the process is far from its limit.
bool far_from_limit() {
    Stats s = Manager::get().stats();
    return s.limit > Manager::get().limit_headroom() &&
           s.measured + Manager::get().limit_headroom() < s.limit;
}

}  // namespace

TEST_CASE("measured grows after touching 64 MiB of plain malloc memory") {
    Manager& m = Manager::get();
    Stats before = m.stats();
    REQUIRE(before.measured > 0);
    auto* p = static_cast<char*>(std::malloc(64 * MIB));
    REQUIRE(p != nullptr);
    touch(p, 64 * MIB);
    Stats after = m.stats();
    CHECK(after.measured >= before.measured + 48 * MIB);
    CHECK(after.reserved == before.reserved);
    CHECK(after.unattributed >= before.unattributed + 48 * MIB);
    std::free(p);
}

TEST_CASE("unattributed is measured minus ours") {
    Manager& m = Manager::get();
    Stats none = m.stats();
    std::size_t ours = none.reserved + none.external;
    CHECK(none.unattributed ==
          (none.measured > ours ? none.measured - ours : 0));

    ExternalSlot external = attach_external("test");
    external.set(5 * MIB);
    Stats s = m.stats();
    CHECK(s.external == none.external + 5 * MIB);
    ours = s.reserved + s.external;
    CHECK(s.unattributed == (s.measured > ours ? s.measured - ours : 0));
}

TEST_CASE("under_pressure turns on past the budget and off after release") {
    Settings settings(256 * MIB);
    Manager& m = Manager::get();
    if (!far_from_limit()) {
        MESSAGE("measured memory is near the process limit; test not run");
        return;
    }
    PoolHandle pool = root().child("pressure");
    CHECK(!m.under_pressure());
    {
        Reservation r = pool->reserve(200 * MIB);
        REQUIRE(r);
        CHECK(!m.under_pressure());
        ExternalSlot external = attach_external("test");
        external.set(100 * MIB);
        CHECK(m.under_pressure());
        external.set(0);
        CHECK(!m.under_pressure());
    }
    CHECK(!m.under_pressure());

    ExternalSlot external = attach_external("test");
    external.set(257 * MIB);
    CHECK(m.under_pressure());
    external.detach();
    CHECK(!m.under_pressure());
}

TEST_CASE("under_pressure follows a new budget") {
    Settings settings(256 * MIB);
    Manager& m = Manager::get();
    if (!far_from_limit()) {
        MESSAGE("measured memory is near the process limit; test not run");
        return;
    }
    ExternalSlot external = attach_external("test");
    external.set(100 * MIB);
    CHECK(!m.under_pressure());
    m.set_budget(64 * MIB);
    CHECK(m.under_pressure());
}

TEST_CASE("external bytes count toward the budget") {
    Settings settings(64 * MIB);
    PoolHandle pool = root().child("external");
    ExternalSlot external = attach_external("test");
    external.set(40 * MIB);
    Reservation over = pool->reserve(30 * MIB);
    CHECK(!over);
    CHECK(over.status() == Status::OverBudget);
    Reservation fits = pool->reserve(20 * MIB);
    CHECK(fits);
    external.set(0);
    Reservation more = pool->reserve(30 * MIB);
    CHECK(more);
}

TEST_CASE("reported external bytes show in stats but not in the budget") {
    Settings settings(64 * MIB);
    PoolHandle pool = root().child("reported");
    const std::size_t before = Manager::get().stats().external;
    ExternalSlot external = attach_external("test");
    external.add(static_cast<std::ptrdiff_t>(100 * MIB),
                 ExternalSlot::Kind::Reported);
    CHECK(external.bytes() == 100 * MIB);
    CHECK(Manager::get().stats().external == before + 100 * MIB);
    Reservation fits = pool->reserve(30 * MIB);
    CHECK(fits);
    external.add(static_cast<std::ptrdiff_t>(30 * MIB),
                 ExternalSlot::Kind::Counted);
    Reservation over = pool->reserve(10 * MIB);
    CHECK(over.status() == Status::OverBudget);
    external.add(-static_cast<std::ptrdiff_t>(30 * MIB),
                 ExternalSlot::Kind::Counted);
    external.add(-static_cast<std::ptrdiff_t>(100 * MIB),
                 ExternalSlot::Kind::Reported);
    CHECK(external.bytes() == 0);
    CHECK(Manager::get().stats().external == before);
}

TEST_CASE("concurrent external adds lose no bytes") {
    ExternalSlot external = attach_external("test");
    constexpr int THREADS = 8;
    constexpr int ROUNDS = 10000;
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t)
        threads.emplace_back([&external, t] {
            const auto kind = t % 2 == 0 ? ExternalSlot::Kind::Counted
                                         : ExternalSlot::Kind::Reported;
            for (int i = 0; i < ROUNDS; ++i) {
                external.add(3, kind);
                external.add(-1, kind);
            }
        });
    for (std::thread& t : threads) t.join();
    CHECK(external.bytes() == std::size_t{2} * THREADS * ROUNDS);
}

TEST_CASE("external slots are listed by name and freed on detach") {
    ExternalSlot a = attach_external("rocksdb memtables");
    ExternalSlot b = attach_external(std::string(100, 'x'));
    a.set(3);
    b.set(4);
    std::size_t seen = 0;
    for (const ExternalStats& e : external_stats()) {
        if (e.name == "rocksdb memtables") {
            CHECK(e.bytes == 3);
            ++seen;
        } else if (e.name == std::string(47, 'x')) {
            CHECK(e.bytes == 4);
            ++seen;
        }
    }
    CHECK(seen == 2);
    a.detach();
    CHECK(a.bytes() == 0);
    for (const ExternalStats& e : external_stats()) {
        CHECK(e.name != "rocksdb memtables");
    }
}

TEST_CASE("the external table fills and frees") {
    std::vector<ExternalSlot> slots;
    for (std::size_t i = 0; i < EXTERNAL_SLOTS; ++i) {
        slots.push_back(attach_external("slot"));
        REQUIRE(slots.back());
    }
    ExternalSlot none = attach_external("none");
    CHECK(!none);
    CHECK(none.status() == Status::SlotTableFull);
    none.set(7);
    CHECK(none.bytes() == 0);
}

TEST_CASE("the hard cap throws and try_allocate returns null") {
    Settings settings(0, 64 * MIB);
    CHECK_THROWS_AS(allocate(128 * MIB, 16), std::bad_alloc);
    CHECK(try_allocate(128 * MIB, 16) == nullptr);

    std::vector<void*> blocks;
    while (void* p = try_allocate(8 * MIB, 16)) {
        blocks.push_back(p);
        REQUIRE(blocks.size() <= 8);
    }
    CHECK(!blocks.empty());
    CHECK_THROWS_AS(allocate(8 * MIB, 16), std::bad_alloc);
    void* small = try_allocate(64, 16);
    CHECK(small != nullptr);
    deallocate(small);
    for (void* p : blocks) deallocate(p);

    void* again = try_allocate(8 * MIB, 16);
    CHECK(again != nullptr);
    deallocate(again);
}

TEST_CASE("no hard cap means large blocks are served") {
    Settings settings(0, 0);
    void* p = try_allocate(128 * MIB, 16);
    REQUIRE(p != nullptr);
    deallocate(p);
}

TEST_CASE("configure sets what config reports and zero selects the default") {
    Config c;
    c.budget = 1024 * MIB;
    c.hard_cap = 3 * 1024 * MIB;
    c.limit_headroom = 10 * MIB;
    c.spill_headroom = 5 * MIB;
    configure(c);
    Config got = config();
    CHECK(got.budget == 1024 * MIB);
    CHECK(got.hard_cap == 3 * 1024 * MIB);
    CHECK(got.limit_headroom == 10 * MIB);
    CHECK(got.spill_headroom == 5 * MIB);

    c.limit_headroom = 0;
    c.spill_headroom = 0;
    configure(c);
    got = config();
    CHECK(got.spill_headroom == 32 * MIB);
    CHECK(got.limit_headroom >= 256 * MIB);

    c.budget = 8192 * MIB;
    configure(c);
    CHECK(config().spill_headroom == 8192 * MIB / 100 * 2);

    configure(Config{});
    got = config();
    CHECK(got.hard_cap == 0);
    CHECK(got.budget > 0);
}
