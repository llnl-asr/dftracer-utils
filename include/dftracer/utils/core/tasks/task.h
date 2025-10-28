#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_H

#include <dftracer/utils/core/common/type_name.h>
#include <dftracer/utils/core/common/typedefs.h>

#include <any>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <typeindex>
#include <vector>

#ifdef __GNUG__
#include <cxxabi.h>

#include <cstdlib>
#endif

namespace dftracer::utils {

class TaskContext;

/**
 * Task - Self-contained DAG node with dependencies
 *
 * Features:
 * - Fluent API for building DAG (.depends_on())
 * - Owns promise/future for result retrieval
 * - Knows parents and children (DAG structure)
 * - Immutable after construction (blueprint pattern)
 * - Type validation during edge creation
 * - Supports automatic tuple packing for multiple parents
 */
class Task : public std::enable_shared_from_this<Task> {
   private:
    std::string name_;            // User-provided name (or empty)
    std::string type_signature_;  // Auto-generated type representation
    std::function<std::any(TaskContext&, const std::any&)> func_;

    std::type_index input_type_;
    std::type_index output_type_;

    // DAG structure
    std::vector<std::shared_ptr<Task>> parents_;
    std::vector<std::shared_ptr<Task>> children_;
    std::atomic<int> pending_parents_count_{0};

    // Result management
    std::shared_ptr<std::promise<std::any>> promise_;
    std::shared_future<std::any> future_;

    // Optional combiner for multiple parents
    std::function<std::any(const std::vector<std::any>&)> input_combiner_;
    bool has_custom_combiner_{false};

    // Execution state
    std::atomic<bool> completed_{false};

    // Optional initial input (for tasks without dependencies)
    std::optional<std::any> initial_input_;

    // Optional timeout for this task (0 = no timeout)
    std::chrono::milliseconds timeout_{0};

   public:
    /**
     * Constructor with function that takes input and TaskContext
     */
    template <typename Func>
    explicit Task(Func&& func, std::string name = "")
        : name_(std::move(name)),
          func_(wrap_function(std::forward<Func>(func))),
          input_type_(deduce_input_type<Func>()),
          output_type_(deduce_output_type<Func>()),
          promise_(std::make_shared<std::promise<std::any>>()),
          future_(promise_->get_future().share()) {
        type_signature_ = generate_type_signature();
    }

    virtual ~Task() = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) = delete;
    Task& operator=(Task&&) = delete;

    /**
     * Add a single parent dependency
     */
    std::shared_ptr<Task> depends_on(std::shared_ptr<Task> parent);

    /**
     * Add multiple parent dependencies (initializer list)
     * Usage: task->depends_on({parent1, parent2, parent3, ...})
     */
    std::shared_ptr<Task> depends_on(
        std::initializer_list<std::shared_ptr<Task>> parents);

    /**
     * Add multiple parent dependencies (variadic, 2+ parents)
     * Usage: task->depends_on(parent1, parent2, parent3, ...)
     */
    template <typename T1, typename T2, typename... Rest>
    std::shared_ptr<Task> depends_on(T1&& parent1, T2&& parent2,
                                     Rest&&... rest) {
        depends_on(std::forward<T1>(parent1));
        depends_on(std::forward<T2>(parent2));
        (depends_on(std::forward<Rest>(rest)), ...);
        return shared_from_this();
    }

    /**
     * Set custom input combiner for multiple parents
     * Accepts a function that takes std::vector<std::any>&
     */
    std::shared_ptr<Task> with_combiner(
        std::function<std::any(const std::vector<std::any>&)> combiner);

    /**
     * Set custom input combiner for multiple parents (tuple-based with
     * std::function) Accepts a function that takes specific types Example:
     * with_combiner(std::function<std::any(int, std::string)>([](int a,
     * std::string b) { ... }))
     */
    template <typename... Args>
    std::shared_ptr<Task> with_combiner(
        std::function<std::any(Args...)> combiner);

    /**
     * Set custom input combiner for multiple parents (lambda/callable)
     * Automatically deduces argument types from lambda
     * Example: task->with_combiner([](int a, std::string b) -> std::any {
     * return a + b.size(); })
     */
    template <typename Func>
    auto with_combiner(Func&& combiner) -> std::enable_if_t<
        !std::is_same_v<std::decay_t<Func>,
                        std::function<std::any(const std::vector<std::any>&)>>,
        std::shared_ptr<Task>>;

    /**
     * Set task name
     */
    std::shared_ptr<Task> with_name(std::string name);

    /**
     * Set initial input for this task
     * This allows providing input directly to a task without using dependencies
     * @tparam T Input type (automatically deduced)
     * @param input The input value (will be converted to std::any internally)
     */
    template <typename T>
    std::shared_ptr<Task> with_input(T&& input) {
        initial_input_ = std::any(std::forward<T>(input));
        return shared_from_this();
    }

    /**
     * Set timeout for this task
     * @param timeout Timeout duration (0 = no timeout, wait forever)
     * @return This task (for method chaining)
     */
    std::shared_ptr<Task> with_timeout(std::chrono::milliseconds timeout) {
        timeout_ = timeout;
        return shared_from_this();
    }

    /**
     * Get task ID (pointer address)
     */
    TaskIndex get_id() const { return reinterpret_cast<TaskIndex>(this); }

    /**
     * Get future for this task's result
     */
    std::shared_future<std::any> get_future() const { return future_; }

    /**
     * Get task result with automatic type casting
     * @tparam T The expected result type
     * @return The task result cast to type T
     * @throws std::bad_any_cast if the result cannot be cast to T
     * @throws std::future_error if the task hasn't completed or threw an
     * exception
     */
    template <typename T>
    T get() const {
        return std::any_cast<T>(future_.get());
    }

