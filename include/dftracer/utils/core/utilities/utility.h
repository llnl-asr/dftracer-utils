#ifndef DFTRACER_UTILS_CORE_UTILITIES_UTILITY_H
#define DFTRACER_UTILS_CORE_UTILITIES_UTILITY_H

#include <dftracer/utils/core/common/type_name.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeindex>

#ifdef __GNUG__
#include <cxxabi.h>

#include <cstdlib>
#endif

namespace dftracer::utils {
// Forward declaration
class TaskContext;
}  // namespace dftracer::utils

namespace dftracer::utils::utilities {

// Forward declare tags namespace
namespace tags {
struct NeedsContext;
}  // namespace tags

/**
 * @brief Base class for composable utilities with tag-based features.
 *
 * Utilities are reusable processing components that can be integrated with
 * the pipeline/task system. They support optional features via tags:
 * - tags::NeedsContext: Enables dynamic task emission
 * - tags::Parallelizable: Marks utility as thread-safe for parallel execution
 * - tags::Cacheable: Enables automatic result caching (with behaviors)
 * - tags::Retryable: Enables automatic retry on failure (with behaviors)
 * - tags::Monitored: Enables execution monitoring/logging (with behaviors)
 *
 * @tparam I Input type
 * @tparam O Output type
 * @tparam Tags Variadic list of tag types for opt-in features
 *
 * Usage:
 * @code
 * // Simple utility without context
 * class MyUtility : public Utility<std::string, int> {
 *     int process(const std::string& input) override {
 *         return std::stoi(input);
 *     }
 * };
 *
 * // Utility with dynamic emission (use context() to access TaskContext)
 * class ParallelUtility : public Utility<std::string, int, tags::NeedsContext>
 * { int process(const std::string& input) override {
 *         // Access context via protected context() method
 *         auto& ctx = context();
 *         ctx.emit<int, int>(...);  // Emit dynamic tasks
 *         return result;
 *     }
 * };
 * @endcode
 */
// Forward declarations for friend access
namespace behaviors {
template <typename I, typename O, typename... Tags>
class UtilityExecutor;
}

template <typename I, typename O, typename... Tags>
class Utility {
   private:
    std::tuple<Tags...> tags_;
    TaskContext* ctx_ = nullptr;  // Optional context reference
    std::string name_;            // User-provided name (or empty)
    std::string type_signature_;  // Auto-generated type representation

    // Friend declaration: only UtilityExecutor can set context
    friend class behaviors::UtilityExecutor<I, O, Tags...>;

   public:
    using Input = I;
    using Output = O;
    using TagsTuple = std::tuple<Tags...>;

    /**
     * @brief Default constructor using default tag values.
     */
    Utility() : name_(), type_signature_(generate_type_signature()) {}

    /**
     * @brief Constructor with explicit tag initialization.
     * @param tags Tag instances with custom configuration
     *
     * Only enabled when there are tags (sizeof...(Tags) > 0)
     */
    template <typename Dummy = void,
              typename = std::enable_if_t<(sizeof...(Tags) > 0) &&
                                          std::is_void_v<Dummy>>>
    explicit Utility(Tags... tags)
        : tags_(std::make_tuple(std::move(tags)...)),
          name_(),
          type_signature_(generate_type_signature()) {}

    virtual ~Utility() = default;

    // Non-copyable but moveable
    Utility(const Utility&) = delete;
    Utility& operator=(const Utility&) = delete;
    Utility(Utility&&) = default;
    Utility& operator=(Utility&&) = default;

    /**
     * @brief Process input.
     *
     * Override this method to implement utility logic.
     * If utility needs TaskContext (has NeedsContext tag), use context() to
     * access it.
     *
     * @param input Input data to process
     * @return Processed output
     */
    virtual O process([[maybe_unused]] const I& input) = 0;

