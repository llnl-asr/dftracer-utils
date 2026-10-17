#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fresh_process.h"

using namespace dftracer::utils::memory;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;
constexpr std::size_t POOL_SLOTS = 65535;

std::string g_reports;
int g_report_count = 0;

void capture(std::string_view text) noexcept {
    g_reports.append(text.data(), text.size());
    ++g_report_count;
}

struct CaptureReports {
    CaptureReports() {
        g_reports.clear();
        g_report_count = 0;
        previous = set_reporter(capture);
    }
    ~CaptureReports() { set_reporter(previous); }
    Reporter previous;
};

struct BudgetGuard {
    explicit BudgetGuard(std::size_t bytes) {
        Manager::get().set_budget(bytes);
    }
    ~BudgetGuard() { Manager::get().set_budget(0); }
};

Pool& root() { return Manager::get().root(); }

PoolStats find(std::uint16_t id) {
    for (const PoolStats& s : pool_stats()) {
        if (s.id == id) return s;
    }
    FAIL("pool is not open: " << id);
    return {};
}

// General is id 1; blocks allocated with no scope are charged to it.
std::size_t general_live() { return find(1).live; }

}  // namespace

TEST_CASE("the root and general pools are open at the start") {
    std::vector<PoolStats> all = pool_stats();
    REQUIRE(all.size() >= 2);
    CHECK(all[0].id == 0);
    CHECK(all[0].path == "root");
    CHECK(all[1].id == 1);
    CHECK(all[1].parent == 0);
    CHECK(all[1].path == "root/general");
}

TEST_CASE("a scope charges the pool and restores the previous one") {
    PoolHandle outer = root().child("outer");
    PoolHandle inner = outer->child("inner");
    REQUIRE(outer);
    REQUIRE(inner);
    CHECK(inner->path() == "root/outer/inner");

    std::size_t general = general_live();
    void* a = nullptr;
    void* b = nullptr;
    void* c = nullptr;
    {
        Scope s1{*outer};
        a = allocate(100, 16);
        {
            Scope s2{*inner};
            b = allocate(200, 16);
        }
        c = allocate(300, 16);
    }
    void* d = allocate(400, 16);

    CHECK(inner->blocks() == 1);
    CHECK(inner->live() >= 200);
    CHECK(outer->blocks() == 3);
    CHECK(outer->live() >= 600);
    CHECK(outer->live() > inner->live());
    CHECK(general_live() >= general + 400);
    CHECK(root().blocks() >= 4);

    deallocate(a);
    deallocate(b);
    deallocate(c);
    deallocate(d);
    CHECK(outer->blocks() == 0);
    CHECK(outer->live() == 0);
    CHECK(inner->live() == 0);
    CHECK(general_live() == general);
}

TEST_CASE("a free credits the pool that was charged, not the scope") {
    PoolHandle owner = root().child("owner");
    PoolHandle other = root().child("other");
    std::size_t before = root().live();
    void* p;
    {
        Scope s{*owner};
        p = allocate(1000, 16);
    }
    std::size_t held = owner->live();
    CHECK(held >= 1000);
    {
        Scope s{*other};
        deallocate(p);
        CHECK(other->live() == 0);
        CHECK(other->blocks() == 0);
    }
    CHECK(owner->live() == 0);
    CHECK(owner->blocks() == 0);
    CHECK(root().live() == before);
}

TEST_CASE("every size tier is charged and credited to its pool") {
    PoolHandle pool = root().child("tiers");
    const std::size_t sizes[] = {1,       100,     4096,
                                 32768,   40000,   MIB,
                                 MIB + 1, 3 * MIB, (std::size_t{64} << 20) + 1};
    for (std::size_t size : sizes) {
        void* p;
        {
            Scope s{*pool};
            p = allocate(size, 16);
        }
        CAPTURE(size);
        CHECK(pool->blocks() == 1);
        CHECK(pool->live() >= size);
        deallocate(p);
        CHECK(pool->blocks() == 0);
        CHECK(pool->live() == 0);
    }
}

TEST_CASE("a pool the first thread frees into stays exact across entries") {
    // Pool ids 16 apart share an entry in the per-thread cache.
    std::vector<PoolHandle> pools;
    for (int i = 0; i < 40; ++i) pools.push_back(root().child("many"));
    std::vector<void*> blocks;
    for (PoolHandle& p : pools) {
        Scope s{*p};
        blocks.push_back(allocate(64, 16));
    }
    for (PoolHandle& p : pools) CHECK(p->blocks() == 1);
    for (void* b : blocks) deallocate(b);
    for (PoolHandle& p : pools) {
        CHECK(p->blocks() == 0);
        CHECK(p->live() == 0);
    }
}

