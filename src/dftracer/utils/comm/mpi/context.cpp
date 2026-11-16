// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/mpi/context.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/string.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

namespace dftracer::utils::comm::mpi {

namespace {

/// How many turns without progress the progress thread polls with no sleep
/// while a native collective is in flight.
constexpr std::size_t NATIVE_SPIN_TURNS = 5000;

std::mutex g_process_mutex;
/// Every MPI call of this library that the progress thread, a poller or a
/// caller makes runs under it. The creation of a communicator does not: it
/// waits for other processes on a thread of its own. A context that is still
/// stopping on a thread of its backend must not call MPI at the same time as
/// the next context starts, or Open MPI stalls in its collectives.
std::mutex g_mpi_mutex;
dstd::Vector<dstd::WeakPtr<Context>> g_live;
bool g_exit_hook = false;
/// Ends MPI at the end of the process when this library started it. The
/// threads of every live context stop first, so none of them calls MPI while
/// it ends. A process that starts a communicator more than once is allowed:
/// MPI starts once per process.
void finalize_at_exit() {
    Context::shutdown_all();
    int done = 0;
    MPI_Finalized(&done);
    if (!done) MPI_Finalize();
}

dstd::Error mpi_error(const char* what) {
    return dstd::Error::from_format<"MPI could not {}">(
        dstd::Condition::Unsupported, dstd::StringView(what));
}

/// Waits for @p request with @p mpi held only while MPI is called, so the
/// progress thread of a context that is still stopping can post its last
/// messages, which a peer may need before it joins this collective.
bool wait_unlocked(MPI_Request& request, std::unique_lock<std::mutex>& mpi) {
    for (;;) {
        int done = 0;
        if (MPI_Test(&request, &done, MPI_STATUS_IGNORE) != MPI_SUCCESS)
            return false;
        if (done) return true;
        mpi.unlock();
        std::this_thread::yield();
        mpi.lock();
    }
}

/// MVAPICH2 builds a node communicator with blocking collectives inside the
/// 16th collective call on a communicator, MPI_I* calls included. Groups whose
/// calls start in a different order on different ranks then deadlock inside
/// that build, so its shared-memory collectives must be off.
constexpr const char* MV2_SHMEM_COLL_ENV = "MV2_USE_SHMEM_COLL";
bool g_shmem_warned = false;

/// Warns once, with g_process_mutex held, when MVAPICH2 runs with its
/// shared-memory collectives on, as when the application started MPI itself.
void warn_shmem_coll_once() {
    if (g_shmem_warned) return;
    g_shmem_warned = true;
    char version[MPI_MAX_LIBRARY_VERSION_STRING] = {};
    int length = 0;
    if (MPI_Get_library_version(version, &length) != MPI_SUCCESS) return;
    if (std::strstr(version, "MVAPICH2") == nullptr) return;
    const auto value = common::Env::get<dstd::StringView>(MV2_SHMEM_COLL_ENV);
    if (value && *value == "0") return;
    DFTRACER_UTILS_LOG_WARN(
        "MVAPICH2 runs with {} unset or not 0; groups whose collectives start "
        "in a different order on different ranks can deadlock. Set {}=0 "
        "before MPI starts.",
        MV2_SHMEM_COLL_ENV, MV2_SHMEM_COLL_ENV);
}

/// What stops the threads when the last handle goes.
struct Guard {
    explicit Guard(dstd::SharedPtr<Context> held) : context(std::move(held)) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() { context->shutdown(false); }
    dstd::SharedPtr<Context> context;
};

/// Counts a thread of the context out when it leaves its loop.
struct Leaving {
    explicit Leaving(std::atomic<int>& count) : running(count) {}
    Leaving(const Leaving&) = delete;
    Leaving& operator=(const Leaving&) = delete;
    ~Leaving() { running.fetch_sub(1, std::memory_order_acq_rel); }
    std::atomic<int>& running;
};

}  // namespace

dstd::Result<dstd::SharedPtr<Context>> Context::make(const void* app_comm) {
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        int ready = 0;
        MPI_Initialized(&ready);
        int finalized = 0;
        MPI_Finalized(&finalized);
        if (finalized) {
            return dstd::Unexpected(dstd::Error::from_format<
                                    "MPI was finalized, so no communicator "
                                    "can start">(dstd::Condition::Unsupported));
        }
        int provided = 0;
        if (!ready) {
            // A value the user set wins.
            ::setenv(MV2_SHMEM_COLL_ENV, "0", 0);
            MPI_Init_thread(nullptr, nullptr, MPI_THREAD_MULTIPLE, &provided);
            if (!g_exit_hook) {
                g_exit_hook = true;
                std::atexit(&finalize_at_exit);
            }
        } else {
            MPI_Query_thread(&provided);
        }
        warn_shmem_coll_once();
        if (provided < MPI_THREAD_MULTIPLE) {
            return dstd::Unexpected(dstd::Error::from_format<
                                    "MPI gave thread level {} and the backend "
                                    "needs MPI_THREAD_MULTIPLE ({})">(
                dstd::Condition::Unsupported, provided,
                static_cast<int>(MPI_THREAD_MULTIPLE)));
        }
    }
    auto context = dstd::make_shared<Context>();
    std::unique_lock<std::mutex> mpi(g_mpi_mutex);
    const MPI_Comm base = app_comm != nullptr
                              ? *static_cast<const MPI_Comm*>(app_comm)
                              : MPI_COMM_WORLD;
    int rank = 0;
    int size = 1;
    MPI_Request request = MPI_REQUEST_NULL;
    if (MPI_Comm_rank(base, &rank) != MPI_SUCCESS ||
        MPI_Comm_size(base, &size) != MPI_SUCCESS ||
        MPI_Comm_idup(base, &context->messages_, &request) != MPI_SUCCESS ||
        !wait_unlocked(request, mpi)) {
        return dstd::Unexpected(mpi_error("duplicate the communicator"));
    }
    context->rank_ = static_cast<std::uint32_t>(rank);
    context->size_ = static_cast<std::uint32_t>(size);

