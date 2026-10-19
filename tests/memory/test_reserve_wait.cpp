#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <random>
#include <thread>
#include <vector>

using namespace dftracer::utils::memory;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;

struct Budget {
    explicit Budget(std::size_t bytes) {
        Config c;
        c.budget = bytes;
        configure(c);
    }
    ~Budget() { configure(Config{}); }
};

struct Wake {
    std::atomic<int> calls{0};
    int id = 0;
    std::vector<int>* log = nullptr;
};

void on_resume(void* context) noexcept {
    auto* wake = static_cast<Wake*>(context);
    if (wake->log != nullptr) wake->log->push_back(wake->id);
    wake->calls.fetch_add(1, std::memory_order_release);
}

struct Waiter {
    Waiter(Pool& pool, std::size_t bytes, int id = 0,
           std::vector<int>* log = nullptr) {
        wake.id = id;
        wake.log = log;
        node = ReserveWait::create(pool, bytes, on_resume, &wake);
        REQUIRE(node != nullptr);
    }
    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;
    ~Waiter() { node->drop(); }

    Wake wake;
    ReserveWait* node = nullptr;
};

Pool& root() { return Manager::get().root(); }

}  // namespace

TEST_CASE("a waiter with room grants itself and is not resumed") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    Waiter w(*a, 8 * MIB);
    CHECK(w.node->push() == Status::Ok);
    CHECK(w.node->state() == WaitState::Granted);
    CHECK(w.wake.calls.load() == 0);
    {
        Reservation r = w.node->take();
        REQUIRE(r);
        CHECK(r.bytes() == 8 * MIB);
        CHECK(root().reserved() == 8 * MIB);
        CHECK(!w.node->take());
    }
    CHECK(root().reserved() == 0);
}

TEST_CASE("a request larger than the budget or a limit fails at once") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a", PoolKind::Scratch, Limits{16 * MIB});
    Waiter over_budget(root(), 128 * MIB);
    CHECK(over_budget.node->push() == Status::OverBudget);
    Waiter over_limit(*a, 32 * MIB);
    CHECK(over_limit.node->push() == Status::OverBudget);

    Reservation full = root().reserve(64 * MIB);
    REQUIRE(full);
    full.release();
    CHECK(over_budget.wake.calls.load() == 0);
    CHECK(over_limit.wake.calls.load() == 0);
    CHECK(root().reserved() == 0);
}

TEST_CASE("waiters are granted first in, first out") {
    Budget budget(64 * MIB);
    PoolHandle holds = root().child("holds");
    PoolHandle wait = root().child("wait");
    Reservation r1 = holds->reserve(32 * MIB);
    PoolHandle holds2 = root().child("holds2");
    Reservation r2 = holds2->reserve(32 * MIB);
    REQUIRE(r1);
    REQUIRE(r2);

    std::vector<int> log;
    log.reserve(8);
    Waiter w1(*wait, 32 * MIB, 1, &log);
    Waiter w2(*wait, 32 * MIB, 2, &log);
    CHECK(w1.node->push() == Status::Pending);
    CHECK(w2.node->push() == Status::Pending);

    r1.release();
    CHECK(log == std::vector<int>{1});
    CHECK(w1.node->state() == WaitState::Granted);
    CHECK(w2.node->state() == WaitState::Waiting);

    r2.release();
    CHECK(log == std::vector<int>{1, 2});
    CHECK(w2.node->state() == WaitState::Granted);
    CHECK(root().reserved() == 64 * MIB);
    {
        Reservation g1 = w1.node->take();
        Reservation g2 = w2.node->take();
        CHECK(g1.bytes() == 32 * MIB);
        CHECK(g2.bytes() == 32 * MIB);
    }
    CHECK(root().reserved() == 0);
}

namespace {

struct PushInside {
    Wake wake;
    std::vector<ReserveWait*> nodes;
    std::vector<Status> pushed;
};

void push_inside(void* context) noexcept {
    auto* inside = static_cast<PushInside*>(context);
    on_resume(&inside->wake);
    for (ReserveWait* w : inside->nodes) inside->pushed.push_back(w->push());
}

}  // namespace

TEST_CASE("pushes that land while a grant runs are granted by arrival") {
    Budget budget(64 * MIB);
    PoolHandle h = root().child("h");
    PoolHandle a = root().child("a");
    Reservation held = h->reserve(64 * MIB);
    REQUIRE(held);

    std::vector<int> log;
    log.reserve(8);
    PushInside inside;
    inside.wake.id = 0;
    inside.wake.log = &log;
    ReserveWait* first = ReserveWait::create(*a, 8 * MIB, push_inside, &inside);
    REQUIRE(first != nullptr);
    Waiter w1(*a, 8 * MIB, 1, &log);
    Waiter w2(*a, 8 * MIB, 2, &log);
    Waiter w3(*a, 8 * MIB, 3, &log);
    inside.nodes = {w1.node, w2.node, w3.node};
    inside.pushed.reserve(3);
    CHECK(first->push() == Status::Pending);

    held.release();
    CHECK(inside.pushed == std::vector<Status>{Status::Pending, Status::Pending,
                                               Status::Pending});
    CHECK(log == std::vector<int>{0, 1, 2, 3});
    first->drop();
}

