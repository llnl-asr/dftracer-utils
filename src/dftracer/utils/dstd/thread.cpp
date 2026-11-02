// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/memory/heap.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace dftracer::utils::dstd {

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

void Thread::set_current_name(const String& name) noexcept {
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
    void* raw = memory::try_allocate(sizeof(Trampoline), alignof(Trampoline));
    if (raw == nullptr) {
        (void)pthread_attr_destroy(&attr);
        return ENOMEM;
    }
    auto* t = ::new (raw) Trampoline{run, arg};
    rc = pthread_create(&handle_, &attr, &trampoline, t);
    (void)pthread_attr_destroy(&attr);
    if (rc != 0) {
        t->~Trampoline();
        memory::deallocate(t, sizeof(Trampoline));
        return rc;
    }
    joinable_ = true;
    return 0;
}

Error Thread::create_error(int rc) {
    String message("cannot create a thread: ");
    message.append(std::strerror(rc));
    return Error(Condition::Internal, message);
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
