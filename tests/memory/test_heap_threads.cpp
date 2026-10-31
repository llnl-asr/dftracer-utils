#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/heap_page.h>
#include <doctest/doctest.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

using namespace dftracer::utils::memory;

namespace {

constexpr std::size_t KIB = 1024;
constexpr std::size_t MIB = 1024 * KIB;
constexpr std::size_t SAMPLE = 4 * KIB;

struct Block {
    void* p;
    std::size_t size;
    unsigned char tag;
};

void fill(const Block& b) {
    auto* c = static_cast<unsigned char*>(b.p);
    if (b.size <= MIB) {
        std::memset(c, b.tag, b.size);
    } else {
        std::memset(c, b.tag, SAMPLE);
        std::memset(c + b.size - SAMPLE, b.tag, SAMPLE);
    }
}

bool intact(const Block& b) {
    auto* c = static_cast<const unsigned char*>(b.p);
    std::size_t head = b.size <= MIB ? b.size : SAMPLE;
    for (std::size_t i = 0; i < head; ++i) {
        if (c[i] != b.tag) return false;
    }
    if (b.size > MIB) {
        for (std::size_t i = b.size - SAMPLE; i < b.size; ++i) {
            if (c[i] != b.tag) return false;
        }
    }
    return true;
}

std::size_t random_size(std::mt19937_64& rng) {
    std::size_t roll = rng() % 1000;
    if (roll < 700) return 1 + rng() % SMALL_MAX;
    if (roll < 950) return SMALL_MAX + 1 + rng() % (MEDIUM_MAX - SMALL_MAX);
    if (roll < 998) return MEDIUM_MAX + 1 + rng() % (3 * MIB);
    return LARGE_MAX + 1 + rng() % (8 * MIB);
}

Block make_block(std::mt19937_64& rng) {
    std::size_t size = random_size(rng);
    Block b{page_allocate(size, 16), size, static_cast<unsigned char>(rng())};
    fill(b);
    return b;
}

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

class Gate {
   public:
    void open() {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

std::size_t live() { return page_stats().live; }

}  // namespace

TEST_CASE("the owner takes remote frees back on its slow path") {
    constexpr std::size_t SIZE = 4096;
    constexpr int COUNT = 64;
    std::vector<void*> first;
    std::set<void*> second;
    Gate allocated;
    Gate freed;
    std::thread owner([&] {
        for (int i = 0; i < COUNT; ++i)
            first.push_back(page_allocate(SIZE, 16));
        allocated.open();
        freed.wait();
        std::vector<void*> again;
        for (int i = 0; i < COUNT; ++i) {
            again.push_back(page_allocate(SIZE, 16));
        }
        second.insert(again.begin(), again.end());
        for (void* p : again) page_deallocate(p, SIZE);
    });
    allocated.wait();
    std::size_t base = live();
    for (void* p : first) page_deallocate(p, SIZE);
    page_flush_remote();
    CHECK(base - live() == COUNT * SIZE);
    freed.open();
    owner.join();
    CHECK(second == std::set<void*>(first.begin(), first.end()));
}

TEST_CASE("a returned heap is adopted and its pending frees are drained") {
    constexpr int COUNT = 100;
    std::size_t start = live();
    std::vector<void*> small;
    void* medium = nullptr;
    std::thread first([&] {
        for (int i = 0; i < COUNT; ++i) small.push_back(page_allocate(64, 16));
        medium = page_allocate(200 * KIB, 16);
    });
    first.join();
    std::size_t heaps = page_heap_count();
    std::size_t base = live();

    for (void* p : small) page_deallocate(p, 64);
    page_deallocate(medium, 200 * KIB);
    page_flush_remote();
    CHECK(live() < base);

    std::set<void*> reused;
    void* medium_again = nullptr;
    std::thread second([&] {
        for (int i = 0; i < COUNT; ++i) reused.insert(page_allocate(64, 16));
        medium_again = page_allocate(200 * KIB, 16);
        for (void* p : reused) page_deallocate(p, 64);
        page_deallocate(medium_again, 200 * KIB);
    });
    second.join();
    CHECK(page_heap_count() == heaps);
    CHECK(reused == std::set<void*>(small.begin(), small.end()));
    CHECK(medium_again == medium);
    CHECK(live() == start);
}

TEST_CASE("threads allocate and free each other's blocks of every tier") {
    constexpr int THREADS = 8;
    constexpr int ROUNDS = 1500;
    std::size_t base = live();
    Mailbox box;
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int id = 0; id < THREADS; ++id) {
        threads.emplace_back([&, id] {
            std::mt19937_64 rng(static_cast<std::uint64_t>(id) * 7919 + 1);
            for (int round = 0; round < ROUNDS; ++round) {
                int burst = round % 100 == 0 ? 100 : 1;
                for (int i = 0; i < burst; ++i) box.push(make_block(rng));
                for (int i = 0; i < burst; ++i) {
                    Block b;
                    if (!box.pop(b)) continue;
                    if (!intact(b)) ++bad;
                    page_deallocate(b.p, b.size);
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK(bad.load() == 0);
    Block b;
    while (box.pop(b)) {
        CHECK(intact(b));
        page_deallocate(b.p, b.size);
    }
    page_flush_remote();
    CHECK(live() == base);
}

TEST_CASE("threads exit with live blocks that others free later") {
    constexpr int THREADS = 4;
    constexpr int BLOCKS = 500;
    std::size_t base = live();
    std::vector<std::vector<Block>> held(THREADS);
    std::vector<std::thread> threads;
    for (int id = 0; id < THREADS; ++id) {
        threads.emplace_back([&held, id] {
            std::mt19937_64 rng(static_cast<std::uint64_t>(id) + 100);
            for (int i = 0; i < BLOCKS; ++i)
                held[id].push_back(make_block(rng));
        });
    }
    for (auto& t : threads) t.join();
    CHECK(live() > base);

    std::atomic<int> bad{0};
    std::vector<std::thread> freers;
    for (int id = 0; id < THREADS; ++id) {
        freers.emplace_back([&held, &bad, id] {
            for (const Block& b : held[(id + 1) % THREADS]) {
                if (!intact(b)) ++bad;
                page_deallocate(b.p, b.size);
            }
        });
    }
    for (auto& t : freers) t.join();
    CHECK(bad.load() == 0);
    CHECK(live() == base);
}

TEST_CASE("short-lived threads reuse heaps") {
    std::size_t heaps = page_heap_count();
    std::size_t base = live();
    for (int wave = 0; wave < 100; ++wave) {
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([] {
                void* p = page_allocate(1000, 16);
                page_deallocate(p, 1000);
            });
        }
        for (auto& t : threads) t.join();
    }
    CHECK(page_heap_count() <= heaps + 8);
    CHECK(live() == base);
}

namespace {

pthread_key_t g_late_key;
void* g_late_block = nullptr;
std::size_t g_late_size = 0;

void late_destructor(void*) { page_deallocate(g_late_block, g_late_size); }

}  // namespace

TEST_CASE("a free from a later thread-exit destructor still reaches its page") {
    constexpr std::size_t SIZE = 20480;
    page_deallocate(page_allocate(1, 16), 1);
    REQUIRE(pthread_key_create(&g_late_key, late_destructor) == 0);
    std::vector<void*> blocks;
    for (int i = 0; i < 3; ++i) blocks.push_back(page_allocate(SIZE, 16));
    g_late_block = blocks[0];
    g_late_size = SIZE;
    std::thread([] {
        page_deallocate(page_allocate(8, 16), 8);
        pthread_setspecific(g_late_key, &g_late_size);
    }).join();
    void* again = page_allocate(SIZE, 16);
    CHECK(again == blocks[0]);
    page_deallocate(again, SIZE);
    page_deallocate(blocks[1], SIZE);
    page_deallocate(blocks[2], SIZE);
    pthread_key_delete(g_late_key);
}

TEST_CASE("a fork child allocates and frees") {
    void* parent_block = page_allocate(300, 16);
    std::memset(parent_block, 0x42, 300);
    std::thread([] {
        void* p = page_allocate(5000, 16);
        page_deallocate(p, 5000);
    }).join();

    pid_t pid = ::fork();
    if (pid == 0) {
        int code = 0;
        page_deallocate(parent_block, 300);
        std::mt19937_64 rng(7);
        std::vector<Block> blocks;
        for (int i = 0; i < 300; ++i) blocks.push_back(make_block(rng));
        for (const Block& b : blocks) {
            if (!intact(b)) code = 1;
            page_deallocate(b.p, b.size);
        }
        std::thread worker([&code] {
            std::mt19937_64 local(9);
            std::vector<Block> mine;
            for (int i = 0; i < 100; ++i) mine.push_back(make_block(local));
            for (const Block& b : mine) {
                if (!intact(b)) code = 2;
                page_deallocate(b.p, b.size);
            }
        });
        worker.join();
        ::_exit(code);
    }
    REQUIRE(pid > 0);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(static_cast<unsigned char*>(parent_block)[299] == 0x42);
    page_deallocate(parent_block, 300);
}
