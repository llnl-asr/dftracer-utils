// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/timer.h>
#include <dftracer/utils/coro/hooks.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <mutex>

namespace dftracer::utils::coro {

namespace detail {
constinit std::atomic<const CoroutineHooks*> g_coroutine_hooks{nullptr};
}  // namespace detail

void set_coroutine_hooks(const CoroutineHooks* hooks) noexcept {
    detail::g_coroutine_hooks.store(hooks, std::memory_order_release);
}

namespace {

constinit thread_local int t_worker_index = -1;

struct Chunk {
    dstd::Vector<CoroutineEvent> events;
    std::size_t count = 0;
};

// The owning worker fills chunks[published % size]. Chunks in
// [consumed, published) belong to the reader.
struct WorkerLog {
    dstd::Vector<Chunk> chunks;
    std::atomic<std::uint64_t> published{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint64_t> dropped{0};
};

struct ThreadBinding {
    const void* log = nullptr;
    std::uint64_t run = 0;
    WorkerLog* worker = nullptr;
};

constinit thread_local ThreadBinding t_binding{};
constinit std::atomic<std::uint64_t> g_next_run{1};

}  // namespace

int current_worker_index() noexcept { return t_worker_index; }

void set_current_worker_index(int index) noexcept { t_worker_index = index; }

struct CoroutineEventLog::Impl {
    CoroutineEventLogOptions options;
    CoroutineHooks hooks;
    std::uint64_t run = 0;

    std::mutex mutex;  // workers, and the writes of threads that are no worker
    dstd::Vector<dstd::UniquePtr<WorkerLog>> workers;
    dstd::Vector<CoroutineEvent> foreign;
    std::atomic<std::uint64_t> foreign_dropped{0};

    std::mutex drain_mutex;

    WorkerLog& bind() {
        auto owned = dstd::make_unique<WorkerLog>();
        owned->chunks.resize(options.chunks_per_worker);
        for (Chunk& chunk : owned->chunks) {
            chunk.events.resize(options.chunk_events);
        }
        WorkerLog* worker = owned.get();
        std::lock_guard<std::mutex> lock(mutex);
        workers.push_back(std::move(owned));
        t_binding = {this, run, worker};
        return *worker;
    }

