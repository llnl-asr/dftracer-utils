#ifndef DFTRACER_UTILS_TRACE_VIEWS_STREAM_ROW_FOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_STREAM_ROW_FOLD_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/native_row_fold.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::trace::views::detail {

class GroupResolver;

/// Approximate resident size of `m`'s columns (flat buffers exact via
/// buffer_bytes; String/Binary data from the offset span; List/Struct and a
/// missing offset buffer fall back to a flat per-row estimate).
std::uint64_t morsel_bytes(const dataframe::Morsel& m);

/// A row-query Fold that pushes each scanned batch as its own Morsel through a
/// shared channel instead of accumulating every event like NativeRowFold.
/// `budget` throttles in-flight (sent but not yet received) morsels by byte
/// size, shared across every slice and the consuming cursor. The channel
/// closes once every fold instance (the original plus each slice) releases
/// its producer registration.
class StreamRowFold : public Fold {
   public:
    StreamRowFold(std::shared_ptr<coro::Channel<dataframe::Morsel>> channel,
                  std::shared_ptr<coro::CoroSemaphore> budget,
                  std::shared_ptr<dftracer::utils::StringIntern> intern,
                  std::vector<std::string> select, double time_scale = 1.0,
                  std::shared_ptr<const GroupResolver> resolver = nullptr,
                  bool keep_metadata = false, bool emit_dyn = false)
        : channel_(std::move(channel)),
          budget_(std::move(budget)),
          intern_(std::move(intern)),
          select_(std::move(select)),
          time_scale_(time_scale),
          resolver_(std::move(resolver)),
          keep_metadata_(keep_metadata),
          emit_dyn_(emit_dyn),
          guard_(channel_.get()) {}

    bool accepts(const ScanShape&) const override { return true; }
    bool needs_args() const override { return true; }

    std::vector<std::string> extra_captures() const override {
        return row_fold_extra_captures(select_);
    }

    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<StreamRowFold>(channel_, budget_, intern_,
                                               select_, time_scale_, resolver_,
                                               keep_metadata_, emit_dyn_);
    }

    void step(const FoldBatch& batch) override;

    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}
    void merge(Fold&) override {}

    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return true;
    }

    ::dftu_task* take_pending() override {
        ::dftu_task* t = pending_;
        pending_ = nullptr;
        return t;
    }

   private:
    coro::CoroTask<void> send(dataframe::Morsel m, std::uint64_t bytes) {
        co_await budget_->acquire(bytes);
        co_await channel_->send(std::move(m));
    }

    std::shared_ptr<coro::Channel<dataframe::Morsel>> channel_;
    std::shared_ptr<coro::CoroSemaphore> budget_;
    std::shared_ptr<dftracer::utils::StringIntern> intern_;
    std::vector<std::string> select_;
    double time_scale_;
    std::shared_ptr<const GroupResolver> resolver_;
    bool keep_metadata_;  // phase("metadata"): keep ph=M records
    bool emit_dyn_;       // auto_numeric_metrics: emit per-batch dyn columns
    coro::Channel<dataframe::Morsel>::ProducerGuard guard_;

    // fuse() awaits take_pending()'s task right after step() and before the
    // next step() on this fold, so one reused slot is enough.
    std::optional<coro::CoroTask<void>> pending_task_;
    ::dftu_task* pending_ = nullptr;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_STREAM_ROW_FOLD_H