TEST_CASE("closing a results pool hands its live bytes to the parent") {
    CaptureReports capture_reports;
    PoolHandle parent = root().child("parent");
    PoolHandle results = parent->child("results", PoolKind::Results);
    void* p;
    {
        Scope s{*results};
        p = allocate(5000, 16);
    }
    std::size_t held = results->live();
    REQUIRE(held >= 5000);
    CHECK(parent->adopted() == 0);

    results.close();
    CHECK(g_report_count == 0);
    CHECK(parent->adopted() == held);
    CHECK(parent->live() == held);
    CHECK(root().adopted() == 0);
    bool found_closed = false;
    for (const PoolStats& s : pool_stats()) {
        if (s.path == "root/parent/results") found_closed = true;
    }
    CHECK_FALSE(found_closed);

    deallocate(p);
    CHECK(parent->adopted() == 0);
    CHECK(parent->live() == 0);
}

TEST_CASE("closing a scratch pool with live bytes reports its path") {
    CaptureReports capture_reports;
    PoolHandle outer = root().child("outer");
    PoolHandle leaky = outer->child("leaky");
    void* p;
    {
        Scope s{*leaky};
        p = allocate(777, 16);
    }
    leaky.close();
    CHECK(g_report_count == 1);
    CHECK(g_reports.find("root/outer/leaky") != std::string::npos);
    CHECK(g_reports.find("1 blocks") != std::string::npos);
    CHECK(g_reports.back() == '\n');
    CHECK(Manager::get().leaked());
    deallocate(p);
}

TEST_CASE("closing a pool with an open child reports the child count") {
    CaptureReports capture_reports;
    PoolHandle outer = root().child("holder");
    PoolHandle kept = outer->child("kept");
    outer.close();
    CHECK(g_report_count == 1);
    CHECK(g_reports.find("root/holder") != std::string::npos);
    CHECK(g_reports.find("1 open children") != std::string::npos);
}

TEST_CASE("closing a clean scratch pool reports nothing") {
    CaptureReports capture_reports;
    PoolHandle pool = root().child("clean");
    void* p;
    {
        Scope s{*pool};
        p = allocate(64, 16);
    }
    deallocate(p);
    pool.close();
    CHECK(g_report_count == 0);
}

TEST_CASE("a slot is not reused while a block still names it") {
    CaptureReports capture_reports;
    PoolHandle results = root().child("kept-alive", PoolKind::Results);
    std::uint16_t id = results->id();
    void* p;
    {
        Scope s{*results};
        p = allocate(100, 16);
    }
    results.close();

    for (int i = 0; i < 200; ++i) {
        PoolHandle fresh = root().child("short");
        REQUIRE(fresh);
        CHECK(fresh->id() != id);
    }
    deallocate(p);
}

TEST_CASE("a clean closed slot is reused with a new generation") {
    PoolHandle first = root().child("first");
    std::uint16_t id = first->id();
    first.close();
    PoolHandle second = root().child("second");
    CHECK(second->id() == id);
    CHECK(second->name() == "second");
    CHECK(second->live() == 0);
    PoolHandle third = root().child("third");
    CHECK(third->id() != id);
}

TEST_CASE("a name over 47 bytes is cut") {
    PoolHandle pool = root().child(std::string(100, 'n'));
    CHECK(pool->name().size() == 47);
}

TEST_CASE("reservations reach the root in quanta") {
    BudgetGuard budget{std::size_t{1} << 30};
    PoolHandle pool = root().child("quanta");
    std::size_t base = root().reserved();

    Reservation a = pool->reserve(100);
    REQUIRE(a);
    CHECK(pool->reserved() == 100);
    CHECK(root().reserved() == base + MIB);
    Reservation b = pool->reserve(100);
    CHECK(pool->reserved() == 200);
    CHECK(root().reserved() == base + MIB);
    Reservation c = pool->reserve(MIB);
    CHECK(root().reserved() == base + 2 * MIB);

    c.release();
    CHECK(root().reserved() == base + MIB);
    b.release();
    a.release();
    CHECK(pool->reserved() == 0);
    CHECK(root().reserved() == base);

    Reservation big = pool->reserve(17 * MIB);
    CHECK(root().reserved() == base + 20 * MIB);
    Reservation huge = pool->reserve(60 * MIB);
    CHECK(root().reserved() == base + 80 * MIB);
    big.release();
    huge.release();
    CHECK(root().reserved() == base);
}

