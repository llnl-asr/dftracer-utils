#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_TAG_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_TAG_H

#include <dftracer/utils/core/common/typedefs.h>

#include <any>
#include <functional>
#include <memory>
#include <typeindex>
#include <variant>

namespace dftracer::utils {

template <typename T>
struct Input {
   private:
    std::variant<std::reference_wrapper<const T>, T> data_;

   public:
    static Input ref(const T& val) { return Input(val); }

    static Input copy(const T& val) {
        Input input;
        input.data_ = val;
        return input;
    }

    static Input move(T&& val) { return Input(std::move(val), true); }

    const T& value() const {
        return std::holds_alternative<std::reference_wrapper<const T>>(data_)
                   ? std::get<std::reference_wrapper<const T>>(data_).get()
                   : std::get<T>(data_);
    }

    explicit Input(const T& val)
        : data_(std::reference_wrapper<const T>(val)) {}

   private:
    Input() = default;
    Input(T&& val, bool) : data_(std::move(val)) {}
};

struct DependsOn {
    TaskIndex id;
    explicit DependsOn(TaskIndex task_id) : id(task_id) {}
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_TAG_H
