#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/debug.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/memory/settings.h>
#include <doctest/doctest.h>
#include <signal.h>
#include <sys/wait.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "fresh_process.h"

using namespace dftracer::utils::memory;

namespace {

std::string g_reports;

void capture(std::string_view text) noexcept {
    g_reports.append(text.data(), text.size());
}

struct CaptureReports {
    CaptureReports() {
        g_reports.clear();
        previous = set_reporter(capture);
    }
    ~CaptureReports() { set_reporter(previous); }
    Reporter previous;
};

// Each planter is a separate function, so each has its own caller address.
__attribute__((noinline)) void* plant_a(std::size_t size) {
    void* p = allocate(size, 16);
    std::memset(p, 1, size);
    return p;
}

__attribute__((noinline)) void* plant_b(std::size_t size) {
    void* p = allocate(size, 16);
    std::memset(p, 2, size);
    return p;
}

// True when @p fn is the planter whose body holds @p site.
bool inside(std::uintptr_t site, void* (*fn)(std::size_t)) {
    auto start = reinterpret_cast<std::uintptr_t>(fn);
    auto a = reinterpret_cast<std::uintptr_t>(&plant_a);
    auto b = reinterpret_cast<std::uintptr_t>(&plant_b);
    std::uintptr_t other = start == a ? b : a;
    bool before_other = other < start || site < other;
    return site > start && site - start < 256 && before_other;
}

struct ReportedSite {
    std::uintptr_t site = 0;
    std::size_t bytes = 0;
    std::size_t blocks = 0;
};

std::vector<ReportedSite> sites_of(const std::string& text,
                                   const std::string& pool) {
    std::vector<ReportedSite> out;
    std::string key = "pool " + pool + " site 0x";
    for (std::size_t at = text.find(key); at != std::string::npos;
         at = text.find(key, at + 1)) {
        unsigned long long site = 0;
        std::size_t bytes = 0;
        std::size_t blocks = 0;
        if (std::sscanf(text.c_str() + at + key.size() - 2,
                        "0x%llx holds %zu bytes in %zu blocks", &site, &bytes,
                        &blocks) == 3) {
            out.push_back({static_cast<std::uintptr_t>(site), bytes, blocks});
        }
    }
    return out;
}

void plain_abort() { ::signal(SIGABRT, SIG_DFL); }

void expect_abort(const char* test_case, const char* text) {
    auto result = fresh_process::run(test_case);
    REQUIRE(result.announced);
    CHECK(WIFSIGNALED(result.status));
    CHECK(WTERMSIG(result.status) == SIGABRT);
    CHECK(result.output.find(text) != std::string::npos);
}

}  // namespace

TEST_CASE("the test runs with the debug wrapper") { CHECK(debug_enabled()); }

TEST_CASE("the allocation count rises by one per allocation") {
    constexpr std::uint64_t N = 25;
    void* blocks[N];
    const auto before = debug_allocation_count();
    for (auto& b : blocks) b = allocate(48, 16);
    CHECK(debug_allocation_count() - before == N);
    for (auto* b : blocks) deallocate(b);
    CHECK(debug_allocation_count() - before == N);
}

TEST_CASE("a planted leak in a scratch pool reports its path and sites") {
    CaptureReports capture_reports;
    PoolHandle pool = Manager::get().root().child("debug leak");
    void* a1;
    void* a2;
    void* b;
    {
        Scope scope{*pool};
        a1 = plant_a(100);
        a2 = plant_a(100);
        b = plant_b(40);
    }
    pool.close();
    CHECK(g_reports.find("pool root/debug leak closed with ") !=
          std::string::npos);
    CHECK(g_reports.find(" in 3 blocks\n") != std::string::npos);
    auto sites = sites_of(g_reports, "root/debug leak");
    REQUIRE(sites.size() == 2);
    for (const ReportedSite& s : sites) {
        if (inside(s.site, plant_a)) {
            CHECK(s.bytes == 200);
            CHECK(s.blocks == 2);
        } else {
            CHECK(inside(s.site, plant_b));
            CHECK(s.bytes == 40);
            CHECK(s.blocks == 1);
        }
    }
    deallocate(a1, 100);
    deallocate(a2, 100);
    deallocate(b, 40);
}

TEST_CASE("blocks freed in time leave no report") {
    CaptureReports capture_reports;
    PoolHandle pool = Manager::get().root().child("debug clean");
    {
        Scope scope{*pool};
        for (int i = 0; i < 200; ++i) {
            void* p = plant_a(24);
            deallocate(p);
        }
    }
    pool.close();
    CHECK(g_reports.empty());
}