TEST_CASE("a moved reservation releases once") {
    BudgetGuard budget{std::size_t{1} << 30};
    PoolHandle pool = root().child("moved");
    Reservation a = pool->reserve(10);
    Reservation b = std::move(a);
    CHECK_FALSE(a);
    CHECK(b);
    CHECK(pool->reserved() == 10);
    b = Reservation{};
    CHECK(pool->reserved() == 0);
}

TEST_CASE("a pool limit refuses a reservation and leaves the totals alone") {
    BudgetGuard budget{std::size_t{1} << 30};
    PoolHandle limited =
        root().child("limited", PoolKind::Scratch, Limits{4 * MIB});
    std::size_t base = root().reserved();
    Reservation fits = limited->reserve(3 * MIB);
    REQUIRE(fits);
    Reservation over = limited->reserve(2 * MIB);
    CHECK_FALSE(over);
    CHECK(over.status() == Status::OverBudget);
    CHECK(limited->reserved() == 3 * MIB);
    CHECK(root().reserved() == base + 3 * MIB);
    Reservation exact = limited->reserve(MIB);
    CHECK(exact);
}

TEST_CASE("a limit on an ancestor refuses a reservation below it") {
    BudgetGuard budget{std::size_t{1} << 30};
    PoolHandle parent =
        root().child("capped", PoolKind::Scratch, Limits{2 * MIB});
    PoolHandle leaf = parent->child("leaf");
    std::size_t base = root().reserved();
    Reservation over = leaf->reserve(3 * MIB);
    CHECK(over.status() == Status::OverBudget);
    CHECK(leaf->reserved() == 0);
    CHECK(parent->reserved() == 0);
    CHECK(root().reserved() == base);
    Reservation fits = leaf->reserve(MIB);
    CHECK(fits);
    CHECK(parent->reserved() == MIB);
}

TEST_CASE("the budget refuses a reservation and a request larger than it") {
    BudgetGuard budget{8 * MIB};
    CHECK(Manager::get().budget() == 8 * MIB);
    PoolHandle one = root().child("one");
    PoolHandle two = root().child("two");
    std::size_t base = root().reserved();
    Reservation first = one->reserve(6 * MIB);
    REQUIRE(first);
    Reservation second = two->reserve(3 * MIB);
    CHECK(second.status() == Status::OverBudget);
    CHECK(two->reserved() == 0);
    CHECK(root().reserved() == base + 6 * MIB);
    Reservation huge = two->reserve(9 * MIB);
    CHECK(huge.status() == Status::OverBudget);
    first.release();
    Reservation retry = two->reserve(3 * MIB);
    CHECK(retry);
}

TEST_CASE("stats report the root totals") {
    BudgetGuard budget{std::size_t{1} << 30};
    PoolHandle pool = root().child("stats");
    Stats before = Manager::get().stats();
    void* p;
    {
        Scope s{*pool};
        p = allocate(1000, 16);
    }
    Reservation r = pool->reserve(5);
    Stats during = Manager::get().stats();
    CHECK(during.live >= before.live + 1000);
    CHECK(during.blocks == before.blocks + 1);
    CHECK(during.pool_reserved == before.pool_reserved + MIB);
    CHECK(during.peak >= during.live);
    CHECK(during.budget == (std::size_t{1} << 30));
    CHECK(during.reserved > 0);
    deallocate(p);
}

TEST_CASE("exported bytes are counted on their pool") {
    PoolHandle pool = root().child("exports");
    pool->add_exported(300);
    CHECK(pool->exported() == 300);
    CHECK(find(pool->id()).exported == 300);
    pool->sub_exported(300);
    CHECK(pool->exported() == 0);
}

TEST_CASE("the table is full after 65,535 slots") {
    auto result = fresh_process::run("pool table full scenario");
    CHECK(result.ok());
}

TEST_CASE("pool table full scenario" * doctest::skip()) {
    fresh_process::announce();
    std::vector<PoolHandle> pools;
    pools.reserve(POOL_SLOTS);
    while (true) {
        PoolHandle h = root().child("slot");
        if (!h) {
            CHECK(h.status() == Status::PoolTableFull);
            break;
        }
        pools.push_back(std::move(h));
    }
    CHECK(pools.size() == POOL_SLOTS - 2);

    std::uint16_t last = pools.back()->id();
    pools.back().close();
    PoolHandle again = root().child("again");
    REQUIRE(again);
    CHECK(again->id() == last);
    PoolHandle none = root().child("none");
    CHECK_FALSE(none);
    CHECK(none.status() == Status::PoolTableFull);
}
