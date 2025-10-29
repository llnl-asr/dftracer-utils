#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_THREAD_SCHEDULER_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_THREAD_SCHEDULER_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/pipeline/executors/scheduler/scheduler.h>
#include <dftracer/utils/core/pipeline/executors/scheduler/task_item.h>
#include <dftracer/utils/core/tasks/tasks.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils {

class Pipeline;
class ExecutorContext;

class ThreadScheduler : public Scheduler {
   public:
    ThreadScheduler();
    ~ThreadScheduler();
    ThreadScheduler(const ThreadScheduler&) = delete;
    ThreadScheduler& operator=(const ThreadScheduler&) = delete;
    ThreadScheduler(ThreadScheduler&&) = default;
    ThreadScheduler& operator=(ThreadScheduler&&) = default;

    void initialize(std::size_t num_threads);
    void shutdown();
    virtual void reset() override;

    PipelineOutput execute(const Pipeline& pipeline,
                           const std::any& input) override;

    void submit_dynamic_task(TaskIndex task_id, Task* task_ptr,
                             const std::any& input);
    void add_dynamic_dependency_tracking(
        TaskIndex task_id, const std::vector<TaskIndex>& dependencies);

   private:
    std::queue<TaskItem> ready_queue_;
    std::atomic<int> pending_count_{0};
    std::vector<std::thread> workers_;

    std::unordered_map<TaskIndex, std::atomic<int>> remaining_deps_;
    std::unordered_map<TaskIndex, std::vector<TaskIndex>> dependents_;
    std::unordered_map<TaskIndex, std::vector<TaskIndex>> dependencies_;
    std::unordered_map<TaskIndex, std::atomic<int>> dependent_count_;

    std::unordered_set<TaskIndex> tasks_with_futures_;

    std::unordered_map<TaskIndex, std::shared_ptr<std::promise<std::any>>>
        promises_;

    std::atomic<bool> running_{true};
    std::condition_variable done_cv_;
    std::condition_variable queue_cv_;
    std::mutex done_mutex_;
    std::mutex queue_mutex_;
    std::mutex results_mutex_;

    const Pipeline* current_pipeline_{nullptr};
    ExecutorContext* current_execution_context_{nullptr};

    void worker_thread();

    void setup_dependencies(const Pipeline& pipeline);
    void queue_ready_tasks(const Pipeline& pipeline, const std::any& input);
    void process_completion(TaskIndex completed_task);
    PipelineOutput extract_terminal_outputs(const Pipeline& pipeline);
    Task* get_task(TaskIndex task_id) const;
    std::vector<TaskIndex> get_dependencies(TaskIndex task_id) const;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_THREAD_SCHEDULER_H
