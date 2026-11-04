// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_DLIO_WORKER_QUEUE_H
#define DFTRACER_UTILS_UTILITIES_DLIO_WORKER_QUEUE_H

#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::utilities::dlio {

struct WorkInterval {
    double start_time;
    double end_time;
    double preprocess_duration;
};

/// Sampler returns {batch_time, preprocess_time} where batch_time is the wall
/// clock the worker spends on the batch (preprocess + I/O, scaled).
using BatchTimeSampler = dstd::Function<dstd::Pair<double, double>()>;

class WorkerQueue {
   public:
    WorkerQueue(int worker_count, int prefetch_factor)
        : worker_count_(worker_count),
          queue_capacity_(static_cast<std::size_t>(worker_count) *
                          static_cast<std::size_t>(prefetch_factor)) {}

    dstd::Vector<WorkInterval> produce_batches(double current_time,
                                               const BatchTimeSampler& sampler);

    /// Returns time consumed (stall + base_overhead).
    double consume_batch(double current_time, double base_overhead);

    std::size_t queue_depth() const { return ready_batches_.size(); }
    bool had_stall() const { return stall_count_ > 0; }
    std::uint64_t stall_count() const { return stall_count_; }

   private:
    int worker_count_;
    std::size_t queue_capacity_;
    std::uint64_t stall_count_ = 0;
    dstd::Vector<double> ready_batches_;  ///< sorted ready times
    dstd::Vector<double> worker_free_times_;
};

}  // namespace dftracer::utils::utilities::dlio

#endif
