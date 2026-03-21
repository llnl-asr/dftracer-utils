#ifndef DFTRACER_UTILS_CORE_UTILITIES_UTILITY_H
#define DFTRACER_UTILS_CORE_UTILITIES_UTILITY_H

#include <dftracer/utils/core/common/type_name.h>
#include <dftracer/utils/core/coro/task.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeindex>

namespace dftracer::utils {
class CoroScope;
}  // namespace dftracer::utils

namespace dftracer::utils::utilities {

namespace tags {
struct NeedsContext;
struct Parallelizable;
}  // namespace tags

namespace behaviors {
template <typename I, typename O, typename... Tags>
class UtilityExecutor;
}

/**
 * @brief Shared machinery for all utility variants.
 *
 * Holds tags, context pointer, name, and type signature. Subclasses
 * (Utility, StreamingUtility) add their specific process() signatures.
 *
 * @tparam I Input type
 * @tparam Tags Variadic tag types for opt-in features
 */
template <typename I, typename... Tags>
class UtilityBase {
   private:
    std::tuple<Tags...> tags_;
    CoroScope* ctx_ = nullptr;
    std::string name_;
    std::string type_signature_;

   public:
    using Input = I;
    using TagsTuple = std::tuple<Tags...>;

    UtilityBase() : name_(), type_signature_(make_input_signature()) {}

    template <typename Dummy = void,
              typename = std::enable_if_t<(sizeof...(Tags) > 0) &&
                                          std::is_void_v<Dummy>>>
    explicit UtilityBase(Tags... tags)
        : tags_(std::make_tuple(std::move(tags)...)),
          name_(),
          type_signature_(make_input_signature()) {}

    virtual ~UtilityBase() = default;

    UtilityBase(const UtilityBase&) = delete;
    UtilityBase& operator=(const UtilityBase&) = delete;
    UtilityBase(UtilityBase&&) = default;
    UtilityBase& operator=(UtilityBase&&) = default;

    template <typename Tag>
    static constexpr bool has_tag() {
        return (std::is_same_v<Tag, Tags> || ...);
    }

    template <typename Tag>
    const Tag& get_tag() const {
        return std::get<Tag>(tags_);
    }

    template <typename Tag>
    Tag& get_tag() {
        return std::get<Tag>(tags_);
    }

    template <typename Tag>
    void set_tag(Tag tag) {
        std::get<Tag>(tags_) = std::move(tag);
    }

    std::string get_name() const {
        if (name_.empty()) return type_signature_;
        return name_ + " " + type_signature_;
    }

    const std::string& get_user_name() const { return name_; }
    const std::string& get_type_signature() const { return type_signature_; }
    void set_name(std::string name) { name_ = std::move(name); }

   protected:
    /**
     * @brief Access CoroScope (only valid when NeedsContext tag is present).
     */
    CoroScope& context() {
        static_assert(
            has_tag<tags::NeedsContext>(),
            "Utility must have tags::NeedsContext to access CoroScope! "
            "Add tags::NeedsContext to your Utility class template "
            "parameters.");
        if (!ctx_) {
            throw std::runtime_error(
                "CoroScope not available. Ensure utility is executed via "
                "UtilityExecutor or pipeline with proper executor.");
        }
        return *ctx_;
    }

    void set_context(CoroScope& ctx) { ctx_ = &ctx; }
    void clear_context() { ctx_ = nullptr; }

    /**
     * @brief Override the auto-generated type signature.
     *
     * Called by derived class constructors to set the full I->O form.
     */
    void set_type_signature(std::string sig) {
        type_signature_ = std::move(sig);
    }

    /**
     * @brief Build "Utility[I->O]" signature from two pre-formatted names.
     */
    static std::string make_signature(const std::string& input_name,
                                      const std::string& output_name) {
        std::ostringstream oss;
        oss << "Utility[" << input_name << "->" << output_name << "]";
        return oss.str();
    }

   private:
    static std::string make_input_signature() {
        std::ostringstream oss;
        oss << "Utility[" << type_label<I>() << "]";
        return oss.str();
    }

    template <typename T>
    static std::string type_label() {
        if constexpr (std::is_void_v<T>) {
            return "void";
        } else {
            return extract_class_name(get_type_name<T>());
        }
    }
};

/**
 * @brief Materialized utility: process() returns CoroTask<O>.
 *
 * @tparam I Input type
 * @tparam O Output type
 * @tparam Tags Variadic tag types
 */
template <typename I, typename O, typename... Tags>
class Utility : public UtilityBase<I, Tags...> {
   public:
    using Output = O;

    Utility() : UtilityBase<I, Tags...>() {
        this->set_type_signature(UtilityBase<I, Tags...>::make_signature(
            extract_class_name(get_type_name<I>()),
            extract_class_name(get_type_name<O>())));
    }

    template <typename Dummy = void,
              typename = std::enable_if_t<(sizeof...(Tags) > 0) &&
                                          std::is_void_v<Dummy>>>
    explicit Utility(Tags... tags)
        : UtilityBase<I, Tags...>(std::move(tags)...) {
        this->set_type_signature(UtilityBase<I, Tags...>::make_signature(
            extract_class_name(get_type_name<I>()),
            extract_class_name(get_type_name<O>())));
    }

    // UtilityExecutor accesses protected set_context/clear_context through
    // this derived class pointer — valid per [class.access.base]/5.
    friend class behaviors::UtilityExecutor<I, O, Tags...>;

    virtual coro::CoroTask<O> process(const I& input) = 0;
};

}  // namespace dftracer::utils::utilities

#endif  // DFTRACER_UTILS_CORE_UTILITIES_UTILITY_H