    /**
     * Wait for task to complete without retrieving the result
     * This is useful for void tasks where you just want to wait for completion
     */
    void wait() const { future_.wait(); }

    /**
     * Get parent tasks (returns a copy for thread safety)
     */
    std::vector<std::shared_ptr<Task>> get_parents() const { return parents_; }

    /**
     * Get child tasks (returns a copy for thread safety)
     */
    std::vector<std::shared_ptr<Task>> get_children() const {
        return children_;
    }

    /**
     * Check if all parents have completed
     */
    bool is_ready() const { return pending_parents_count_ == 0; }

    /**
     * Check if task has completed
     */
    bool is_completed() const { return completed_.load(); }

    /**
     * Get input type
     */
    std::type_index get_input_type() const { return input_type_; }

    /**
     * Get output type
     */
    std::type_index get_output_type() const { return output_type_; }

    /**
     * Get task name formatted as: "NAME (type_signature)" or just
     * "(type_signature)"
     */
    std::string get_name() const {
        if (name_.empty()) {
            return type_signature_;
        } else {
            return name_ + " " + type_signature_;
        }
    }

    /**
     * Get raw user-provided name (empty if not provided)
     */
    const std::string& get_user_name() const { return name_; }

    /**
     * Get type signature (always available)
     */
    const std::string& get_type_signature() const { return type_signature_; }

    /**
     * Check if task has custom combiner
     */
    bool has_combiner() const { return has_custom_combiner_; }

    /**
     * Check if task has initial input set
     */
    bool has_initial_input() const { return initial_input_.has_value(); }

    /**
     * Get initial input (if set)
     */
    const std::optional<std::any>& get_initial_input() const {
        return initial_input_;
    }

    /**
     * Set initial input for this task
     */
    void set_initial_input(std::any input) {
        initial_input_ = std::move(input);
    }

    /**
     * Get task timeout (0 = no timeout)
     */
    std::chrono::milliseconds get_timeout() const { return timeout_; }

    /**
     * Check if task has timeout set
     */
    bool has_timeout() const { return timeout_.count() > 0; }

   private:
    /**
     * Execute task function with given input
     * (Internal - called by Executor)
     */
    std::any execute(TaskContext& context, const std::any& input);

    /**
     * Apply custom combiner to parent outputs
     * (Internal - called by Scheduler)
     */
    std::any apply_combiner(const std::vector<std::any>& inputs) const;

    /**
     * Decrement pending parents count
     * (Internal - called by Scheduler when parent completes)
     */
    void decrement_pending_parents() { --pending_parents_count_; }

    /**
     * Initialize pending parents count
     * (Internal - called by Scheduler before execution)
     */
    void initialize_pending_count() {
        pending_parents_count_ = static_cast<int>(parents_.size());
    }

    /**
     * Add child task
     * (Internal - called by depends_on)
     */
    void add_child(std::shared_ptr<Task> child) { children_.push_back(child); }

    /**
     * Validate type compatibility between tasks
     */
    bool validate_connection(std::type_index from, std::type_index to) const;

    /**
     * Fulfill promise with result (called by Executor)
     */
    void fulfill_promise(std::any result);

    /**
     * Fulfill promise with exception (called by Executor)
     */
    void fulfill_promise_exception(std::exception_ptr ex);

    /**
     * Wrap different function signatures to common signature
     */
    template <typename Func>
    std::function<std::any(TaskContext&, const std::any&)> wrap_function(
        Func&& func);

    /**
     * Type deduction helpers
     */
    template <typename Func>
    std::type_index deduce_input_type();

    template <typename Func>
    std::type_index deduce_output_type();

    /**
     * Generate type signature from input/output types
     */
    std::string generate_type_signature() const {
        std::ostringstream oss;

        // Get input type name
        std::string input_name;
        if (input_type_ == typeid(void)) {
            input_name = "void";
        } else {
            // Demangle the type name and extract class name
            std::string full_input = demangle_type_name(input_type_);
            input_name = extract_class_name(full_input);
        }

        // Get output type name
        std::string output_name;
        if (output_type_ == typeid(void)) {
            output_name = "void";
        } else {
            std::string full_output = demangle_type_name(output_type_);
            output_name = extract_class_name(full_output);
        }

        oss << "Task[" << input_name << "->" << output_name << "]";

        return oss.str();
    }

    /**
     * Helper to unpack vector<any> into tuple and call function
     */
    template <typename... Args, std::size_t... Is>
    static std::any unpack_and_call(
        const std::function<std::any(Args...)>& func,
        const std::vector<std::any>& inputs, std::index_sequence<Is...>) {
        return func(std::any_cast<Args>(inputs[Is])...);
    }

    /**
     * Helper to demangle a type_index name
     */
    static std::string demangle_type_name(std::type_index type) {
#ifdef __GNUG__
        int status = -1;
        std::unique_ptr<char, void (*)(void*)> res{
            abi::__cxa_demangle(type.name(), nullptr, nullptr, &status),
            std::free};
        return (status == 0) ? res.get() : type.name();
#else
        return type.name();
#endif
    }

    // Friends for internal access
    friend class Scheduler;
    friend class Executor;
};

/**
 * Helper function to create a shared_ptr<Task>
 */
template <typename Func>
std::shared_ptr<Task> make_task(Func&& func, std::string name = "") {
    return std::make_shared<Task>(std::forward<Func>(func), std::move(name));
}

}  // namespace dftracer::utils

// Include template implementations
#include "task_impl.h"

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_H
