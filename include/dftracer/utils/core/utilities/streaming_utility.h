#ifndef DFTRACER_UTILS_CORE_UTILITIES_STREAMING_UTILITY_H
#define DFTRACER_UTILS_CORE_UTILITIES_STREAMING_UTILITY_H

#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/utilities/utility.h>

namespace dftracer::utils::utilities {

template <typename I, typename Batch>
consteval auto make_streaming_signature() {
    return ConstString<512>()
        .append("Utility[")
        .append(get_type_name<I>())
        .append("->")
        .append(get_type_name<Batch>())
        .append("*]");
}

/**
 * @brief Streaming utility: process() yields batches via AsyncGenerator<Batch>.
 *
 * Use when a single input produces an unbounded or large sequence of output
 * batches that should be consumed lazily rather than materialized all at once.
 *
 * @tparam I     Input type
 * @tparam Batch Element type yielded per iteration
 * @tparam Tags  Variadic tag types for opt-in features
 */
template <typename I, typename Batch, typename... Tags>
class StreamingUtility : public UtilityBase<I, Tags...> {
   private:
    static constexpr auto sig_ = make_streaming_signature<I, Batch>();

   public:
    using BatchType = Batch;

    StreamingUtility() : UtilityBase<I, Tags...>() {}

    template <typename Dummy = void,
              typename = std::enable_if_t<(sizeof...(Tags) > 0) &&
                                          std::is_void_v<Dummy>>>
    explicit StreamingUtility(Tags... tags)
        : UtilityBase<I, Tags...>(std::move(tags)...) {}

    static constexpr std::string_view get_type_signature() { return sig_; }
    static constexpr std::string_view get_name() { return sig_; }

    virtual coro::AsyncGenerator<Batch> process(const I& input) = 0;
};

}  // namespace dftracer::utils::utilities

#endif  // DFTRACER_UTILS_CORE_UTILITIES_STREAMING_UTILITY_H