TEST_CASE("the grant stops at the first waiter that does not fit") {
    Budget budget(64 * MIB);
    PoolHandle h1 = root().child("h1");
    PoolHandle h2 = root().child("h2");
    PoolHandle big = root().child("big");
    PoolHandle small = root().child("small");
    Reservation r1 = h1->reserve(32 * MIB);
    Reservation r2 = h2->reserve(32 * MIB);
    REQUIRE(r1);
    REQUIRE(r2);

    std::vector<int> log;
    log.reserve(8);
    Waiter w1(*big, 48 * MIB, 1, &log);
    Waiter w2(*small, 8 * MIB, 2, &log);
    CHECK(w1.node->push() == Status::Pending);
    CHECK(w2.node->push() == Status::Pending);

    r1.release();
    CHECK(log.empty());
    CHECK(w2.node->state() == WaitState::Waiting);
    CHECK(root().reserved() == 32 * MIB);

    r2.release();
    CHECK(log == std::vector<int>{1, 2});
    CHECK(root().reserved() == 56 * MIB);
}

TEST_CASE("a limit on the waiter's pool is checked at grant") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a", PoolKind::Scratch, Limits{16 * MIB});
    Reservation held = a->reserve(16 * MIB);
    REQUIRE(held);
    Waiter w(*a, 8 * MIB);
    CHECK(w.node->push() == Status::Pending);

    PoolHandle other = root().child("other");
    Reservation unrelated = other->reserve(8 * MIB);
    REQUIRE(unrelated);
    unrelated.release();
    CHECK(w.node->state() == WaitState::Waiting);

    held.release();
    CHECK(w.node->state() == WaitState::Granted);
    CHECK(w.wake.calls.load() == 1);
    CHECK(a->reserved() == 8 * MIB);
}

TEST_CASE("a cancel before the grant leaves no bytes and no resume") {
    Budget budget(64 * MIB);
    PoolHandle h = root().child("h");
    PoolHandle a = root().child("a");
    Reservation held = h->reserve(64 * MIB);
    REQUIRE(held);
    Waiter w(*a, 16 * MIB);
    CHECK(w.node->push() == Status::Pending);
    CHECK(w.node->cancel());
    CHECK(!w.node->cancel());
    CHECK(w.node->state() == WaitState::Cancelled);

    held.release();
    CHECK(w.wake.calls.load() == 0);
    CHECK(!w.node->take());
    CHECK(root().reserved() == 0);
}

TEST_CASE("a cancel at the head lets the waiters behind it through") {
    Budget budget(64 * MIB);
    PoolHandle h = root().child("h");
    PoolHandle a = root().child("a");
    Reservation held = h->reserve(32 * MIB);
    REQUIRE(held);
    Waiter head(*a, 48 * MIB);
    Waiter next(*a, 16 * MIB);
    CHECK(head.node->push() == Status::Pending);
    CHECK(next.node->push() == Status::Pending);
    CHECK(head.node->cancel());
    CHECK(next.node->state() == WaitState::Granted);
    CHECK(next.wake.calls.load() == 1);
    CHECK(head.wake.calls.load() == 0);
}

TEST_CASE("a cancel after the grant fails and the waiter keeps its bytes") {
    Budget budget(64 * MIB);
    PoolHandle h = root().child("h");
    PoolHandle a = root().child("a");
    Reservation held = h->reserve(64 * MIB);
    REQUIRE(held);
    Waiter w(*a, 16 * MIB);
    CHECK(w.node->push() == Status::Pending);
    held.release();
    CHECK(w.wake.calls.load() == 1);
    CHECK(!w.node->cancel());
    CHECK(w.node->state() == WaitState::Granted);
    Reservation r = w.node->take();
    CHECK(r.bytes() == 16 * MIB);
    CHECK(root().reserved() == 16 * MIB);
}

TEST_CASE("a granted waiter dropped without take gives its bytes back") {
    Budget budget(64 * MIB);
    PoolHandle h = root().child("h");
    PoolHandle a = root().child("a");
    Reservation held = h->reserve(64 * MIB);
    REQUIRE(held);
    {
        Waiter w(*a, 16 * MIB);
        CHECK(w.node->push() == Status::Pending);
        held.release();
        CHECK(w.node->state() == WaitState::Granted);
        CHECK(root().reserved() == 16 * MIB);
    }
    CHECK(root().reserved() == 0);
}

