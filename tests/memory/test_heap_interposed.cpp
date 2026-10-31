#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dlfcn.h>
#include <doctest/doctest.h>
#include <sys/mman.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace {

std::atomic<unsigned long> WRAPPED{0};

}  // namespace

// What a tracer such as DFTracer puts in front of munmap: its wrapper records
// the call, and the record allocates through the global operator new, which
// this executable takes from the heap.
extern "C" int munmap(void* addr, std::size_t len) {
    using Real = int (*)(void*, std::size_t);
    static const Real real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "munmap"));
    WRAPPED.fetch_add(1, std::memory_order_relaxed);
    auto record = std::make_unique<std::vector<char>>(64);
    asm volatile("" : : "r"(record.get()) : "memory");
    return real(addr, len);
}

TEST_SUITE("HeapInterposed") {
    TEST_CASE(
        "a new thread's first allocation does not reach a wrapped munmap") {
        unsigned long during = 0;
        std::thread worker([&during] {
            const unsigned long before = WRAPPED.load();
            for (int i = 0; i < 64; ++i) {
                auto* block = new char[std::size_t{1} << 20];
                asm volatile("" : : "r"(block) : "memory");
                delete[] block;
            }
            during = WRAPPED.load() - before;
        });
        worker.join();
        CHECK(during == 0);
    }
}
