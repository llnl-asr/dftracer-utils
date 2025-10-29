#ifndef DFTRACER_UTILS_CORE_TASKS_TYPED_TASK_H
#define DFTRACER_UTILS_CORE_TASKS_TYPED_TASK_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/tasks/task.h>

#include <typeindex>

namespace dftracer::utils {

template <typename I, typename O>
class TypedTask : public Task {
   protected:
    TypedTask() : Task(typeid(I), typeid(O)) {}

   public:
    virtual ~TypedTask() = default;

    I get_input(const std::any& in) { return std::any_cast<I>(in); }

    O get_output(const std::any& out) { return std::any_cast<O>(out); }

    virtual O apply(const I& in) = 0;

   protected:
    bool validate(const I& in) {
        bool result = std::type_index(typeid(in)) == this->get_input_type();
        if (!result) {
            DFTRACER_UTILS_LOG_ERROR(
                "Input type validation failed, expected: {}, got: {}",
                this->get_input_type().name(), typeid(in).name());
        }
        return result;
    }

   public:
    std::any execute(const std::any& in) override final {
        // Cast to const reference to avoid copy
        // std::any stores by value, so we cast to const reference
        try {
            return apply(std::any_cast<const I&>(in));
        } catch (const std::bad_any_cast& e) {
            DFTRACER_UTILS_LOG_ERROR(
                "TypedTask: bad_any_cast - expected type %s, got type %s",
                typeid(I).name(), in.type().name());
            throw;
        }
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TYPED_TASK_H