TEST_CASE("a freed block holds the fill pattern") {
    auto* p = static_cast<unsigned char*>(allocate(64, 16));
    std::memset(p, 7, 64);
    deallocate(p, 64);
    for (std::size_t i = 0; i < 64; ++i) {
        REQUIRE(p[i] == DEBUG_FREED_FILL);
    }
    debug_drain();
}

TEST_CASE("a retained block stays out of reuse until its pool closes") {
    CaptureReports capture_reports;
    PoolHandle pool = Manager::get().root().child("debug retained");
    {
        Scope scope{*pool};
        auto* p = static_cast<unsigned char*>(allocate(64, 16));
        std::memset(p, 7, 64);
        deallocate_retained(p, 64);
        CHECK(pool->blocks() == 1);
        for (int i = 0; i < 4 * static_cast<int>(DEBUG_QUARANTINE_BLOCKS);
             ++i) {
            void* q = allocate(64, 16);
            REQUIRE(q != p);
            deallocate(q, 64);
        }
        for (std::size_t i = 0; i < 64; ++i) {
            REQUIRE(p[i] == DEBUG_FREED_FILL);
        }
    }
    pool.close();
    CHECK(g_reports.empty());
}

TEST_CASE("blocks keep their alignment and the redzone does not move them") {
    for (std::size_t align = 1; align <= 4096; align *= 2) {
        void* p = allocate(48, align);
        CHECK(reinterpret_cast<std::uintptr_t>(p) % align == 0);
        deallocate(p, 48);
    }
    void* zero = allocate(0, 16);
    void* other = allocate(0, 16);
    CHECK(zero != other);
    deallocate(zero);
    deallocate(other);
    debug_drain();
}

TEST_CASE("many threads allocate and free across threads") {
    PoolHandle pool = Manager::get().root().child("debug threads");
    std::size_t before = Manager::get().root().live();
    constexpr int THREADS = 8;
    constexpr int ROUNDS = 5000;
    std::vector<std::atomic<void*>> mailbox(THREADS);
    for (auto& m : mailbox) m.store(nullptr);
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t] {
            Scope scope{*pool};
            std::mt19937 rng(static_cast<unsigned>(t));
            for (int i = 0; i < ROUNDS; ++i) {
                std::size_t size = rng() % 3000;
                void* p = allocate(size, 16);
                if (size > 0) std::memset(p, t, size);
                void* handed =
                    mailbox[static_cast<std::size_t>(rng() % THREADS)].exchange(
                        p, std::memory_order_acq_rel);
                if (handed != nullptr) deallocate(handed);
            }
        });
    }
    for (auto& th : threads) th.join();
    for (auto& m : mailbox) {
        void* p = m.exchange(nullptr);
        if (p != nullptr) deallocate(p);
    }
    debug_drain();
    CHECK(pool->live() == 0);
    CHECK(Manager::get().root().live() == before);
}

TEST_CASE("a redzone overrun is caught" * doctest::skip()) {
    fresh_process::announce();
    plain_abort();
    auto* p = static_cast<char*>(allocate(32, 16));
    p[32] = 1;
    deallocate(p);
}

TEST_CASE("a double free is caught" * doctest::skip()) {
    fresh_process::announce();
    plain_abort();
    void* p = allocate(32, 16);
    deallocate(p);
    deallocate(p);
}

TEST_CASE("a write to a freed block is caught" * doctest::skip()) {
    fresh_process::announce();
    plain_abort();
    auto* p = static_cast<char*>(allocate(64, 16));
    deallocate(p);
    p[8] = 1;
    for (std::size_t i = 0; i <= DEBUG_QUARANTINE_BLOCKS; ++i) {
        deallocate(allocate(64, 16));
    }
}

TEST_CASE("reallocate moves the block and keeps its pool and contents") {
    PoolHandle owner = Manager::get().root().child("owner");
    PoolHandle other = Manager::get().root().child("other");
    void* p;
    {
        Scope s{*owner};
        p = allocate(100, 16);
    }
    std::memset(p, 7, 100);
    void* q;
    {
        Scope s{*other};
        q = reallocate(p, 100, 5000, 16);
    }
    CHECK(q != p);
    bool intact = true;
    for (std::size_t i = 0; i < 100; ++i) {
        intact &= static_cast<unsigned char*>(q)[i] == 7;
    }
    CHECK(intact);
    std::size_t blocks = 0;
    debug_for_each_site(
        owner->id(),
        [](void* context, const DebugSite& group) noexcept {
            *static_cast<std::size_t*>(context) += group.blocks;
        },
        &blocks);
    CHECK(blocks == 1);
    deallocate(q, 5000);
    debug_drain();
    CHECK(owner->live() == 0);
    CHECK(other->live() == 0);
}

