// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/detail/heap.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/memory/detail/pool_access.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <vector>

using namespace dftracer::utils::memory;
using namespace dftracer::utils::memory::detail;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;

struct Holder : Reclaimer {
    explicit Holder(std::size_t initial) : bytes(initial) {}
    std::size_t reclaimable() const override { return bytes; }
    std::size_t reclaim(std::size_t target) override {
        std::size_t given = target < bytes ? target : bytes;
        bytes -= given;
        return given;
    }
    std::size_t bytes;
};

struct Budget {
    explicit Budget(std::size_t bytes, std::size_t spill_headroom = 0) {
        Config c;
        c.budget = bytes;
        c.spill_headroom = spill_headroom;
        configure(c);
    }
    ~Budget() { configure(Config{}); }
};

Pool& root() { return Manager::get().root(); }

}  // namespace

TEST_CASE("over budget posts to the largest reclaimable slot first") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    PoolHandle b = root().child("b");
    PoolHandle c = root().child("c");
    Holder small(10 * MIB), medium(30 * MIB), large(50 * MIB);
    ReclaimSlot s_small = PoolAccess::attach(*a, small);
    ReclaimSlot s_medium = PoolAccess::attach(*a, medium);
    ReclaimSlot s_large = PoolAccess::attach(*b, large);

    Reservation held = PoolAccess::reserve(*b, 40 * MIB);
    REQUIRE(held);
    Reservation over = PoolAccess::reserve(*c, 40 * MIB);
    CHECK(!over);
    CHECK(over.status() == Status::Pending);
    CHECK(s_large.take_request() == 16 * MIB);
    CHECK(s_medium.take_request() == 0);
    CHECK(s_small.take_request() == 0);
}

TEST_CASE("slots under the same child of the root come first") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    PoolHandle inner = a->child("inner");
    PoolHandle b = root().child("b");
    Holder near(30 * MIB), far(50 * MIB);
    ReclaimSlot s_near = PoolAccess::attach(*a, near);
    ReclaimSlot s_far = PoolAccess::attach(*b, far);

    Reservation held = PoolAccess::reserve(*b, 40 * MIB);
    REQUIRE(held);
    Reservation over = PoolAccess::reserve(*inner, 40 * MIB);
    CHECK(over.status() == Status::Pending);
    CHECK(s_near.take_request() == 16 * MIB);
    CHECK(s_far.take_request() == 0);
}

TEST_CASE("a shortfall larger than one slot spills over to the next") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    PoolHandle b = root().child("b");
    Holder first(10 * MIB), second(30 * MIB);
    ReclaimSlot s_first = PoolAccess::attach(*a, first);
    ReclaimSlot s_second = PoolAccess::attach(*b, second);

    Reservation held = PoolAccess::reserve(*b, 60 * MIB);
    REQUIRE(held);
    Reservation over = PoolAccess::reserve(*a, 20 * MIB);
    CHECK(over.status() == Status::Pending);
    CHECK(s_first.take_request() == 10 * MIB);
    CHECK(s_second.take_request() == 6 * MIB);
}

TEST_CASE("a request is taken once") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    Holder holder(30 * MIB);
    ReclaimSlot slot = PoolAccess::attach(*a, holder);
    Reservation held = PoolAccess::reserve(*a, 40 * MIB);
    REQUIRE(held);
    Reservation over = PoolAccess::reserve(*a, 40 * MIB);
    CHECK(over.status() == Status::Pending);
    CHECK(slot.take_request() == 16 * MIB);
    CHECK(slot.take_request() == 0);
}

TEST_CASE("the first reclaimable value is read at attach") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    Holder holder(30 * MIB);
    ReclaimSlot slot = PoolAccess::attach(*a, holder);
    Reservation held = PoolAccess::reserve(*a, 40 * MIB);
    Reservation over = PoolAccess::reserve(*a, 40 * MIB);
    CHECK(over.status() == Status::Pending);
    CHECK(slot.take_request() == 16 * MIB);

    slot.set_reclaimable(0);
    Reservation again = PoolAccess::reserve(*a, 40 * MIB);
    CHECK(again.status() == Status::OverBudget);
    CHECK(slot.take_request() == 0);
}

TEST_CASE("a pending request is not asked for twice") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    Holder holder(16 * MIB);
    ReclaimSlot slot = PoolAccess::attach(*a, holder);
    Reservation held = PoolAccess::reserve(*a, 40 * MIB);
    Reservation first = PoolAccess::reserve(*a, 40 * MIB);
    CHECK(first.status() == Status::Pending);
    Reservation second = PoolAccess::reserve(*a, 40 * MIB);
    CHECK(second.status() == Status::OverBudget);
    CHECK(slot.take_request() == 16 * MIB);
}

