#ifndef DFTRACER_UTILS_CORE_TASKS_TYPED_TASK_H
#define DFTRACER_UTILS_CORE_TASKS_TYPED_TASK_H

#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>

#include <iostream>
#include <stdexcept>
#include <typeindex>

namespace dftracer::utils {

/**
 * TypedTask - A type-safe task wrapper with compile-time type checking
 *
 * This class provides a strongly-typed interface for tasks with known input
 * and output types. It's an alternative to using lambdas with Task directly.
 *
 * Example usage:
 *
 *   class MyTask : public TypedTask<int, std::string> {
 *   public:
 *       std::string apply(TaskContext& ctx, const int& input) override {
 *           return "Result: " + std::to_string(input * 2);
 *       }
 *   };
 *
 *   auto task = std::make_shared<MyTask>();
 *
 * Note: For simple cases, prefer using Task with lambdas:
 *   auto task = make_task([](int x) { return "Result: " + std::to_string(x *
 * 2); });
 */
template <typename I, typename O>
class TypedTask : public Task {
   protected:
    /**
     * Protected constructor - inherit from this class to create typed tasks
     */
    TypedTask()
        : Task([this](TaskContext& ctx, const std::any& input) -> std::any {
              try {
                  if constexpr (std::is_void_v<I>) {
                      // No input case
                      if constexpr (std::is_void_v<O>) {
                          apply(ctx);
                          return std::any{};
                      } else {
                          return std::any(apply(ctx));
                      }
                  } else {
                      // Has input
                      const I& typed_input = std::any_cast<const I&>(input);
                      if constexpr (std::is_void_v<O>) {
                          apply(ctx, typed_input);
                          return std::any{};
                      } else {
                          return std::any(apply(ctx, typed_input));
                      }
                  }
              } catch (const std::bad_any_cast& e) {
                  std::cerr << "TypedTask: bad_any_cast - expected type "
                            << typeid(I).name() << ", got type "
                            << input.type().name() << std::endl;
                  throw;
              }
          }) {}

   public:
    virtual ~TypedTask() = default;

    /**
     * Type-safe execution method - override this in your subclass
     *
     * Variants based on whether input/output are void:
     * - O apply(TaskContext&, const I&)      - Has input and output
     * - void apply(TaskContext&, const I&)   - Has input, no output
     * - O apply(TaskContext&)                 - No input, has output
     * - void apply(TaskContext&)              - No input or output
     */

    // Version with input and output
    template <typename I_ = I, typename O_ = O>
    typename std::enable_if_t<!std::is_void_v<I_> && !std::is_void_v<O_>, O_>
    apply(TaskContext& ctx, const I_& input) {
        static_assert(std::is_same_v<I_, I> && std::is_same_v<O_, O>,
                      "Template parameters must match class parameters");
        // Default implementation - should be overridden
        throw std::runtime_error("TypedTask::apply() must be overridden");
    }

    // Version with input, no output
    template <typename I_ = I, typename O_ = O>
    typename std::enable_if_t<!std::is_void_v<I_> && std::is_void_v<O_>, void>
    apply(TaskContext& ctx, const I_& input) {
        static_assert(std::is_same_v<I_, I> && std::is_same_v<O_, O>,
                      "Template parameters must match class parameters");
        DFTRACER_UTILS_LOG_ERROR("%s", "TypedTask::apply() not overridden");
        throw std::runtime_error("TypedTask::apply() must be overridden");
    }

    // Version with output, no input
    template <typename I_ = I, typename O_ = O>
    typename std::enable_if_t<std::is_void_v<I_> && !std::is_void_v<O_>, O_>
    apply(TaskContext& ctx) {
        static_assert(std::is_same_v<I_, I> && std::is_same_v<O_, O>,
                      "Template parameters must match class parameters");
        DFTRACER_UTILS_LOG_ERROR("%s", "TypedTask::apply() not overridden");
        throw std::runtime_error("TypedTask::apply() must be overridden");
    }

    // Version with no input or output
    template <typename I_ = I, typename O_ = O>
    typename std::enable_if_t<std::is_void_v<I_> && std::is_void_v<O_>, void>
    apply(TaskContext& ctx) {
        static_assert(std::is_same_v<I_, I> && std::is_same_v<O_, O>,
                      "Template parameters must match class parameters");
        DFTRACER_UTILS_LOG_ERROR("%s", "TypedTask::apply() not overridden");
        throw std::runtime_error("TypedTask::apply() must be overridden");
    }

   protected:
    /**
     * Validate input type at runtime (for debugging)
     */
    bool validate(const I& in) {
        bool result = std::type_index(typeid(in)) == this->get_input_type();
        if (!result) {
            std::cerr << "Input type validation failed, expected: "
                      << this->get_input_type().name()
                      << ", got: " << typeid(in).name() << std::endl;
        }
        return result;
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TYPED_TASK_H