    void write(const CoroutineEvent& event) {
        if (event.worker < 0) {
            std::lock_guard<std::mutex> lock(mutex);
            if (foreign.size() < options.chunk_events) {
                foreign.push_back(event);
            } else {
                foreign_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        WorkerLog& log = t_binding.log == this && t_binding.run == run
                             ? *t_binding.worker
                             : bind();
        const std::uint64_t published =
            log.published.load(std::memory_order_relaxed);
        const std::size_t slots = log.chunks.size();
        if (published - log.consumed.load(std::memory_order_acquire) >= slots) {
            log.dropped.store(log.dropped.load(std::memory_order_relaxed) + 1,
                              std::memory_order_relaxed);
            return;
        }
        Chunk& chunk = log.chunks[published % slots];
        chunk.events[chunk.count++] = event;
        if (chunk.count == chunk.events.size()) {
            log.published.store(published + 1, std::memory_order_release);
        }
    }

    // A partial chunk goes to the reader, so an idle worker holds back nothing.
    void flush() {
        if (t_binding.log != this || t_binding.run != run) return;
        WorkerLog& log = *t_binding.worker;
        const std::uint64_t published =
            log.published.load(std::memory_order_relaxed);
        if (published - log.consumed.load(std::memory_order_acquire) >=
            log.chunks.size()) {
            return;
        }
        if (log.chunks[published % log.chunks.size()].count != 0) {
            log.published.store(published + 1, std::memory_order_release);
        }
    }
};

namespace {

CoroutineEvent make_event(const void* coroutine, CoroutineEventKind kind,
                          CoroutineOrigin origin) noexcept {
    return CoroutineEvent{common::steady_now_ns(), coroutine, t_worker_index,
                          kind, origin};
}

}  // namespace

CoroutineEventLog::CoroutineEventLog(CoroutineEventLogOptions options)
    : impl_(dstd::make_unique<Impl>()) {
    impl_->options = options;
    impl_->options.chunk_events =
        std::max<std::size_t>(options.chunk_events, 1);
    impl_->options.chunks_per_worker =
        std::max<std::size_t>(options.chunks_per_worker, 1);
    CoroutineHooks& hooks = impl_->hooks;
    hooks.user = impl_.get();
    hooks.enqueue = [](void* user, const void* coroutine,
                       CoroutineOrigin origin) {
        static_cast<Impl*>(user)->write(
            make_event(coroutine, CoroutineEventKind::Enqueue, origin));
    };
    hooks.resume = [](void* user, const void* coroutine) {
        static_cast<Impl*>(user)->write(make_event(
            coroutine, CoroutineEventKind::Resume, CoroutineOrigin::Spawn));
    };
    hooks.suspend = [](void* user, const void* coroutine) {
        static_cast<Impl*>(user)->write(make_event(
            coroutine, CoroutineEventKind::Suspend, CoroutineOrigin::Spawn));
    };
    hooks.finish = [](void* user, const void* coroutine) {
        static_cast<Impl*>(user)->write(make_event(
            coroutine, CoroutineEventKind::Finish, CoroutineOrigin::Spawn));
    };
    hooks.await_begin = [](void* user, const void* coroutine) {
        static_cast<Impl*>(user)->write(make_event(
            coroutine, CoroutineEventKind::AwaitBegin, CoroutineOrigin::Await));
    };
    hooks.await_end = [](void* user, const void* coroutine) {
        static_cast<Impl*>(user)->write(make_event(
            coroutine, CoroutineEventKind::AwaitEnd, CoroutineOrigin::Await));
    };
    hooks.idle = [](void* user) { static_cast<Impl*>(user)->flush(); };
}

CoroutineEventLog::~CoroutineEventLog() { stop(); }

void CoroutineEventLog::start() noexcept {
    impl_->run = g_next_run.fetch_add(1, std::memory_order_relaxed);
    set_coroutine_hooks(&impl_->hooks);
}

void CoroutineEventLog::stop() noexcept {
    if (coroutine_hooks() == &impl_->hooks) set_coroutine_hooks(nullptr);
}

std::size_t CoroutineEventLog::drain(
    dstd::FunctionRef<void(const CoroutineEvent&)> fn) {
    std::lock_guard<std::mutex> drain_lock(impl_->drain_mutex);
    std::size_t total = 0;
    dstd::Vector<WorkerLog*> workers;
    dstd::Vector<CoroutineEvent> foreign;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        workers.reserve(impl_->workers.size());
        for (const auto& worker : impl_->workers) {
            workers.push_back(worker.get());
        }
        foreign.swap(impl_->foreign);
    }
    for (const CoroutineEvent& event : foreign) {
        fn(event);
        ++total;
    }
    for (WorkerLog* log : workers) {
        const std::uint64_t published =
            log->published.load(std::memory_order_acquire);
        std::uint64_t consumed = log->consumed.load(std::memory_order_relaxed);
        for (; consumed < published; ++consumed) {
            Chunk& chunk = log->chunks[consumed % log->chunks.size()];
            for (std::size_t i = 0; i < chunk.count; ++i) fn(chunk.events[i]);
            total += chunk.count;
            chunk.count = 0;
            log->consumed.store(consumed + 1, std::memory_order_release);
        }
    }
    return total;
}

std::uint64_t CoroutineEventLog::dropped() const noexcept {
    std::uint64_t total =
        impl_->foreign_dropped.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& worker : impl_->workers) {
        total += worker->dropped.load(std::memory_order_relaxed);
    }
    return total;
}

}  // namespace dftracer::utils::coro
