#ifndef DFTRACER_UTILS_TRACE_VIEWS_STREAM_ROW_FOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_STREAM_ROW_FOLD_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/trace/views/batch_bridge.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::index::plan {
class GroupResolver;
}  // namespace dftracer::utils::index::plan

namespace dftracer::utils::trace::views::detail {

/// Approximate resident size of `m`'s columns (flat buffers exact via
/// buffer_bytes; String/Binary data from the offset span; List/Struct and a
/// missing offset buffer fall back to a flat per-row estimate).
std::uint64_t morsel_bytes(const dataframe::Morsel& m);

/// A row-query Fold that pushes each scanned batch as its own Morsel through a
/// shared channel instead of accumulating every event like NativeRowFold.
/// `budget` throttles in-flight (sent but not yet received) morsels by byte
/// size, shared across every slice and the consuming cursor. The channel
/// closes once every fold instance (the original plus each slice) releases
/// its producer registration. With a `branch` plan, the fold serves one branch
/// of a shared session scan: it keeps only the events that plan's phase and
/// query select, as its own scan would. An `ordered` fold stamps each morsel's
/// batch_index with its unit's seq and ends each unit with an empty morsel of
/// that seq, so the consumer can hand rows out in unit order.
class StreamRowFold : public Fold {
   public:
    StreamRowFold(std::shared_ptr<coro::Channel<dataframe::Morsel>> channel,
                  std::shared_ptr<coro::CoroSemaphore> budget,
                  std::shared_ptr<dftracer::utils::StringIntern> intern,
                  std::vector<std::string> select, double time_scale = 1.0,
                  bool keep_metadata = false, bool emit_dyn = false,
                  std::shared_ptr<const ViewPlan> branch = nullptr,
                  std::shared_ptr<const std::atomic<bool>> dropped = nullptr,
                  bool by_path = false, bool ordered = false,
                  std::shared_ptr<const JsonColumns> json = nullptr)
        : channel_(std::move(channel)),
          budget_(std::move(budget)),
          intern_(std::move(intern)),
          select_(std::move(select)),
          time_scale_(time_scale),
          keep_metadata_(keep_metadata),
          emit_dyn_(emit_dyn),
          branch_(std::move(branch)),
          dropped_(std::move(dropped)),
          by_path_(by_path),
          ordered_(ordered),
          json_(std::move(json)),
          guard_(channel_.get()) {}

    bool accepts(const ScanShape&) const override { return true; }
    bool needs_args() const override { return true; }
    bool wants_metadata() const override { return keep_metadata_; }

    std::vector<std::string> extra_captures() const override {
        std::vector<std::string> fields = select_;
        if (branch_ && branch_->query)
            for (std::string_view f : branch_->query->fields())
                fields.emplace_back(f);
        return row_fold_extra_captures(fields);
    }

    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<StreamRowFold>(
            channel_, budget_, intern_, select_, time_scale_, keep_metadata_,
            emit_dyn_, branch_, dropped_, by_path_, ordered_, json_);
    }

    void step(const FoldBatch& batch) override;

    void seal_unit(const ScanUnit& unit) override { end_unit(unit); }
    void drop_unit(const ScanUnit& unit) override { end_unit(unit); }
    void skip_unit(const ScanUnit& unit) override { end_unit(unit); }
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
    void end_unit(const ScanUnit& unit);

    coro::CoroTask<void> send(dataframe::Morsel m, std::uint64_t bytes) {
        co_await budget_->acquire(bytes);
        co_await channel_->send(std::move(m));
    }

    std::shared_ptr<coro::Channel<dataframe::Morsel>> channel_;
    std::shared_ptr<coro::CoroSemaphore> budget_;
    std::shared_ptr<dftracer::utils::StringIntern> intern_;
    std::vector<std::string> select_;
    double time_scale_;
    bool keep_metadata_;  // phase("metadata"): keep ph=M records
    bool emit_dyn_;       // auto_numeric_metrics: emit per-batch dyn columns
    std::shared_ptr<const ViewPlan> branch_;
    // Set once the consumer let go; the fold then stops sending to it.
    std::shared_ptr<const std::atomic<bool>> dropped_;
    bool by_path_;
    bool ordered_;
    std::shared_ptr<const JsonColumns> json_;
    duql::ValueMap qmap_;
    coro::Channel<dataframe::Morsel>::ProducerGuard guard_;

    // fuse() awaits take_pending()'s task right after step() and before the
    // next step() on this fold, so one reused slot is enough.
    std::optional<coro::CoroTask<void>> pending_task_;
    ::dftu_task* pending_ = nullptr;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_STREAM_ROW_FOLD_H