    // The ranks that report one processor name share a node; the lowest of
    // them leads it.
    const auto stride = static_cast<std::size_t>(MPI_MAX_PROCESSOR_NAME);
    dstd::Vector<char> names(static_cast<std::size_t>(size) * stride, '\0');
    dstd::Vector<char> own(stride, '\0');
    int own_size = 0;
    const bool named =
        MPI_Get_processor_name(own.data(), &own_size) == MPI_SUCCESS;
    own[stride - 1] = '\0';
    if (!named ||
        MPI_Iallgather(own.data(), MPI_MAX_PROCESSOR_NAME, MPI_CHAR,
                       names.data(), MPI_MAX_PROCESSOR_NAME, MPI_CHAR,
                       context->messages_, &request) != MPI_SUCCESS ||
        !wait_unlocked(request, mpi)) {
        return dstd::Unexpected(mpi_error("gather the node of every rank"));
    }
    mpi.unlock();
    context->node_leaders_.resize(static_cast<std::size_t>(size), 0);
    for (std::size_t at = 0; at < static_cast<std::size_t>(size); ++at) {
        const char* slot = names.data() + at * stride;
        const dstd::StringView name(slot, ::strnlen(slot, stride));
        std::size_t leader = at;
        for (std::size_t other = 0; other < at; ++other) {
            const char* prior = names.data() + other * stride;
            if (dstd::StringView(prior, ::strnlen(prior, stride)) == name) {
                leader = other;
                break;
            }
        }
        context->node_leaders_[at] = static_cast<std::uint32_t>(leader);
    }

    context->running_.store(2, std::memory_order_release);
    context->progress_ = dstd::Thread([context] { context->progress_loop(); },
                                      "comm_mpi_progress");
    context->completion_ = dstd::Thread(
        [context] { context->completion_loop(); }, "comm_mpi_complete");
    context->progress_id_ = context->progress_.id();
    context->completion_id_ = context->completion_.id();
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        g_live.erase(std::remove_if(g_live.begin(), g_live.end(),
                                    [](const dstd::WeakPtr<Context>& weak) {
                                        return weak.expired();
                                    }),
                     g_live.end());
        g_live.push_back(context);
    }
    auto guard = dstd::make_shared<Guard>(context);
    return dstd::Result<dstd::SharedPtr<Context>>(
        dstd::SharedPtr<Context>(guard, context.get()));
}

// The messages communicator is not freed here: a free is collective, and
// contexts of different ranks end at different times. MPI_Finalize frees it.
Context::~Context() = default;

