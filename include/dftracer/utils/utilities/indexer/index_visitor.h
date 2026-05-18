#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_VISITOR_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_VISITOR_H

#include <dftracer/utils/core/coro/task.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::indexer {

class IndexDatabaseWriterContext;

/// Shared buffer for zero-copy line passing. The string_view passed to
/// on_line points into this buffer; storing the shared_ptr keeps it alive.
using SharedLineBuffer = std::shared_ptr<std::string>;

class IndexVisitor {
   public:
    virtual ~IndexVisitor() = default;

    virtual void begin(std::size_t num_checkpoints) = 0;

    virtual coro::CoroTask<void> on_checkpoint(std::size_t checkpoint_idx) = 0;

    virtual coro::CoroTask<void> on_chunk(const char* data, std::size_t len,
                                          std::size_t checkpoint_idx) {
        auto buffer = std::make_shared<std::string>(data, len);
        std::size_t pos = 0;
        while (pos < len) {
            const void* nl = std::memchr(data + pos, '\n', len - pos);
            if (!nl) break;
            std::size_t end = static_cast<const char*>(nl) - data;
            on_line(std::string_view(buffer->data() + pos, end - pos), buffer,
                    checkpoint_idx);
            pos = end + 1;
        }
        co_return;
    }

    /// Called for each line. The line string_view points into buffer.
    /// Implementations that need the data to outlive this call should
    /// store the buffer shared_ptr (zero-copy) rather than copying line.
    virtual void on_line(std::string_view line, SharedLineBuffer buffer,
                         std::size_t checkpoint_idx) = 0;

    virtual coro::CoroTask<void> flush() { co_return; }

    /// Cheap hint that drain_pending() should be called to apply
    /// backpressure. Default false. Polled after each on_line call.
    virtual bool wants_drain() const noexcept { return false; }

    /// Drain accumulated work via async ops (e.g. channel send). Suspends
    /// the calling coroutine when downstream is full -- real backpressure
    /// without blocking an executor thread.
    virtual coro::CoroTask<void> drain_pending() { co_return; }

    virtual void finalize(IndexDatabaseWriterContext& writer, int file_id) = 0;
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_VISITOR_H
