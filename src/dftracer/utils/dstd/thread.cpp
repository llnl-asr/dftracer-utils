// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/thread_probe.h>
#include <dftracer/utils/memory/detail/heap.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

namespace dftracer::utils::dstd {

unsigned Thread::hardware_concurrency() noexcept {
    return std::thread::hardware_concurrency();
}

#if defined(DFTRACER_UTILS_TEST_PROBES) && DFTRACER_UTILS_TEST_PROBES
namespace {
std::atomic<int> g_failing_creates{0};

bool take_create_failure() noexcept {
    int n = g_failing_creates.load(std::memory_order_relaxed);
    while (n > 0 && !g_failing_creates.compare_exchange_weak(
                        n, n - 1, std::memory_order_relaxed)) {
    }
    return n > 0;
}
}  // namespace

void detail::fail_thread_creates(int count) noexcept {
    g_failing_creates.store(count, std::memory_order_relaxed);
}
#endif

namespace {

struct Trampoline {
    void (*run)(void*);
    void* arg;
};

void* trampoline(void* raw) {
    UniquePtr<Trampoline> t(static_cast<Trampoline*>(raw));
    t->run(t->arg);
    return nullptr;
}

}  // namespace

bool Thread::Id::operator==(const Id& other) const noexcept {
    if (set_ != other.set_) return false;
    return !set_ || pthread_equal(handle_, other.handle_) != 0;
}

std::size_t Thread::Id::hash() const noexcept {
    if (!set_) return 0;
    // pthread_t is an integer on Linux and a pointer on macOS, and on both a
    // handle equals another under pthread_equal only when the bytes match.
    static_assert(sizeof(pthread_t) <= sizeof(std::uint64_t));
    std::uint64_t bits = 0;
    std::memcpy(&bits, &handle_, sizeof(pthread_t));
    return static_cast<std::size_t>(hash::splitmix64(bits));
}

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this != &other) {
        join();
        handle_ = other.handle_;
        joinable_ = other.joinable_;
        other.joinable_ = false;
    }
    return *this;
}

void Thread::join() noexcept {
    if (!joinable_) return;
    joinable_ = false;
    (void)pthread_join(handle_, nullptr);
}

void Thread::detach() noexcept {
    if (!joinable_) return;
    joinable_ = false;
    (void)pthread_detach(handle_);
}

void Thread::set_current_name(StringView name) noexcept {
    char cut[16];
    const std::size_t n = name.size() < 15 ? name.size() : 15;
    std::memcpy(cut, name.data(), n);
    cut[n] = '\0';
#if defined(__APPLE__)
    (void)pthread_setname_np(cut);
#else
    (void)pthread_setname_np(pthread_self(), cut);
#endif
}

int Thread::create(void (*run)(void*), void* arg,
                   std::size_t stack_size) noexcept {
#if defined(DFTRACER_UTILS_TEST_PROBES) && DFTRACER_UTILS_TEST_PROBES
    if (take_create_failure()) return EAGAIN;
#endif
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) return rc;
    if (stack_size != 0) {
        const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        std::size_t size = std::max<std::size_t>(stack_size, PTHREAD_STACK_MIN);
        size = (size + page - 1) / page * page;
        rc = pthread_attr_setstacksize(&attr, size);
        if (rc != 0) {
            (void)pthread_attr_destroy(&attr);
            return rc;
        }
    }
    void* raw =
        memory::detail::try_allocate(sizeof(Trampoline), alignof(Trampoline));
    if (raw == nullptr) {
        (void)pthread_attr_destroy(&attr);
        return ENOMEM;
    }
    auto* t = ::new (raw) Trampoline{run, arg};
    rc = pthread_create(&handle_, &attr, &trampoline, t);
    (void)pthread_attr_destroy(&attr);
    if (rc != 0) {
        t->~Trampoline();
        memory::detail::deallocate(t, sizeof(Trampoline));
        return rc;
    }
    joinable_ = true;
    return 0;
}

Error Thread::make_error(int rc) {
    return Error::from_format<"cannot create a thread: {}">(Condition::Internal,
                                                            std::strerror(rc));
}

void Thread::launch(void (*run)(void*), void* arg, std::size_t stack_size) {
    const int rc = create(run, arg, stack_size);
    if (rc != 0) {
        std::fprintf(stderr, "dftracer-utils: cannot create a thread: %s\n",
                     std::strerror(rc));
        std::abort();
    }
}

}  // namespace dftracer::utils::dstd
