#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/constants.h>
#include <dftracer/utils/memory/heap_page.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#include "fresh_process.h"

using namespace dftracer::utils::memory;

namespace {

constexpr std::size_t KIB = 1024;
constexpr std::size_t MIB = 1024 * KIB;

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || \
    __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr bool SANITIZED = true;
#else
constexpr bool SANITIZED = false;
#endif

// The delay is read once, before the first allocation of the process.
const int DECAY_SET = ::setenv(constants::DECAY_MS_ENV, "300", 0);

std::size_t drop(std::size_t before, std::size_t after) {
    return before > after ? before - after : 0;
}

std::size_t live() { return page_stats().live; }
std::size_t reserved() { return page_stats().reserved; }

std::size_t resident_bytes() {
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::size_t>(info.phys_footprint);
#else
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    unsigned long total = 0;
    unsigned long resident = 0;
    int fields = std::fscanf(f, "%lu %lu", &total, &resident);
    std::fclose(f);
    if (fields != 2) return 0;
    return resident * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
#endif
}

void touch_pages(void* p, std::size_t size, unsigned char tag) {
    auto* c = static_cast<unsigned char*>(p);
    for (std::size_t i = 0; i < size; i += 4096) c[i] = tag;
    c[size - 1] = tag;
}

bool pages_intact(const void* p, std::size_t size, unsigned char tag) {
    auto* c = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < size; i += 4096) {
        if (c[i] != tag) return false;
    }
    return c[size - 1] == tag;
}

constexpr std::size_t LARGE_BLOCK = 8 * MIB;
constexpr std::size_t MEDIUM_BLOCK = MEDIUM_MAX;
constexpr std::size_t SMALL_BLOCK = 1000;

struct WorkingSet {
    std::vector<void*> large;
    std::vector<void*> medium;
    std::vector<void*> small;
    std::size_t bytes = 0;
};

WorkingSet build_set() {
    std::size_t div = SANITIZED ? 4 : 1;
    WorkingSet set;
    for (std::size_t i = 0; i < 8 / div; ++i) {
        set.large.push_back(page_allocate(LARGE_BLOCK, 16));
        touch_pages(set.large.back(), LARGE_BLOCK, 0x11);
    }
    for (std::size_t i = 0; i < 32 / div; ++i) {
        set.medium.push_back(page_allocate(MEDIUM_BLOCK, 16));
        touch_pages(set.medium.back(), MEDIUM_BLOCK, 0x22);
    }
    for (std::size_t i = 0; i < 32768 / div; ++i) {
        set.small.push_back(page_allocate(SMALL_BLOCK, 16));
        *static_cast<unsigned char*>(set.small.back()) = 0x33;
    }
    set.bytes = set.large.size() * LARGE_BLOCK +
                set.medium.size() * MEDIUM_BLOCK + set.small.size() * 1024;
    return set;
}

bool set_intact(const WorkingSet& set) {
    for (void* p : set.large) {
        if (!pages_intact(p, LARGE_BLOCK, 0x11)) return false;
    }
    for (void* p : set.medium) {
        if (!pages_intact(p, MEDIUM_BLOCK, 0x22)) return false;
    }
    for (void* p : set.small) {
        if (*static_cast<unsigned char*>(p) != 0x33) return false;
    }
    return true;
}

