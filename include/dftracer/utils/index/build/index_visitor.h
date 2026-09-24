#ifndef DFTRACER_UTILS_INDEX_BUILD_INDEX_VISITOR_H
#define DFTRACER_UTILS_INDEX_BUILD_INDEX_VISITOR_H

#include <dftracer/utils/core/coro/task.h>

#include <cstddef>

namespace dftracer::utils::index::store {
class IndexDatabaseWriterContext;
}  // namespace dftracer::utils::index::store

namespace dftracer::utils::index::build {

class IndexVisitor {
   public:
    virtual ~IndexVisitor() = default;

    virtual void begin(std::size_t num_checkpoints) = 0;

    virtual coro::CoroTask<void> on_checkpoint(std::size_t checkpoint_idx) = 0;

    /// Process one decompressed chunk of a member's plaintext. A chunk is a
    /// batch of lines; the consumer splits or parses as it needs. A line may
    /// straddle a chunk boundary, so a consumer that needs whole lines carries
    /// the partial tail across calls.
    virtual coro::CoroTask<void> on_chunk(const char* data, std::size_t len,
                                          std::size_t checkpoint_idx) = 0;

    virtual coro::CoroTask<void> flush() { co_return; }

    /// Cheap hint that drain_pending() should be called to apply
    /// backpressure. Default false. Polled after each on_chunk call.
    virtual bool wants_drain() const noexcept { return false; }

    /// Drain accumulated work via async ops (e.g. channel send). Suspends
    /// the calling coroutine when downstream is full -- real backpressure
    /// without blocking an executor thread.
    virtual coro::CoroTask<void> drain_pending() { co_return; }

    virtual void finalize(index::store::IndexDatabaseWriterContext& writer,
                          int file_id) = 0;
};

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_INDEX_VISITOR_H
