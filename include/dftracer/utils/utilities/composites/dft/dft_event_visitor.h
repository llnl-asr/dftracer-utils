#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFT_EVENT_VISITOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFT_EVENT_VISITOR_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/indexer/index_visitor.h>
#include <simdjson.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft {

struct EventRecord {
    const DFTracerEvent& ev;
    const common::json::JsonValue& json;
    std::string_view line;
    indexer::SharedLineBuffer line_buffer;  // keeps line data alive
    std::size_t checkpoint_idx;
    std::size_t line_number;
    simdjson::dom::element args_dom{};
    bool has_args{false};
};

class DftEventVisitor {
   public:
    virtual ~DftEventVisitor() = default;

    virtual void begin(std::size_t num_checkpoints) = 0;

    virtual void on_checkpoint(std::size_t checkpoint_idx) = 0;

    virtual void on_event(const EventRecord& record) = 0;

    // Hint that the visitor has accumulated work that should be drained.
    // Cheap (no allocation/co_await): the dispatcher polls this after every
    // on_event and only co_awaits drain_pending() when true.
    virtual bool wants_drain() const noexcept { return false; }

    // Drain any accumulated work via async operations (e.g. channel send).
    // Suspends the calling coroutine when downstream is full, providing
    // real backpressure without blocking an executor thread.
    virtual coro::CoroTask<void> drain_pending() { co_return; }

    virtual coro::CoroTask<void> on_file_complete() { co_return; }

    virtual std::unique_ptr<DftEventVisitor> create_parallel_slice() const {
        return nullptr;
    }
    virtual void merge_parallel_slice(DftEventVisitor& /*slice*/) {}

    /// In parallel-flush mode, slices receive events with slice-local line
    /// numbers (0..N-1). The dispatcher calls this on the slice before
    /// merge_parallel_slice with the cumulative successful-event count of
    /// prior slices, so the slice can renumber its stored line indices.
    virtual void set_line_offset(std::size_t /*offset*/) {}

    /// Successful events processed by this slice. Used by the dispatcher to
    /// propagate line offsets across slices in byte order.
    virtual std::size_t parallel_event_count() const { return 0; }

    virtual bool needs_args_map() const { return false; }
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFT_EVENT_VISITOR_H