void free_set(WorkingSet& set) {
    for (void* p : set.large) page_deallocate(p, LARGE_BLOCK);
    for (void* p : set.medium) page_deallocate(p, MEDIUM_BLOCK);
    for (void* p : set.small) page_deallocate(p, SMALL_BLOCK);
    set.large.clear();
    set.medium.clear();
    set.small.clear();
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

TEST_CASE("reserved counts cached memory until it is purged") {
    CHECK(fresh_process::run("cached run scenario").ok());
}

TEST_CASE("cached run scenario" * doctest::skip()) {
    fresh_process::announce();
    constexpr std::size_t RUN = 32 * MIB;
    page_deallocate(page_allocate(1, 16), 1);
    page_trim(Mode::All);
    std::size_t base = reserved();

    void* p = page_allocate(RUN, 16);
    CHECK(reserved() - base == RUN + HEAP_PAGE_SIZE);
    page_deallocate(p, RUN);
    CHECK(reserved() - base == RUN + HEAP_PAGE_SIZE);
    page_trim(Mode::All);
    CHECK(reserved() - base == HEAP_PAGE_SIZE);

    p = page_allocate(RUN, 16);
    CHECK(reserved() - base == RUN + HEAP_PAGE_SIZE);
    touch_pages(p, RUN, 0x44);
    CHECK(pages_intact(p, RUN, 0x44));
    page_deallocate(p, RUN);
    page_trim(Mode::All);
    CHECK(reserved() - base == HEAP_PAGE_SIZE);
}

TEST_CASE("spans are cached, purged and reused with exact counters") {
    CHECK(fresh_process::run("cached span scenario").ok());
}

TEST_CASE("cached span scenario" * doctest::skip()) {
    fresh_process::announce();
    std::size_t base = reserved();
    std::vector<std::size_t> seen;
    std::thread worker([&seen] {
        auto run = [&seen] {
            std::vector<void*> runs;
            for (int i = 0; i < 4; ++i) runs.push_back(page_allocate(MIB, 16));
            seen.push_back(reserved());
            for (int i : {3, 0, 1, 2}) page_deallocate(runs[i], MIB);
            seen.push_back(reserved());
            page_trim(Mode::All);
            seen.push_back(reserved());
        };
        run();
        run();
    });
    worker.join();

    REQUIRE(seen.size() == 6);
    const std::size_t heap = HEAP_PAGE_SIZE;
    const std::size_t span = HEAP_SPAN_SIZE;
    CHECK(seen[0] - base == heap + 2 * span);
    CHECK(seen[1] - base == heap + 2 * span);
    CHECK(seen[2] - base == 3 * HEAP_PAGE_SIZE);
    CHECK(seen[3] - base == (1 + 49 + 17) * HEAP_PAGE_SIZE);
    CHECK(seen[4] - base == (1 + 49 + 17) * HEAP_PAGE_SIZE);
    CHECK(seen[5] - base == 3 * HEAP_PAGE_SIZE);
}

TEST_CASE("trim lowers the resident bytes after a large working set is freed") {
    std::size_t base_live = live();
    WorkingSet set = build_set();
    std::size_t peak = resident_bytes();
    std::size_t reserved_peak = reserved();
    CHECK(set_intact(set));
    free_set(set);
    CHECK(live() == base_live);

    std::size_t freed = resident_bytes();
    if (!SANITIZED) CHECK(freed + set.bytes / 4 >= peak);

    page_trim(Mode::All);
    std::size_t trimmed = resident_bytes();
    CHECK(drop(reserved_peak, reserved()) >= set.bytes * 9 / 10);
    if (!SANITIZED) CHECK(drop(freed, trimmed) >= set.bytes * 7 / 10);
}

TEST_CASE("a purged span and run are reusable") {
    std::size_t base_live = live();
    page_trim(Mode::All);
    std::size_t trimmed = reserved();

    WorkingSet set = build_set();
    CHECK(set_intact(set));
    CHECK(reserved() - trimmed >= set.bytes * 9 / 10);
    free_set(set);
    CHECK(live() == base_live);

    page_trim(Mode::All);
    WorkingSet again = build_set();
    CHECK(set_intact(again));
    free_set(again);
    page_trim(Mode::All);
    CHECK(live() == base_live);
}

TEST_CASE("decay purges after the delay with no trim call") {
    page_trim(Mode::All);
    WorkingSet set = build_set();
    free_set(set);
    std::size_t reserved_free = reserved();
    std::size_t resident_free = resident_bytes();
    sleep_ms(50);
    CHECK(reserved() == reserved_free);

    sleep_ms(700);
    std::size_t goal = set.bytes * 8 / 10;
    for (int i = 0; i < 400 && drop(reserved_free, reserved()) < goal; ++i) {
        void* p = page_allocate(MEDIUM_MAX + 1, 16);
        page_deallocate(p, MEDIUM_MAX + 1);
    }
    CHECK(drop(reserved_free, reserved()) >= goal);
    if (!SANITIZED) {
        CHECK(drop(resident_free, resident_bytes()) >= set.bytes * 6 / 10);
    }
}

TEST_CASE("trim of decayed memory keeps what is not yet idle long enough") {
    page_trim(Mode::All);
    WorkingSet set = build_set();
    free_set(set);
    std::size_t reserved_free = reserved();
    page_trim(Mode::Decayed);
    CHECK(reserved() >= reserved_free - HEAP_PAGE_SIZE * 64);

    sleep_ms(700);
    page_trim(Mode::Decayed);
    CHECK(drop(reserved_free, reserved()) >= set.bytes * 9 / 10);
}

namespace {

struct Block {
    void* p;
    std::size_t size;
    unsigned char tag;
};

class Mailbox {
   public:
    void push(const Block& b) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(b);
    }
    bool pop(Block& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop_front();
        return true;
    }

   private:
    std::mutex mutex_;
    std::deque<Block> queue_;
};

std::size_t random_size(std::mt19937_64& rng) {
    std::size_t roll = rng() % 100;
    if (roll < 70) return 1 + rng() % SMALL_MAX;
    if (roll < 95) return SMALL_MAX + 1 + rng() % (MEDIUM_MAX - SMALL_MAX);
    return MEDIUM_MAX + 1 + rng() % (7 * MIB);
}

}  // namespace