TEST_CASE("the pool overload charges its pool under the debug wrapper") {
    PoolHandle named = Manager::get().root().child("named");
    PoolHandle scope = Manager::get().root().child("scope");
    Scope s{*scope};
    void* p = allocate(100, 16, *named);
    CHECK(named->blocks() == 1);
    CHECK(scope->blocks() == 0);
    deallocate(p, 100);
    debug_drain();
    CHECK(named->live() == 0);
}

TEST_CASE("a resize with the wrong old size is caught" * doctest::skip()) {
    fresh_process::announce();
    plain_abort();
    void* p = allocate(64, 16);
    p = reallocate(p, 32, 128, 16);
    deallocate(p, 128);
}

TEST_CASE("a sized free of another size is caught" * doctest::skip()) {
    fresh_process::announce();
    plain_abort();
    deallocate(allocate(64, 16), 32);
}

TEST_CASE("a pointer that was never allocated is caught" * doctest::skip()) {
    fresh_process::announce();
    plain_abort();
    char local[64];
    deallocate(local);
}

TEST_CASE("the debug checks abort in a forked child") {
    expect_abort("a redzone overrun is caught", "redzone behind the block");
    expect_abort("a double free is caught", "double free");
    expect_abort("a write to a freed block is caught",
                 "a freed block was written");
    expect_abort("a sized free of another size is caught",
                 "sized free does not match");
    expect_abort("a pointer that was never allocated is caught",
                 "did not allocate");
    expect_abort("a resize with the wrong old size is caught",
                 "sized free does not match");
}

TEST_CASE("planted leak at exit" * doctest::skip()) {
    fresh_process::announce();
    Pool& root = Manager::get().root();
    PoolHandle pool = root.child("exit leak");
    Scope scope{*pool};
    std::fprintf(stderr, "planter 0x%llx\n",
                 static_cast<unsigned long long>(
                     reinterpret_cast<std::uintptr_t>(&plant_a)));
    plant_a(123);
}

TEST_CASE("clean process at exit" * doctest::skip()) {
    fresh_process::announce();
    deallocate(plant_a(123));
}

TEST_CASE("exported bytes at exit" * doctest::skip()) {
    fresh_process::announce();
    void* p = plant_a(123);
    Pool& root = Manager::get().root();
    root.add_exported(root.live());
    (void)p;
}

TEST_CASE("check turned off at exit" * doctest::skip()) {
    fresh_process::announce();
    plant_a(123);
    set_exit_check(false);
}

// Run only by the ctest memory_exit_check_leak, which turns the exit check on.
TEST_CASE("exit check fails a planted leak" * doctest::skip()) {
    auto result = fresh_process::run("planted leak at exit");
    REQUIRE(result.announced);
    CHECK(WIFEXITED(result.status));
    CHECK(WEXITSTATUS(result.status) == 70);
    CHECK(result.output.find("exit check failed") != std::string::npos);
    CHECK(result.output.find("pool root/exit leak holds") != std::string::npos);
    auto sites =
        sites_of(result.output.substr(result.output.find("exit check failed")),
                 "root/exit leak");
    REQUIRE(sites.size() == 1);
    unsigned long long planter = 0;
    std::size_t at = result.output.find("planter 0x");
    REQUIRE(at != std::string::npos);
    REQUIRE(std::sscanf(result.output.c_str() + at, "planter 0x%llx",
                        &planter) == 1);
    CHECK(sites[0].site > planter);
    CHECK(sites[0].site - planter < 256);
    CHECK(sites[0].bytes == 123);
    CHECK(sites[0].blocks == 1);
}

TEST_CASE("exit check passes a clean process" * doctest::skip()) {
    auto result = fresh_process::run("clean process at exit");
    CHECK(result.ok());
    CHECK(result.output.empty());
}

TEST_CASE("exit check subtracts exported bytes" * doctest::skip()) {
    auto result = fresh_process::run("exported bytes at exit");
    CHECK(result.ok());
}

TEST_CASE("exit check can be turned off" * doctest::skip()) {
    auto result = fresh_process::run("check turned off at exit");
    CHECK(result.ok());
}
