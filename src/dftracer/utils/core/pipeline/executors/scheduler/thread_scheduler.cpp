#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/pipeline/executors/executor_context.h>
#include <dftracer/utils/core/pipeline/executors/scheduler/task_item.h>
#include <dftracer/utils/core/pipeline/executors/scheduler/thread_scheduler.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/task_context.h>

#include <algorithm>
#include <any>
#include <iostream>

namespace dftracer::utils {
ThreadScheduler::ThreadScheduler() = default;

ThreadScheduler::~ThreadScheduler() { shutdown(); }

void ThreadScheduler::initialize(std::size_t num_threads) {
    shutdown();

    running_ = true;
    pending_count_ = 0;

    {
        std::lock_guard<std::mutex> results_lock(results_mutex_);
        remaining_deps_.clear();
        dependents_.clear();
        dependencies_.clear();
        dependent_count_.clear();
        tasks_with_futures_.clear();
        promises_.clear();
    }

    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        while (!ready_queue_.empty()) {
            ready_queue_.pop();
        }
    }

    workers_.clear();
    workers_.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&ThreadScheduler::worker_thread, this);
    }
}

void ThreadScheduler::reset() {
    Scheduler::reset();

    remaining_deps_.clear();
    dependents_.clear();
    dependencies_.clear();
    dependent_count_.clear();
    tasks_with_futures_.clear();
    promises_.clear();

    if (current_execution_context_) {
        current_execution_context_->reset();
    }
}

void ThreadScheduler::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        running_ = false;
    }
    queue_cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    workers_.clear();
    pending_count_ = 0;

    DFTRACER_UTILS_LOG_DEBUG("%s", "ThreadScheduler shutdown complete");
}

PipelineOutput ThreadScheduler::execute(const Pipeline& pipeline,
                                        const std::any& input) {
    ExecutorContext execution_context(&pipeline);
    current_pipeline_ = &pipeline;
    current_execution_context_ = &execution_context;

    if (!execution_context.validate()) {
        throw PipelineError(PipelineError::VALIDATION_ERROR,
                            "Pipeline validation failed");
    }

    execution_context.initialize_task_tracking();

    completed_tasks_ = 0;
    total_tasks_ = pipeline.size();
    current_pipeline_name_ = pipeline.get_name();

    setup_dependencies(pipeline);

    queue_ready_tasks(pipeline, input);

    {
        std::unique_lock<std::mutex> lock(done_mutex_);
        done_cv_.wait(lock, [this] { return pending_count_ == 0; });

        while (true) {
            {
                std::lock_guard<std::mutex> queue_lock(queue_mutex_);
                if (ready_queue_.empty() && pending_count_ == 0) {
                    break;
                }
            }
            done_cv_.wait(lock, [this] { return pending_count_ == 0; });
        }
    }

    auto results = extract_terminal_outputs(pipeline);

    current_pipeline_ = nullptr;
    current_execution_context_ = nullptr;

    return results;
}

void ThreadScheduler::submit_dynamic_task(TaskIndex task_id, Task* task_ptr,
                                          const std::any& input) {
    if (!running_ || !current_execution_context_) {
        return;
    }

    std::lock_guard<std::mutex> lock(queue_mutex_);

    tasks_with_futures_.insert(task_id);
    total_tasks_++;

    const auto& deps =
        current_execution_context_->get_dynamic_dependencies(task_id);
    if (deps.empty()) {
        ready_queue_.push(
            {task_id, task_ptr, std::move(const_cast<std::any&>(input))});
        pending_count_++;
        queue_cv_.notify_one();
    } else {
        remaining_deps_[task_id] = static_cast<int>(deps.size());
        dependencies_[task_id] = deps;

        for (TaskIndex dep : deps) {
            dependents_[dep].push_back(task_id);
            dependent_count_[dep]++;
        }

        bool all_satisfied = true;
        for (TaskIndex dep : deps) {
            if (!current_execution_context_->is_task_completed(dep)) {
                all_satisfied = false;
                break;
            }
        }

        if (all_satisfied) {
            std::shared_ptr<std::any> task_input;
            if (deps.size() == 1) {
                task_input = std::make_shared<std::any>(
                    current_execution_context_->get_task_output(deps[0]));
            } else {
                std::vector<std::any> combined_inputs;
                for (TaskIndex dep : deps) {
                    combined_inputs.push_back(
                        current_execution_context_->get_task_output(dep));
                }
                task_input =
                    std::make_shared<std::any>(std::move(combined_inputs));
            }

            ready_queue_.push({task_id, task_ptr, task_input});
            pending_count_++;
            queue_cv_.notify_one();
        }
    }
}

