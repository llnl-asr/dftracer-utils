#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/pipeline/executors/executor_context.h>
#include <dftracer/utils/core/pipeline/executors/scheduler/sequential_scheduler.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/task_context.h>

#include <any>
#include <unordered_map>

namespace dftracer::utils {

SequentialScheduler::SequentialScheduler()
    : current_execution_context_(nullptr), current_pipeline_(nullptr) {}

PipelineOutput SequentialScheduler::execute(const Pipeline& pipeline,
                                            const std::any& input) {
    ExecutorContext execution_context(&pipeline);
    current_execution_context_ = &execution_context;
    current_pipeline_ = &pipeline;

    if (!execution_context.validate()) {
        throw PipelineError(PipelineError::VALIDATION_ERROR,
                            "Pipeline validation failed");
    }

    execution_context.initialize_task_tracking();

    task_completed_.clear();
    dependency_count_.clear();
    while (!task_queue_.empty()) {
        task_queue_.pop();
    }

    total_tasks_ = pipeline.size();
    completed_tasks_ = 0;
    current_pipeline_name_ = pipeline.get_name();

    TaskIndex initial_pipeline_size = static_cast<TaskIndex>(pipeline.size());

    for (TaskIndex i = 0; i < initial_pipeline_size; ++i) {
        task_completed_[i] = false;
        dependency_count_[i] =
            execution_context.get_task_dependencies(i).size();
    }

    for (TaskIndex i = 0; i < initial_pipeline_size; ++i) {
        if (execution_context.get_task_dependencies(i).empty()) {
            execute_task_with_dependencies(execution_context, i, input);
        }
    }

    std::vector<TaskIndex> terminal_tasks;
    for (TaskIndex i = 0; i < static_cast<TaskIndex>(pipeline.size()); ++i) {
        if (execution_context.get_task_dependents(i).empty()) {
            terminal_tasks.push_back(i);
        }
    }

    process_all_tasks();

    current_execution_context_ = nullptr;

    PipelineOutput terminal_outputs;

    if (terminal_tasks.empty()) {
        terminal_outputs[-1] = input;
    } else {
        for (TaskIndex id : terminal_tasks) {
            terminal_outputs[id] = execution_context.get_task_output(id);
        }
    }

    current_pipeline_ = nullptr;
    return terminal_outputs;
}

void SequentialScheduler::submit_dynamic_task(TaskIndex task_id, Task* task_ptr,
                                              const std::any& input) {
    if (!current_execution_context_) {
        return;
    }

    total_tasks_++;

    // For sequential execution: execute the task immediately instead of queuing
    // This allows emitted tasks to complete before the parent task returns,
    // avoiding deadlock when the parent calls future.get()
    try {
        std::any result;
        TaskContext task_context(this, current_execution_context_, task_id);

        if (task_ptr->needs_context()) {
            task_ptr->setup_context(&task_context);
        }

        DFTRACER_UTILS_LOG_DEBUG(
            "SequentialScheduler: Immediately executing dynamic task %d",
            task_id);
        result = task_ptr->execute(input);

        current_execution_context_->set_task_output(task_id, result);
        current_execution_context_->fulfill_dynamic_promise(task_id, result);
        current_execution_context_->mark_task_completed(task_id);
        task_completed_[task_id] = true;

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR(
            "SequentialScheduler: Exception executing dynamic task %d: %s",
            task_id, e.what());

        if (current_execution_context_) {
            current_execution_context_->fulfill_dynamic_promise_exception(
                task_id, std::current_exception());
            current_execution_context_->mark_task_completed(task_id);
        }
        task_completed_[task_id] = true;
    }
}

void SequentialScheduler::execute_task_with_dependencies(
    ExecutorContext& execution_context, TaskIndex task_id,
    const std::any& input) {
    Task* task_ptr = execution_context.get_task(task_id);
    task_queue_.emplace(task_id, task_ptr, std::move(input));
}

void SequentialScheduler::process_all_tasks() {
    while (!task_queue_.empty()) {
        TaskItem task = std::move(task_queue_.front());
        task_queue_.pop();

        try {
            std::any result;

            if (task.task_ptr) {
                TaskContext task_context(this, current_execution_context_,
                                         task.task_id);
                if (task.task_ptr->needs_context()) {
                    task.task_ptr->setup_context(&task_context);
                }

                DFTRACER_UTILS_LOG_DEBUG(
                    "SequentialScheduler: Executing task %d, input type %s",
                    task.task_id, task.input.type().name());
                result = task.task_ptr->execute(task.input);
                DFTRACER_UTILS_LOG_DEBUG(
                    "SequentialScheduler: Executed task %d", task.task_id);
            } else {
                DFTRACER_UTILS_LOG_WARN(
                    "SequentialScheduler: No task pointer for task %d, using "
                    "input as result",
                    task.task_id);
                result = task.input;
            }

            bool is_pipeline_task =
                current_pipeline_ &&
                task.task_id <
                    static_cast<TaskIndex>(current_pipeline_->size());

            current_execution_context_->set_task_output(task.task_id, result);

            if (is_pipeline_task) {
                current_pipeline_->fulfill_promise(task.task_id, result);
            } else {
                current_execution_context_->fulfill_dynamic_promise(
                    task.task_id, result);
            }

            task_completed_[task.task_id] = true;
            report_progress(task.task_id, current_pipeline_name_);

            // Trigger dependent tasks
            for (TaskIndex dependent :
                 current_execution_context_->get_task_dependents(
                     task.task_id)) {
                if (--dependency_count_[dependent] == 0) {
                    // All dependencies satisfied - submit the dependent task
                    std::any dependent_input;

                    if (current_execution_context_
                            ->get_task_dependencies(dependent)
                            .size() == 1) {
                        // Single dependency - consume the output
                        dependent_input =
                            current_execution_context_->consume_task_output(
                                task.task_id);
                    } else {
                        // Multiple dependencies - combine inputs
                        std::vector<std::any> combined_inputs;
                        for (TaskIndex dep :
                             current_execution_context_->get_task_dependencies(
                                 dependent)) {
                            combined_inputs.push_back(
                                current_execution_context_->consume_task_output(
                                    dep));
                        }
                        dependent_input = combined_inputs;
                    }

                    // Submit dependent task directly without callback
                    execute_task_with_dependencies(*current_execution_context_,
                                                   dependent,
                                                   std::move(dependent_input));
                }
            }

        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR(
                "SequentialScheduler: Exception executing task %d: %s",
                task.task_id, e.what());

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

            task_completed_[task.task_id] = true;
            report_progress(task.task_id, current_pipeline_name_);
        }

        // Process any dynamic tasks that were emitted during task execution
        // This adds them to the queue so the while loop will pick them up
        process_remaining_dynamic_tasks();
    }
}

void SequentialScheduler::process_remaining_dynamic_tasks() {
    if (!current_execution_context_) return;

    std::size_t dynamic_count =
        current_execution_context_->dynamic_task_count();
    std::size_t pipeline_size =
        current_execution_context_->get_pipeline()->size();

    for (std::size_t idx = 0; idx < dynamic_count; ++idx) {
        TaskIndex task_id = static_cast<TaskIndex>(pipeline_size + idx);
        if (current_execution_context_->is_task_completed(task_id)) {
            continue;
        }

        Task* task_ptr = current_execution_context_->get_dynamic_task(task_id);
        if (!task_ptr) continue;

        auto dependencies =
            current_execution_context_->get_dynamic_dependencies(task_id);

        bool all_deps_ready = true;
        for (TaskIndex dep_id : dependencies) {
            if (!current_execution_context_->is_task_completed(dep_id)) {
                all_deps_ready = false;
                break;
            }
        }

        if (!all_deps_ready) continue;

        std::any task_input;

        if (dependencies.empty()) {
            task_input = std::any{};  // Independent task
        } else if (dependencies.size() == 1) {
            task_input =
                current_execution_context_->get_task_output(dependencies[0]);
        } else {
            std::vector<std::any> combined_inputs;
            for (TaskIndex dep_id : dependencies) {
                combined_inputs.push_back(
                    current_execution_context_->get_task_output(dep_id));
            }
            task_input = combined_inputs;
        }

        task_queue_.emplace(task_id, task_ptr, std::move(task_input));
    }
}

}  // namespace dftracer::utils