void Context::shutdown(bool force) {
    const dstd::Thread::Id me = dstd::Thread::current_id();
    const bool on_progress = me == progress_id_;
    const bool on_completion = me == completion_id_;
    std::unique_lock<std::mutex> life(lifecycle_, std::defer_lock);
    if (on_progress || on_completion) {
        // Another thread that is stopping the context may be joining this one.
        if (!life.try_lock()) return;
    } else {
        life.lock();
    }
    if (!stopped_) {
        if (native_pending() > 0) {
            if (!on_progress && !on_completion) {
                const std::uint64_t stop =
                    dstd::steady_now_ns() +
                    common::constants::comm_settings::MPI_NATIVE_DRAIN_MS *
                        1'000'000;
                while (native_pending() > 0 && dstd::steady_now_ns() < stop)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (native_pending() > 0) {
                std::fprintf(stderr,
                             "comm_mpi: %zu native collective requests are "
                             "still pending at shutdown and cannot be "
                             "cancelled; ending the job with MPI_Abort\n",
                             native_pending());
                std::fflush(stderr);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }
        }
        stopped_ = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            forced_ = forced_ || force;
        }
        wake_.notify_all();
        if (on_progress)
            progress_.detach();
        else
            progress_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completion_stopping_ = true;
        }
        finished_wake_.notify_all();
        if (on_completion)
            completion_.detach();
        else
            completion_.join();
    } else if (force) {
        std::lock_guard<std::mutex> lock(mutex_);
        forced_ = true;
        wake_.notify_all();
    }
    // The threads that stop alone are out of their loops before MPI ends.
    if (!on_progress && !on_completion) {
        for (int turns = 0;
             running_.load(std::memory_order_acquire) > 0 && turns < 5000;
             ++turns)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Context::shutdown_all() {
    dstd::Vector<dstd::SharedPtr<Context>> live;
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        for (const auto& weak : g_live)
            if (auto context = weak.lock()) live.push_back(std::move(context));
        g_live.clear();
    }
    for (const auto& context : live) context->shutdown(true);
}

void Context::submit(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void Context::complete(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_.push_back(std::move(job));
    }
    finished_wake_.notify_one();
}

void Context::watch(MPI_Request request, Job done) {
    watched_.push_back(Watched{request, std::move(done)});
    watched_count_.store(watched_.size(), std::memory_order_relaxed);
}

std::uint64_t Context::add_poller(dstd::Function<bool()> poller) {
    std::lock_guard<std::mutex> lock(mutex_);
    pollers_.push_back(
        Poller{++next_poller_,
               dstd::make_shared<dstd::Function<bool()>>(std::move(poller))});
    return next_poller_;
}

void Context::remove_poller(std::uint64_t id) {
    dstd::SharedPtr<dstd::Function<bool()>> gone;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto at = pollers_.begin(); at != pollers_.end(); ++at) {
            if (at->id != id) continue;
            gone = std::move(at->run);
            pollers_.erase(at);
            break;
        }
    }
    // Released outside the lock: its owner may be the last user of the context.
}

std::size_t Context::watched() const {
    return watched_count_.load(std::memory_order_relaxed);
}

void Context::progress_loop() {
    const Leaving leaving(running_);
    auto idle = std::chrono::microseconds(0);
    std::size_t quiet = 0;
    for (;;) {
        dstd::Deque<Job> jobs;
        dstd::Vector<dstd::SharedPtr<dstd::Function<bool()>>> pollers;
        bool stop = false;
        bool forced = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs.swap(jobs_);
            stop = stopping_;
            forced = forced_;
            for (const Poller& poller : pollers_) pollers.push_back(poller.run);
        }
        bool progressed = !jobs.empty();
        for (Job& job : jobs) {
            const std::lock_guard<std::mutex> mpi(g_mpi_mutex);
            job();
        }
        for (std::size_t at = 0; at < watched_.size();) {
            int flag = 0;
            {
                const std::lock_guard<std::mutex> mpi(g_mpi_mutex);
                MPI_Test(&watched_[at].request, &flag, MPI_STATUS_IGNORE);
            }
            if (!flag) {
                ++at;
                continue;
            }
            Job done = std::move(watched_[at].done);
            watched_[at] = std::move(watched_.back());
            watched_.pop_back();
            watched_count_.store(watched_.size(), std::memory_order_relaxed);
            progressed = true;
            if (done) done();
        }
        if (!stop) {
            for (const auto& poller : pollers) {
                const std::lock_guard<std::mutex> mpi(g_mpi_mutex);
                if ((*poller)()) progressed = true;
            }
        }
        if (stop && (forced || (watched_.empty() && jobs.empty()))) return;
        if (progressed) {
            idle = std::chrono::microseconds(0);
            quiet = 0;
            continue;
        }
        if (native_pending() > 0 && quiet < NATIVE_SPIN_TURNS) {
            // A nonblocking collective moves only while this thread calls
            // MPI, so it polls without a sleep for a while after the last
            // progress, and then backs off like any other wait.
            ++quiet;
            std::this_thread::yield();
            continue;
        }
        if (idle < std::chrono::microseconds(1000))
            idle += std::chrono::microseconds(20);
        std::unique_lock<std::mutex> lock(mutex_);
        if (jobs_.empty() && !stopping_) wake_.wait_for(lock, idle);
    }
}

void Context::completion_loop() {
    const Leaving leaving(running_);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            finished_wake_.wait(lock, [this] {
                return !finished_.empty() || completion_stopping_;
            });
            if (finished_.empty()) return;
            job = std::move(finished_.front());
            finished_.pop_front();
        }
        job();
    }
}

}  // namespace dftracer::utils::comm::mpi
