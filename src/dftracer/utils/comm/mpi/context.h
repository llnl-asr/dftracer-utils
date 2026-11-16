// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_MPI_CONTEXT_H
#define DFTRACER_UTILS_COMM_MPI_CONTEXT_H

#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <mpi.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::comm::mpi {

/**
 * @brief The MPI state of one process and the two threads that serve it.
 *
 * Every MPI call after construction runs on the progress thread. The progress
 * thread issues calls, tests the requests it watches and polls the pollers,
 * and never runs code of the program: whatever a caller must run goes through
 * @ref complete, which a second thread runs in order.
 *
 * The threads hold the context, so it outlives them, and the handle that
 * @ref make gives stops them when its last copy goes, on any thread: a thread
 * of the context that drops the last copy detaches itself and joins the
 * other, so no thread ever joins itself. The first context of the process
 * initializes MPI when no one did. When this library initialized MPI, a hook
 * at the end of the process stops the threads of every live context, and then
 * finalizes MPI.
 */
class Context {
   public:
    using Job = dstd::Function<void()>;

    /// Adopts the communicator at @p app_comm, or MPI_COMM_WORLD when null.
    /// Needs MPI at thread level MPI_THREAD_MULTIPLE, and fails with an error
    /// that names the level it got otherwise.
    static dstd::Result<dstd::SharedPtr<Context>> make(const void* app_comm);

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context();

    std::uint32_t rank() const noexcept { return rank_; }
    std::uint32_t size() const noexcept { return size_; }

    /// The communicator of the point-to-point messages and of the native
    /// collectives: a duplicate of the adopted one.
    MPI_Comm messages() const noexcept { return messages_; }

    /// The world rank that leads the node of each world rank, which names the
    /// node.
    const dstd::Vector<std::uint32_t>& node_leaders() const noexcept {
        return node_leaders_;
    }

    /// Runs @p job on the progress thread.
    void submit(Job job);

    /// Runs @p job on the completion thread; the progress thread only enqueues
    /// it.
    void complete(Job job);

    /// Watches @p request: @p done runs on the progress thread once MPI says
    /// the request completed. Called on the progress thread only.
    void watch(MPI_Request request, Job done);

    /// Adds a poller, run on the progress thread at each turn; it gives true
    /// when it made progress. Gives the id that @ref remove_poller takes.
    std::uint64_t add_poller(dstd::Function<bool()> poller);

    /// Stops running the poller @p id. A turn that already took it may run it
    /// once more, so the poller must keep what it uses alive.
    void remove_poller(std::uint64_t id);

    /// How many requests the progress thread still watches.
    std::size_t watched() const;

    /// Counts a native collective request in flight, from its post to the end
    /// of its watch. A native request cannot be cancelled, so a context that
    /// stops with some still pending ends the job: see @ref shutdown.
    void native_begin() noexcept {
        native_pending_.fetch_add(1, std::memory_order_acq_rel);
    }
    void native_end() noexcept {
        native_pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
    std::size_t native_pending() const noexcept {
        return native_pending_.load(std::memory_order_acquire);
    }

    /// Stops both threads and waits for them, except the one this call runs
    /// on, which it detaches. A context with native requests still pending
    /// waits a short while for them, and then ends the job with MPI_Abort and
    /// one line on stderr instead of finalizing MPI, so the job never hangs on
    /// a collective that a peer never matched. @p force ends the progress
    /// thread with requests still watched, which only the end of the process
    /// may do. Does nothing for a context that already stopped.
    void shutdown(bool force);

    /// Stops the threads of every live context, for the exit hook.
    static void shutdown_all();

    /// Builds the context; use @ref make.
    Context() = default;

   private:
    void progress_loop();
    void completion_loop();

    std::uint32_t rank_ = 0;
    std::uint32_t size_ = 1;
    MPI_Comm messages_ = MPI_COMM_NULL;
    dstd::Vector<std::uint32_t> node_leaders_;

    struct Watched {
        MPI_Request request;
        Job done;
    };

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    dstd::Deque<Job> jobs_;
    dstd::Deque<Job> finished_;
    std::condition_variable finished_wake_;
    dstd::Vector<Watched> watched_;
    std::atomic<std::size_t> watched_count_{0};
    std::atomic<std::size_t> native_pending_{0};
    struct Poller {
        std::uint64_t id;
        dstd::SharedPtr<dstd::Function<bool()>> run;
    };
    dstd::Vector<Poller> pollers_;
    std::uint64_t next_poller_ = 0;
    bool stopping_ = false;
    bool forced_ = false;
    bool completion_stopping_ = false;
    bool stopped_ = false;
    std::mutex lifecycle_;
    std::atomic<int> running_{0};
    dstd::Thread::Id progress_id_;
    dstd::Thread::Id completion_id_;
    dstd::Thread progress_;
    dstd::Thread completion_;
};

}  // namespace dftracer::utils::comm::mpi

#endif  // DFTRACER_UTILS_COMM_MPI_CONTEXT_H
