// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_VECTORIZED_FOLD_H
#define DFTRACER_UTILS_TRACE_VIEWS_VECTORIZED_FOLD_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/native_row_fold.h>

#include <utility>

namespace dftracer::utils::trace::views::detail {

/// A Fold that hands each scanned batch to `on_batch` as a native DataFrame -
/// the batch's events materialized into columns (build_row_frame) - so a fold
/// runs SIMD column ops per batch instead of a per-event loop. Bounded memory:
/// one batch is materialized at a time, never the whole scan.
///
/// State is a per-worker monoid so the fold rides the parallel fused scan: the
/// driver calls `slice()` per worker, each folds its batches lock-free, and the
/// slices are combined with `merge` after the join. `merge` MUST be
/// associative. The caller pulls `result()` after fuse() returns. All slices
/// share one copy of `init`, `on_batch` and `merge` and call them at the same
/// time, so they keep no state of their own.
template <class State>
class VectorizedFold : public Fold {
   public:
    using Init = dstd::Function<State()>;
    using OnBatch = dstd::Function<void(State&, const dataframe::DataFrame&)>;
    using Merge = dstd::Function<void(State&, const State&)>;

    /// `intern` is the scan's shared, caller-owned interner (build_row_frame
    /// resolves string ids through it); `select` projects columns (empty =
    /// every column); `time_scale` multiplies ts/dur.
    VectorizedFold(const dftracer::utils::dstd::StringIntern& intern, Init init,
                   OnBatch on_batch, Merge merge,
                   dstd::Vector<dstd::String> select = {},
                   double time_scale = 1.0)
        : VectorizedFold(&intern, dstd::make_shared<Init>(std::move(init)),
                         dstd::make_shared<OnBatch>(std::move(on_batch)),
                         dstd::make_shared<Merge>(std::move(merge)),
                         std::move(select), time_scale) {}

    VectorizedFold(const dftracer::utils::dstd::StringIntern* intern,
                   dstd::SharedPtr<Init> init,
                   dstd::SharedPtr<OnBatch> on_batch,
                   dstd::SharedPtr<Merge> merge,
                   dstd::Vector<dstd::String> select, double time_scale)
        : intern_(intern),
          init_(std::move(init)),
          on_batch_(std::move(on_batch)),
          merge_(std::move(merge)),
          select_(std::move(select)),
          time_scale_(time_scale),
          state_((*init_)()) {}

    bool accepts(const ScanShape&) const override { return true; }
    bool needs_args() const override { return true; }

    dstd::UniquePtr<Fold> slice() const override {
        return dstd::make_unique<VectorizedFold>(intern_, init_, on_batch_,
                                                 merge_, select_, time_scale_);
    }

    dstd::Result<void> step(const FoldBatch& batch) override {
        scratch_.clear();
        for (const FoldEvent& ev : batch.events) {
            if (ev.phase == RecordPhase::Metadata ||
                ev.phase == RecordPhase::Unknown)
                continue;
            scratch_.push_back(ev);
        }
        if (scratch_.empty()) return {};
        (*on_batch_)(state_,
                     build_row_frame(scratch_, *intern_, select_, time_scale_));
        return {};
    }

    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}

    dstd::Result<void> merge(Fold& other) override {
        (*merge_)(state_, static_cast<VectorizedFold&>(other).state_);
        return {};
    }

    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return false;  // pulls a result via result(), persists nothing
    }

    const State& result() const { return state_; }

   private:
    const dftracer::utils::dstd::StringIntern* intern_;
    dstd::SharedPtr<Init> init_;
    dstd::SharedPtr<OnBatch> on_batch_;
    dstd::SharedPtr<Merge> merge_;
    dstd::Vector<dstd::String> select_;
    double time_scale_;
    State state_;
    dstd::Vector<FoldEvent> scratch_;
};

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VECTORIZED_FOLD_H