   protected:
    /**
     * @brief Get TaskContext reference (only for utilities with NeedsContext
     * tag).
     *
     * Use this method to access TaskContext for dynamic task emission.
     *
     * @return Reference to TaskContext
     * @throws std::runtime_error if context not available
     *
     * Usage:
     * @code
     * class MyUtility : public Utility<int, int, NeedsContext> {
     *     int process(const int& input) override {
     *         auto& ctx = context();
     *         ctx.emit<int, int>(...);
     *         return result;
     *     }
     * };
     * @endcode
     */
    TaskContext& context() {
        // Compile-time check: Utility must have NeedsContext tag
        static_assert(
            has_tag<tags::NeedsContext>(),
            "Utility must have tags::NeedsContext to access TaskContext! "
            "Add tags::NeedsContext to your Utility class template "
            "parameters.");

        // Runtime check: Context must be set by executor
        if (!ctx_) {
            throw std::runtime_error(
                "TaskContext not available. Utility has NeedsContext tag but "
                "context was not set. Ensure utility is executed via "
                "UtilityExecutor "
                "or pipeline with proper executor.");
        }
        return *ctx_;
    }

   private:
    /**
     * @brief Set TaskContext reference.
     *
     * @param ctx TaskContext reference to store
     */
    void set_context(TaskContext& ctx) { ctx_ = &ctx; }

    /**
     * @brief Clear TaskContext reference.
     */
    void clear_context() { ctx_ = nullptr; }

   public:
    /**
     * @brief Check if utility has a specific tag at compile-time.
     *
     * @tparam Tag The tag type to check for
     * @return true if the utility has the specified tag, false otherwise
     *
     * Usage:
     * @code
     * if constexpr (MyUtility::has_tag<tags::NeedsContext>()) {
     *     // Handle context-aware utility
     * }
     * @endcode
     */
    template <typename Tag>
    static constexpr bool has_tag() {
        return (std::is_same_v<Tag, Tags> || ...);
    }

    /**
     * @brief Get const reference to tag configuration.
     *
     * @tparam Tag The tag type to retrieve
     * @return Const reference to the tag instance
     * @throws std::bad_variant_access if tag doesn't exist (compile-time check
     * recommended)
     */
    template <typename Tag>
    const Tag& get_tag() const {
        return std::get<Tag>(tags_);
    }

    /**
     * @brief Get mutable reference to tag configuration.
     *
     * @tparam Tag The tag type to retrieve
     * @return Mutable reference to the tag instance
     * @throws std::bad_variant_access if tag doesn't exist (compile-time check
     * recommended)
     */
    template <typename Tag>
    Tag& get_tag() {
        return std::get<Tag>(tags_);
    }

    /**
     * @brief Set tag configuration.
     *
     * @tparam Tag The tag type to set
     * @param tag The new tag configuration
     */
    template <typename Tag>
    void set_tag(Tag tag) {
        std::get<Tag>(tags_) = std::move(tag);
    }

    /**
     * @brief Get utility name formatted as: "NAME Utility[I->O]" or just
     * "Utility[I->O]"
     */
    std::string get_name() const {
        if (name_.empty()) {
            return type_signature_;
        } else {
            return name_ + " " + type_signature_;
        }
    }

    /**
     * @brief Get raw user-provided name (empty if not provided)
     */
    const std::string& get_user_name() const { return name_; }

    /**
     * @brief Get type signature (always available)
     */
    const std::string& get_type_signature() const { return type_signature_; }

    /**
     * @brief Set utility name
     */
    void set_name(std::string name) { name_ = std::move(name); }

   private:
    /**
     * @brief Generate type signature from input/output types
     */
    std::string generate_type_signature() const {
        std::ostringstream oss;

        // Get input type name
        std::string input_name;
        if (typeid(I) == typeid(void)) {
            input_name = "void";
        } else {
            // Demangle the type name and extract class name
            std::string full_input = demangle_type_name(typeid(I));
            input_name = extract_class_name(full_input);
        }

        // Get output type name
        std::string output_name;
        if (typeid(O) == typeid(void)) {
            output_name = "void";
        } else {
            std::string full_output = demangle_type_name(typeid(O));
            output_name = extract_class_name(full_output);
        }

        oss << "Utility[" << input_name << "->" << output_name << "]";

        return oss.str();
    }

    /**
     * @brief Helper to demangle a type_index name
     */
    static std::string demangle_type_name(const std::type_info& type) {
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
};

}  // namespace dftracer::utils::utilities

#endif  // DFTRACER_UTILS_CORE_UTILITIES_UTILITY_H
