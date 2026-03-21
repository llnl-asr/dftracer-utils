#ifndef DFTRACER_UTILS_CORE_UTILITIES_STREAMING_UTILITY_H
#define DFTRACER_UTILS_CORE_UTILITIES_STREAMING_UTILITY_H

#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/utilities/utility.h>

namespace dftracer::utils::utilities {

/**
 * @brief Streaming utility: process() yields batches via AsyncGenerator<Batch>.
 *
 * Use when a single input produces an unbounded or large sequence of output
 * batches that should be consumed lazily rather than materialized all at once.
 *
 * @tparam I     Input type
 * @tparam Batch Element type yielded per iteration
 * @tparam Tags  Variadic tag types for opt-in features
 *
 * Usage:
 * @code
 * class MyStreamer
 *     : public StreamingUtility<FileInput, std::vector<Event>> {
 *     coro::AsyncGenerator<std::vector<Event>>
 *     process(const FileInput& input) override {
 *         while (has_more(input)) {
 *             co_yield read_batch(input);
 *         }
 *     }
 * };
 *
 * auto gen = streamer.process(file_input);
 * while (auto batch = co_await gen.next()) {
 *     handle(*batch);
 * }
 * @endcode
 */
template <typename I, typename Batch, typename... Tags>
class StreamingUtility : public UtilityBase<I, Tags...> {
   public:
    using BatchType = Batch;

    StreamingUtility() : UtilityBase<I, Tags...>() {
        this->set_type_signature(UtilityBase<I, Tags...>::make_signature(
            extract_class_name(get_type_name<I>()),
            extract_class_name(get_type_name<Batch>()) + "*"));
    }

    template <typename Dummy = void,
              typename = std::enable_if_t<(sizeof...(Tags) > 0) &&
                                          std::is_void_v<Dummy>>>
    explicit StreamingUtility(Tags... tags)
        : UtilityBase<I, Tags...>(std::move(tags)...) {
        this->set_type_signature(UtilityBase<I, Tags...>::make_signature(
            extract_class_name(get_type_name<I>()),
            extract_class_name(get_type_name<Batch>()) + "*"));
    }

    virtual coro::AsyncGenerator<Batch> process(const I& input) = 0;
};

}  // namespace dftracer::utils::utilities

#endif  // DFTRACER_UTILS_CORE_UTILITIES_STREAMING_UTILITY_H