TEST_CASE("a reclaiming pool reserves within the headroom") {
    Budget budget(64 * MIB, 32 * MIB);
    PoolHandle busy = root().child("busy");
    PoolHandle spill = root().child("spill");
    Holder other(50 * MIB), own(50 * MIB);
    ReclaimSlot s_other = PoolAccess::attach(*busy, other);
    ReclaimSlot s_own = PoolAccess::attach(*spill, own);

    Reservation held = PoolAccess::reserve(*busy, 64 * MIB);
    REQUIRE(held);
    CHECK(PoolAccess::reserve(*spill, MIB).status() == Status::Pending);
    s_other.take_request();
    s_own.take_request();

    ReclaimGuard guard = s_own.begin_reclaim();
    Reservation within = PoolAccess::reserve(*spill, 20 * MIB);
    CHECK(within);
    Reservation past = PoolAccess::reserve(*spill, 20 * MIB);
    CHECK(!past);
    CHECK(past.status() == Status::OverBudget);
    CHECK(s_other.take_request() == 0);
    CHECK(s_own.take_request() == 0);
}

TEST_CASE("the headroom ends with the guard") {
    Budget budget(64 * MIB, 32 * MIB);
    PoolHandle busy = root().child("busy");
    PoolHandle spill = root().child("spill");
    Holder own(50 * MIB);
    ReclaimSlot slot = PoolAccess::attach(*spill, own);
    Reservation held = PoolAccess::reserve(*busy, 64 * MIB);
    REQUIRE(held);
    {
        ReclaimGuard guard = slot.begin_reclaim();
        Reservation within = PoolAccess::reserve(*spill, 8 * MIB);
        CHECK(within);
    }
    Reservation after = PoolAccess::reserve(*spill, 8 * MIB);
    CHECK(after.status() == Status::Pending);
}

TEST_CASE("nothing reclaimable gives over budget") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    PoolHandle b = root().child("b");
    Reservation held = PoolAccess::reserve(*a, 40 * MIB);
    REQUIRE(held);
    CHECK(PoolAccess::reserve(*b, 40 * MIB).status() == Status::OverBudget);

    Holder empty(0);
    ReclaimSlot slot = PoolAccess::attach(*b, empty);
    CHECK(PoolAccess::reserve(*b, 40 * MIB).status() == Status::OverBudget);
    CHECK(slot.take_request() == 0);
}

TEST_CASE("a detached slot is never posted to") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    PoolHandle b = root().child("b");
    Holder gone(50 * MIB);
    {
        ReclaimSlot slot = PoolAccess::attach(*a, gone);
        REQUIRE(slot);
    }
    Reservation held = PoolAccess::reserve(*a, 40 * MIB);
    REQUIRE(held);
    CHECK(PoolAccess::reserve(*b, 40 * MIB).status() == Status::OverBudget);

    Holder next(0);
    ReclaimSlot reused = PoolAccess::attach(*b, next);
    CHECK(reused.take_request() == 0);
}

TEST_CASE("a request larger than the budget posts nothing") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    Holder holder(50 * MIB);
    ReclaimSlot slot = PoolAccess::attach(*a, holder);
    Reservation r = PoolAccess::reserve(*a, 65 * MIB);
    CHECK(!r);
    CHECK(r.status() == Status::OverBudget);
    CHECK(slot.take_request() == 0);
}

TEST_CASE("a request larger than a pool limit posts nothing") {
    Budget budget(64 * MIB);
    PoolHandle limited =
        root().child("limited", PoolKind::Scratch, Limits{8 * MIB});
    Holder holder(50 * MIB);
    ReclaimSlot slot = PoolAccess::attach(*limited, holder);
    Reservation r = PoolAccess::reserve(*limited, 9 * MIB);
    CHECK(r.status() == Status::OverBudget);
    CHECK(slot.take_request() == 0);
}

TEST_CASE("a pool at its limit asks the holders") {
    Budget budget(64 * MIB);
    PoolHandle limited =
        root().child("limited", PoolKind::Scratch, Limits{8 * MIB});
    Holder holder(6 * MIB);
    ReclaimSlot slot = PoolAccess::attach(*limited, holder);
    Reservation held = PoolAccess::reserve(*limited, 6 * MIB);
    REQUIRE(held);
    Reservation over = PoolAccess::reserve(*limited, 4 * MIB);
    CHECK(over.status() == Status::Pending);
    CHECK(slot.take_request() == 2 * MIB);
}

TEST_CASE("the reclaimer table fills and frees") {
    PoolHandle a = root().child("a");
    Holder holder(MIB);
    std::vector<ReclaimSlot> slots;
    for (std::size_t i = 0; i < RECLAIMER_SLOTS; ++i) {
        slots.push_back(PoolAccess::attach(*a, holder));
        REQUIRE(slots.back());
    }
    ReclaimSlot none = PoolAccess::attach(*a, holder);
    CHECK(!none);
    CHECK(none.status() == Status::SlotTableFull);
    CHECK(none.take_request() == 0);
    slots.pop_back();
    ReclaimSlot again = PoolAccess::attach(*a, holder);
    CHECK(again);
}
