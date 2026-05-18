#ifndef DFTRACER_UTILS_UTILITIES_DLIO_WORKER_QUEUE_H
#define DFTRACER_UTILS_UTILITIES_DLIO_WORKER_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::dlio {

struct WorkInterval {
    double start_time;
    double end_time;
    double preprocess_duration;
};

// Sampler returns {batch_time, preprocess_time} where batch_time is the wall
// clock the worker spends on the batch (preprocess + I/O, scaled).
using BatchTimeSampler = std::function<std::pair<double, double>()>;

class WorkerQueue {
   public:
    WorkerQueue(int num_workers, int prefetch_factor)
        : num_workers_(num_workers),
          prefetch_factor_(prefetch_factor),
          queue_capacity_(static_cast<std::size_t>(num_workers) *
                          static_cast<std::size_t>(prefetch_factor)) {}

    std::vector<WorkInterval> produce_batches(double current_time,
                                              const BatchTimeSampler& sampler);

    // Returns time consumed (stall + base_overhead).
    double consume_batch(double current_time, double base_overhead);

    std::size_t queue_depth() const { return ready_batches_.size(); }
    bool had_stall() const { return stall_count_ > 0; }
    std::uint64_t stall_count() const { return stall_count_; }

   private:
    int num_workers_;
    int prefetch_factor_;
    std::size_t queue_capacity_;
    std::uint64_t stall_count_ = 0;
    std::vector<double> ready_batches_;  // sorted ready times
    std::vector<double> worker_free_times_;
};

}  // namespace dftracer::utils::utilities::dlio

#endif
