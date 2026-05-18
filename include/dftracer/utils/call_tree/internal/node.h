#ifndef DFTRACER_UTILS_CALL_TREE_INTERNAL_NODE_H
#define DFTRACER_UTILS_CALL_TREE_INTERNAL_NODE_H

#include <dftracer/utils/utilities/composites/dft/args_map.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::call_tree {
namespace internal {

using ArgsMap = dftracer::utils::utilities::composites::dft::ArgsMap;

// name_ and category_ are non-owning views into a process-wide StringIntern
// pool. ArgsMap interns its own keys.
class CallTreeNode {
   public:
    CallTreeNode();
    CallTreeNode(std::uint64_t id, std::string_view name,
                 std::string_view category);
    ~CallTreeNode();

    CallTreeNode(const CallTreeNode&) = delete;
    CallTreeNode& operator=(const CallTreeNode&) = delete;

    CallTreeNode(CallTreeNode&& other) noexcept;
    CallTreeNode& operator=(CallTreeNode&& other) noexcept;

    void initialize(std::uint64_t id, std::string_view name,
                    std::string_view category, std::uint64_t start_time,
                    std::uint64_t duration, int level);

    void cleanup();

    std::uint64_t get_id() const { return id_; }
    std::string_view get_name() const { return name_; }
    std::string_view get_category() const { return category_; }
    std::uint64_t get_start_time() const { return start_time_; }
    std::uint64_t get_duration() const { return duration_; }
    int get_level() const { return level_; }
    std::uint64_t get_parent_id() const { return parent_id_; }
    const ArgsMap& get_args() const { return args_; }
    ArgsMap& mut_args() { return args_; }
    const std::vector<std::uint64_t>& get_children() const { return children_; }

    void set_parent_id(std::uint64_t parent_id) { parent_id_ = parent_id; }
    void add_child(std::uint64_t child_id) { children_.push_back(child_id); }
    void set_args(ArgsMap args) { args_ = std::move(args); }

   private:
    std::uint64_t id_;
    std::string_view name_;
    std::string_view category_;
    std::uint64_t start_time_;
    std::uint64_t duration_;
    int level_;
    std::uint64_t parent_id_;
    ArgsMap args_;
    std::vector<std::uint64_t> children_;
    bool initialized_;
    bool cleaned_up_;
};

using FunctionCall = CallTreeNode;

}  // namespace internal
}  // namespace dftracer::utils::call_tree

#endif  // DFTRACER_UTILS_CALL_TREE_INTERNAL_NODE_H