void ThreadScheduler::add_dynamic_dependency_tracking(
    TaskIndex task_id, const std::vector<TaskIndex>& dependencies) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    if (!dependencies.empty()) {
        remaining_deps_[task_id] = static_cast<int>(dependencies.size());
        dependencies_[task_id] = dependencies;

        for (TaskIndex dep : dependencies) {
            dependents_[dep].push_back(task_id);
            dependent_count_[dep]++;
        }
    }
}

void ThreadScheduler::worker_thread() {
    while (running_) {
        TaskItem task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(
                lock, [this] { return !running_ || !ready_queue_.empty(); });

            if (!running_) {
                break;
            }

            if (!ready_queue_.empty()) {
                task = std::move(ready_queue_.front());
                ready_queue_.pop();
            } else {
                continue;
            }
        }

        try {
            std::any result;

            if (task.task_ptr) {
                TaskContext task_context(this, current_execution_context_,
                                         task.task_id);
                if (task.task_ptr->needs_context()) {
                    task.task_ptr->setup_context(&task_context);
                }

                result = task.task_ptr->execute(
                    const_cast<std::any&>(task.get_input()));
                DFTRACER_UTILS_LOG_DEBUG("Worker executed task %d",
                                         task.task_id);
            } else {
                DFTRACER_UTILS_LOG_WARN("No task pointer for task %d",
                                        task.task_id);
                result = task.get_input();
            }

            {
                std::lock_guard<std::mutex> lock(results_mutex_);

                // Check if this is a pipeline task
                bool is_pipeline_task =
                    current_pipeline_ &&
                    task.task_id <
                        static_cast<TaskIndex>(current_pipeline_->size());

                if (is_pipeline_task) {
                    bool has_dependents =
                        current_execution_context_
                            ? !current_execution_context_
                                   ->get_task_dependents(task.task_id)
                                   .empty()
                            : false;

                    if (has_dependents) {
                        // Has dependents - must copy for both ExecutorContext
                        // and promise
                        if (current_execution_context_) {
                            current_execution_context_->set_task_output(
                                task.task_id, result);
                        }
                        current_pipeline_->fulfill_promise(task.task_id,
                                                           result);
                    } else {
                        // No dependents - can move to promise, skip
                        // ExecutorContext storage for non-terminal tasks
                        bool is_terminal =
                            current_execution_context_
                                ? current_execution_context_->is_terminal_task(
                                      task.task_id)
                                : false;
                        if (current_execution_context_ && is_terminal) {
                            current_execution_context_->set_task_output(
                                task.task_id, result);
                        }
                        current_pipeline_->fulfill_promise(task.task_id,
                                                           std::move(result));
                    }
                } else {
                    if (current_execution_context_) {
                        current_execution_context_->fulfill_dynamic_promise(
                            task.task_id, std::move(result));
                    }
                }

                if (current_execution_context_) {
                    current_execution_context_->mark_task_completed(
                        task.task_id);
                }
            }

            report_progress(task.task_id, current_pipeline_name_);
            process_completion(task.task_id);

        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR(
                "Exception in worker thread executing task %d: %s",
                task.task_id, e.what());

            {
                std::lock_guard<std::mutex> lock(results_mutex_);

                // Store empty result for dependency chain and mark completed
                if (current_execution_context_) {
                    current_execution_context_->set_task_output(task.task_id,
                                                                std::any{});
                    current_execution_context_->mark_task_completed(
                        task.task_id);
                }

                // Check if this is a pipeline task
                bool is_pipeline_task =
                    current_pipeline_ &&
                    task.task_id <
                        static_cast<TaskIndex>(current_pipeline_->size());

                if (is_pipeline_task) {
                    // Fulfill promise with exception for pipeline tasks
                    current_pipeline_->fulfill_promise_exception(
                        task.task_id, std::current_exception());
                } else {
                    // Fulfill promise with exception for dynamic tasks
                    if (current_execution_context_) {
                        current_execution_context_
                            ->fulfill_dynamic_promise_exception(
                                task.task_id, std::current_exception());
                    }
                }
            }
            report_progress(task.task_id, current_pipeline_name_);
            process_completion(task.task_id);
        }

        if (--pending_count_ == 0) {
            std::lock_guard<std::mutex> lock(done_mutex_);
            done_cv_.notify_all();
        }
    }

    DFTRACER_UTILS_LOG_DEBUG("Worker thread terminated", "");
}

