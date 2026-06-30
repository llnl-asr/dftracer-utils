#ifndef DFTRACER_UTILS_CORE_UTILITIES_UTILITY_H
#define DFTRACER_UTILS_CORE_UTILITIES_UTILITY_H

#include <dftracer/utils/core/common/const_string.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/type_name.h>
#include <dftracer/utils/core/coro/task.h>

#include <memory>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace dftracer::utils {
class CoroScope;
}  // namespace dftracer::utils

namespace dftracer::utils::utilities {

namespace tags {
struct NeedsContext;
}  // namespace tags

namespace behaviors {
template <typename I, typename O, typename... Tags>
class UtilityExecutor;
}

template <typename I>
consteval auto make_input_signature() {
    return ConstString<512>()
        .append("Utility[")
        .append(get_type_name<I>())
        .append("]");
}

template <typename I, typename O>
consteval auto make_utility_signature() {
    return ConstString<512>()
        .append("Utility[")
        .append(get_type_name<I>())
        .append("->")
        .append(get_type_name<O>())
        .append("]");
}

/**
 * @brief Shared machinery for all utility variants.
 *
 * Holds the context pointer; tags are compile-time markers (queried via
 * has_tag<>()), not stored per instance.
 * Type signature is generated at compile time and stored as a static constexpr
 * string_view.
 *
 * @tparam I Input type
 * @tparam Tags Variadic tag types for opt-in features
 */
template <typename I, typename... Tags>
class UtilityBase {
   private:
    CoroScope* ctx_ = nullptr;

    static constexpr auto sig_ = make_input_signature<I>();

   public:
    using Input = I;
    using TagsTuple = std::tuple<Tags...>;

    UtilityBase() = default;

    virtual ~UtilityBase() = default;

    UtilityBase(const UtilityBase&) = default;
    UtilityBase& operator=(const UtilityBase&) = default;
    UtilityBase(UtilityBase&&) = default;
    UtilityBase& operator=(UtilityBase&&) = default;

    template <typename Tag>
    static constexpr bool has_tag() {
        return (std::is_same_v<Tag, Tags> || ...);
    }

    static constexpr std::string_view get_type_signature() { return sig_; }
    static constexpr std::string_view get_name() { return sig_; }

   protected:
    bool has_context() const noexcept { return ctx_ != nullptr; }

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
            throw DFTUtilsException::cat(
                ErrorCode::PIPELINE, get_name(),
                " requires a bound CoroScope. Execute it via Runtime::scope(), "
                "UtilityExecutor, or a pipeline, not by calling process() "
                "directly.");
        }
        return *ctx_;
    }

    void set_context(CoroScope& ctx) { ctx_ = &ctx; }
    void clear_context() { ctx_ = nullptr; }

    friend class ::dftracer::utils::CoroScope;
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
   private:
    static constexpr auto sig_ = make_utility_signature<I, O>();

   public:
    using Output = O;

    Utility() : UtilityBase<I, Tags...>() {}

    template <typename Dummy = void,
              typename = std::enable_if_t<(sizeof...(Tags) > 0) &&
                                          std::is_void_v<Dummy>>>
    explicit Utility(Tags... tags)
        : UtilityBase<I, Tags...>(std::move(tags)...) {}

    static constexpr std::string_view get_type_signature() { return sig_; }
    static constexpr std::string_view get_name() { return sig_; }

    // UtilityExecutor accesses protected set_context/clear_context through
    // this derived class pointer — valid per [class.access.base]/5.
    friend class behaviors::UtilityExecutor<I, O, Tags...>;

    virtual coro::CoroTask<O> process(const I& input) = 0;

    // Rvalue overload picked automatically for braced-init / std::move /
    // other prvalue call expressions. Moves the input into wrapper storage
    // so the inner virtual receives a stable reference that outlives every
    // internal suspension point. Lvalue call sites still bind to
    // process(const I&) directly, so hot loops that reuse a named local
    // pay zero overhead.
    coro::CoroTask<O> process(I&& input) {
#if defined(__GNUC__) && !defined(__clang__) && (__GNUC__ < 14)
        // GCC 12/13 miscalculate frame offsets for non-trivial locals in
        // coroutine frames. Only non-trivial I is affected, so heap-allocate it
        // (a trivial pointer slot lives in the frame) and let
        // trivially-copyable I stay a zero-alloc frame local. Drop once GCC
        // 12/13 is retired.
        if constexpr (!std::is_trivially_copyable_v<I>) {
            auto owned = std::make_unique<I>(std::move(input));
            co_return co_await this->process(static_cast<const I&>(*owned));
        } else {
            I local(std::move(input));
            co_return co_await this->process(static_cast<const I&>(local));
        }
#else
        // GCC 14+, Clang 14+, MSVC: frame-local is safe per the language rules.
        I local(std::move(input));
        co_return co_await this->process(static_cast<const I&>(local));
#endif
    }
};

}  // namespace dftracer::utils::utilities

#endif  // DFTRACER_UTILS_CORE_UTILITIES_UTILITY_H