TEST_CASE("a lower external term wakes a waiter") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    ExternalSlot ext = attach_external("ext");
    ext.set(64 * MIB);
    Waiter w(*a, 16 * MIB);
    CHECK(w.node->push() == Status::Pending);
    ext.set(32 * MIB);
    CHECK(w.node->state() == WaitState::Granted);
    CHECK(w.wake.calls.load() == 1);
}

TEST_CASE("only a lower counted external term wakes a waiter") {
    Budget budget(64 * MIB);
    PoolHandle a = root().child("a");
    ExternalSlot ext = attach_external("ext");
    ext.add(static_cast<std::ptrdiff_t>(64 * MIB), ExternalSlot::Kind::Counted);
    ext.add(static_cast<std::ptrdiff_t>(64 * MIB),
            ExternalSlot::Kind::Reported);
    Waiter w(*a, 16 * MIB);
    CHECK(w.node->push() == Status::Pending);
    ext.add(-static_cast<std::ptrdiff_t>(64 * MIB),
            ExternalSlot::Kind::Reported);
    CHECK(w.node->state() == WaitState::Waiting);
    CHECK(w.wake.calls.load() == 0);
    ext.add(-static_cast<std::ptrdiff_t>(32 * MIB),
            ExternalSlot::Kind::Counted);
    CHECK(w.node->state() == WaitState::Granted);
    CHECK(w.wake.calls.load() == 1);
}

TEST_CASE("no lost wakeup when a release races a push") {
    Budget budget(64 * MIB);
    PoolHandle h = root().child("h");
    PoolHandle a = root().child("a");
    constexpr int ROUNDS = 400;
    int lost = 0;
    int double_resume = 0;
    for (int round = 0; round < ROUNDS; ++round) {
        Reservation held = h->reserve(64 * MIB);
        REQUIRE(held);
        Waiter w(*a, 64 * MIB);
        std::atomic<bool> ready{false};
        std::atomic<bool> go{false};
        Status pushed = Status::Pending;
        std::thread pusher([&] {
            ready.store(true, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
            }
            pushed = w.node->push();
        });
        while (!ready.load(std::memory_order_acquire)) {
        }
        go.store(true, std::memory_order_release);
        held.release();
        pusher.join();
        int calls = w.wake.calls.load(std::memory_order_acquire);
        if (w.node->state() != WaitState::Granted) ++lost;
        if (pushed == Status::Ok ? calls != 0 : calls != 1) ++double_resume;
    }
    CHECK(lost == 0);
    CHECK(double_resume == 0);
    CHECK(root().reserved() == 0);
}

TEST_CASE("many threads reserve, wait, cancel and release") {
    Budget budget(64 * MIB);
    constexpr int THREADS = 8;
    constexpr int ROUNDS = 300;
    constexpr std::size_t SIZES[] = {4 * MIB, 8 * MIB, 16 * MIB, 24 * MIB};
    std::vector<PoolHandle> pools;
    for (int t = 0; t < THREADS; ++t) pools.push_back(root().child("t"));

    std::atomic<int> waits{0};
    std::atomic<int> self{0};
    std::atomic<int> resumed{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 rng(31 + static_cast<unsigned>(t));
            Pool& pool = *pools[static_cast<std::size_t>(t)];
            for (int i = 0; i < ROUNDS; ++i) {
                std::size_t size = SIZES[rng() % 4];
                if (rng() % 4 == 0) {
                    Reservation plain = pool.reserve(size);
                    std::this_thread::yield();
                    continue;
                }
                waits.fetch_add(1, std::memory_order_relaxed);
                Wake wake;
                ReserveWait* w =
                    ReserveWait::create(pool, size, on_resume, &wake);
                if (w == nullptr) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                Status s = w->push();
                bool granted = true;
                if (s == Status::Ok) {
                    self.fetch_add(1, std::memory_order_relaxed);
                } else if (s != Status::Pending) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                    granted = false;
                } else {
                    if (rng() % 3 == 0) {
                        std::this_thread::yield();
                        if (w->cancel()) {
                            cancelled.fetch_add(1, std::memory_order_relaxed);
                            granted = false;
                        }
                    }
                    if (granted) {
                        while (wake.calls.load(std::memory_order_acquire) ==
                               0) {
                            std::this_thread::yield();
                        }
                        resumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (granted && rng() % 8 != 0) {
                    Reservation r = w->take();
                    if (!r || r.bytes() != size) {
                        bad.fetch_add(1, std::memory_order_relaxed);
                    }
                    std::this_thread::yield();
                }
                w->drop();
                int calls = wake.calls.load(std::memory_order_acquire);
                if (calls > 1 || (s == Status::Ok && calls != 0)) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& th : threads) th.join();
    CHECK(bad.load() == 0);
    CHECK(self.load() + resumed.load() + cancelled.load() == waits.load());
    CHECK(root().reserved() == 0);
}