void ThreadScheduler::process_completion(TaskIndex completed_task) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    if (auto it = dependents_.find(completed_task); it != dependents_.end()) {
        for (TaskIndex dependent : it->second) {
            if (--remaining_deps_[dependent] == 0) {
                auto deps = get_dependencies(dependent);
                std::shared_ptr<std::any> input_ptr;

                if (deps.size() == 1) {
                    input_ptr = std::make_shared<std::any>(
                        current_execution_context_->get_task_output(deps[0]));
                } else {
                    std::vector<std::any> combined_inputs;
                    for (TaskIndex dep : deps) {
                        combined_inputs.push_back(
                            current_execution_context_->get_task_output(dep));
                    }
                    input_ptr =
                        std::make_shared<std::any>(std::move(combined_inputs));
                }

                ready_queue_.push({dependent, get_task(dependent), input_ptr});
                pending_count_++;
                queue_cv_.notify_one();
            }
        }
    }
}

void ThreadScheduler::setup_dependencies(const Pipeline& pipeline) {
    for (TaskIndex i = 0; i < static_cast<TaskIndex>(pipeline.size()); ++i) {
        const auto& deps = pipeline.get_task_dependencies(i);
        remaining_deps_[i] = static_cast<int>(deps.size());
        dependencies_[i] = deps;

        for (TaskIndex dep : deps) {
            dependents_[dep].push_back(i);
            dependent_count_[dep]++;
        }
    }
}

void ThreadScheduler::queue_ready_tasks(const Pipeline& pipeline,
                                        const std::any& input) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    auto input_storage = std::make_shared<std::any>(input);
    for (TaskIndex i = 0; i < static_cast<TaskIndex>(pipeline.size()); ++i) {
        if (remaining_deps_[i] == 0) {
            ready_queue_.push({i, pipeline.get_task(i), input_storage});
            pending_count_++;
        }
    }

    queue_cv_.notify_all();
}

PipelineOutput ThreadScheduler::extract_terminal_outputs(
    const Pipeline& pipeline) {
    PipelineOutput terminal_outputs;

    std::vector<TaskIndex> terminal_tasks;
    for (TaskIndex i = 0; i < static_cast<TaskIndex>(pipeline.size()); ++i) {
        if (pipeline.get_task_dependents(i).empty()) {
            terminal_tasks.push_back(i);
        }
    }

    if (terminal_tasks.empty()) {
        terminal_outputs[-1] = std::any{};
    } else {
        for (TaskIndex id : terminal_tasks) {
            if (current_execution_context_) {
                terminal_outputs[id] =
                    current_execution_context_->consume_task_output(id);
            } else {
                terminal_outputs[id] = std::any{};
            }
        }
    }

    return terminal_outputs;
}

Task* ThreadScheduler::get_task(TaskIndex task_id) const {
    if (current_pipeline_) {
        Task* pipeline_task = current_pipeline_->get_task(task_id);
        if (pipeline_task) {
            return pipeline_task;
        }
    }

    return current_execution_context_
               ? current_execution_context_->get_dynamic_task(task_id)
               : nullptr;
}

std::vector<TaskIndex> ThreadScheduler::get_dependencies(
    TaskIndex task_id) const {
    if (auto it = dependencies_.find(task_id); it != dependencies_.end()) {
        return it->second;
    }

    if (current_execution_context_) {
        return current_execution_context_->get_dynamic_dependencies(task_id);
    }

    return {};
}

}  // namespace dftracer::utils
