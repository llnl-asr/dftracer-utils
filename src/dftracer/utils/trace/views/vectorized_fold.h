#ifndef DFTRACER_UTILS_TRACE_VIEWS_VECTORIZED_FOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_VECTORIZED_FOLD_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/native_row_fold.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::views::detail {

/// A Fold that hands each scanned batch to `on_batch` as a native DataFrame -
/// the batch's events materialized into columns (build_row_frame) - so a fold
/// runs SIMD column ops per batch instead of a per-event loop. Bounded memory:
/// one batch is materialized at a time, never the whole scan.
///
/// State is a per-worker monoid so the fold rides the parallel fused scan: the
/// driver calls `slice()` per worker, each folds its batches lock-free, and the
/// slices are combined with `merge` after the join. `merge` MUST be
/// associative. The caller pulls `result()` after fuse() returns.
template <class State>
class VectorizedFold : public Fold {
   public:
    using Init = std::function<State()>;
    using OnBatch = std::function<void(State&, const dataframe::DataFrame&)>;
    using Merge = std::function<void(State&, const State&)>;

    /// `intern` is the scan's shared, caller-owned interner (build_row_frame
    /// resolves string ids through it); `select` projects columns (empty =
    /// every column); `time_scale` multiplies ts/dur.
    VectorizedFold(const dftracer::utils::StringIntern& intern, Init init,
                   OnBatch on_batch, Merge merge,
                   std::vector<std::string> select = {},
                   double time_scale = 1.0)
        : intern_(&intern),
          init_(std::move(init)),
          on_batch_(std::move(on_batch)),
          merge_(std::move(merge)),
          select_(std::move(select)),
          time_scale_(time_scale),
          state_(init_()) {}

    bool accepts(const ScanShape&) const override { return true; }
    bool needs_args() const override { return true; }

    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<VectorizedFold>(*intern_, init_, on_batch_,
                                                merge_, select_, time_scale_);
    }

    void step(const FoldBatch& batch) override {
        scratch_.clear();
        for (const FoldEvent& ev : batch.events) {
            if (ev.phase == RecordPhase::METADATA ||
                ev.phase == RecordPhase::UNKNOWN)
                continue;
            scratch_.push_back(ev);
        }
        if (scratch_.empty()) return;
        on_batch_(state_, build_row_frame(scratch_, *intern_, select_,
                                          time_scale_, nullptr));
    }

    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}

    void merge(Fold& other) override {
        merge_(state_, static_cast<VectorizedFold&>(other).state_);
    }

    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return false;  // pulls a result via result(), persists nothing
    }

    const State& result() const { return state_; }

   private:
    const dftracer::utils::StringIntern* intern_;
    Init init_;
    OnBatch on_batch_;
    Merge merge_;
    std::vector<std::string> select_;
    double time_scale_;
    State state_;
    std::vector<FoldEvent> scratch_;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VECTORIZED_FOLD_H