TEST_CASE("trim runs while other threads allocate and free across tiers") {
    constexpr int WORKERS = 4;
    constexpr int ROUNDS = 400;
    std::size_t base_live = live();
    std::size_t base_reserved = reserved();
    Mailbox box;
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};

    std::thread trimmer([&stop] {
        for (int i = 0; !stop.load(std::memory_order_relaxed); ++i) {
            page_trim(i % 4 == 0 ? Mode::Decayed : Mode::All);
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> workers;
    for (int id = 0; id < WORKERS; ++id) {
        workers.emplace_back([&, id] {
            std::mt19937_64 rng(static_cast<std::uint64_t>(id) + 5);
            for (int round = 0; round < ROUNDS; ++round) {
                std::size_t size = random_size(rng);
                auto tag = static_cast<unsigned char>(rng());
                void* p = page_allocate(size, 16);
                touch_pages(p, size, tag);
                box.push({p, size, tag});
                Block b;
                if (box.pop(b)) {
                    if (!pages_intact(b.p, b.size, b.tag)) ++bad;
                    page_deallocate(b.p, b.size);
                }
            }
        });
    }
    for (auto& t : workers) t.join();
    stop.store(true, std::memory_order_relaxed);
    trimmer.join();

    Block b;
    while (box.pop(b)) {
        CHECK(pages_intact(b.p, b.size, b.tag));
        page_deallocate(b.p, b.size);
    }
    page_flush_remote();
    CHECK(bad.load() == 0);
    CHECK(live() == base_live);
    page_trim(Mode::All);
    CHECK(reserved() <= base_reserved + 32 * MIB);
}

// Run only by the ctest memory_bad_decay_value, which sets a bad delay before
// the first allocation of the process.
TEST_CASE("bad decay value" * doctest::skip()) {
    pid_t pid = ::fork();
    if (pid == 0) {
        int null = ::open("/dev/null", O_WRONLY);
        ::dup2(null, STDERR_FILENO);
        ::signal(SIGABRT, SIG_DFL);
        config();
        ::_exit(0);
    }
    REQUIRE(pid > 0);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);
}
