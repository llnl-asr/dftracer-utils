// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_BUILD_INDEX_VISITOR_H
#define DFTRACER_UTILS_TRACE_INDEX_BUILD_INDEX_VISITOR_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/result.h>

#include <cstddef>

namespace dftracer::utils::coro {
class Scope;
}  // namespace dftracer::utils::coro

namespace dftracer::utils::trace::index::build {

class IndexVisitor {
   public:
    virtual ~IndexVisitor() = default;

    virtual void begin(std::size_t checkpoint_count) = 0;

    /// The scope the indexer runs this file in, called after begin(). A
    /// visitor may spawn work there that it awaits by flush(); the scope
    /// outlives the file's chunks. Default: ignored.
    virtual void bind_scope(coro::Scope* /*scope*/) {}

    /// A visitor's failure ends the pass with its error.
    virtual coro::CoroTask<dstd::Result<void>> on_checkpoint(
        std::size_t checkpoint_idx) = 0;

    /// Process one decompressed chunk of a member's plaintext. A chunk is a
    /// batch of lines; the consumer splits or parses as it needs. A line may
    /// straddle a chunk boundary, so a consumer that needs whole lines carries
    /// the partial tail across calls.
    virtual coro::CoroTask<dstd::Result<void>> on_chunk(
        const char* data, std::size_t len, std::size_t checkpoint_idx) = 0;

    /// The chunks so far end in a line that no later chunk completes, since
    /// the member they came from is damaged there. A consumer that carries a
    /// partial tail reads it as a line of its own, so it does not join the
    /// first line of the next member. Default: ignored.
    virtual coro::CoroTask<dstd::Result<void>> on_line_cut(
        std::size_t /*checkpoint_idx*/) {
        co_return dstd::Result<void>{};
    }

    virtual coro::CoroTask<dstd::Result<void>> flush() {
        co_return dstd::Result<void>{};
    }

    /// Cheap hint that drain_pending() should be called to apply
    /// backpressure. Default false. Polled after each on_chunk call.
    virtual bool wants_drain() const noexcept { return false; }

    /// Drain accumulated work via async ops (e.g. channel send). Suspends
    /// the calling coroutine when downstream is full -- real backpressure
    /// without blocking an executor thread.
    virtual coro::CoroTask<dstd::Result<void>> drain_pending() {
        co_return dstd::Result<void>{};
    }
};

}  // namespace dftracer::utils::trace::index::build

#endif  // DFTRACER_UTILS_TRACE_INDEX_BUILD_INDEX_VISITOR_H
