#ifndef DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_RESULT_H
#define DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_RESULT_H

#include <dftracer/utils/core/common/error.h>

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace dftracer::utils::task_graph {

/**
 * TaskResult<T> - Container for task outputs
 *
 * Provides a consistent API for task data passing:
 * - Zero-copy reads via get()
 * - Explicit copies via copy()
 * - Future-proof for disk spilling without API changes
 *
 * Design rationale:
 * - Wraps data in shared_ptr for zero-copy between tasks
 * - Consumer explicitly copies if mutation needed
 * - size_bytes() enables smart memory management decisions
 * - Future: can add spill_to_disk() without changing user code
 */
template <typename T>
class TaskResult {
   public:
    /**
     * Create TaskResult by moving value into shared storage
     */
    static TaskResult<T> make(T&& value) {
        TaskResult<T> result;
        result.data_ = std::make_shared<T>(std::move(value));
        return result;
    }

    /**
     * Create TaskResult by copying value into shared storage
     */
    static TaskResult<T> make(const T& value) {
        TaskResult<T> result;
        result.data_ = std::make_shared<T>(value);
        return result;
    }

    /**
     * Create TaskResult from existing shared_ptr
     */
    static TaskResult<T> from_shared(std::shared_ptr<T> ptr) {
        TaskResult<T> result;
        result.data_ = std::move(ptr);
        return result;
    }

    /**
     * Default constructor - creates empty result
     */
    TaskResult() = default;

    /**
     * Zero-copy read access
     *
     * Returns const reference to underlying data.
     * Future: may trigger load from disk if spilled.
     */
    const T& get() const {
        if (!data_) {
            throw DFTUtilsException(ErrorCode::INTERNAL,
                                    "TaskResult::get() called on empty result");
        }
        return *data_;
    }

    /**
     * Explicit copy for when mutation is needed
     *
     * Consumer calls this when they need to modify the data.
     * Returns owned copy that can be mutated.
     */
    T copy() const {
        if (!data_) {
            throw DFTUtilsException(
                ErrorCode::INTERNAL,
                "TaskResult::copy() called on empty result");
        }
        return T(*data_);
    }

    /**
     * Get shared ownership of data
     *
     * Use when you need to extend lifetime or share with others.
     */
    std::shared_ptr<T> share() const { return data_; }

    /**
     * Check if data is ready (not empty, not spilled)
     */
    bool is_ready() const { return data_ != nullptr; }

    /**
     * Check if result is empty
     */
    bool empty() const { return data_ == nullptr; }

    /**
     * Estimate size in bytes for memory tracking
     *
     * Default implementation uses sizeof(T).
     * Specialize for types with dynamic allocation.
     */
    std::size_t size_bytes() const {
        if (!data_) return 0;
        return estimate_size(*data_);
    }

    /**
     * Boolean conversion - true if not empty
     */
    explicit operator bool() const { return is_ready(); }

   private:
    std::shared_ptr<T> data_;

    // Default size estimation
    template <typename U>
    static std::size_t estimate_size(const U& value) {
        // For simple types, use sizeof
        // Users can specialize for containers, etc.
        if constexpr (requires { value.size(); }) {
            // Container-like types
            return sizeof(U) + value.size() * sizeof(typename U::value_type);
        } else {
            return sizeof(U);
        }
    }
};

/**
 * Specialization for void - no data, just completion signal
 */
template <>
class TaskResult<void> {
   public:
    static TaskResult<void> make() { return TaskResult<void>{}; }

    bool is_ready() const { return true; }
    bool empty() const { return false; }
    std::size_t size_bytes() const { return 0; }
    explicit operator bool() const { return true; }
};

}  // namespace dftracer::utils::task_graph

#endif  // DFTRACER_UTILS_CORE_TASK_GRAPH_TASK_RESULT_H
