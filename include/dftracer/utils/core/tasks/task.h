#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_H

#include <any>
#include <cstdint>
#include <string>
#include <typeindex>

namespace dftracer::utils {

class TaskContext;

class Task {
   private:
    std::type_index input_type_;
    std::type_index output_type_;

   protected:
    Task(std::type_index input_type, std::type_index output_type)
        : input_type_(input_type), output_type_(output_type) {}

   public:
    virtual ~Task() = default;
    virtual std::any execute(const std::any& in) = 0;

    virtual void setup_context(TaskContext*) {}
    virtual bool needs_context() const { return false; }

    std::type_index get_input_type() const { return input_type_; }
    std::type_index get_output_type() const { return output_type_; }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_H
