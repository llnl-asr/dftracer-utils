#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_CHUNK_VERIFIER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_CHUNK_VERIFIER_UTILITY_H

#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/utilities.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

namespace dftracer::utils::utilities::composites {

/**
 * @brief Input for chunk verification.
 */
template <typename ChunkType, typename MetadataType>
struct ChunkVerificationUtilityInput {
    std::vector<ChunkType> chunks;
    std::vector<MetadataType> metadata;

    ChunkVerificationUtilityInput() = default;

    ChunkVerificationUtilityInput(std::vector<ChunkType> c,
                                  std::vector<MetadataType> m)
        : chunks(std::move(c)), metadata(std::move(m)) {}

    static ChunkVerificationUtilityInput<ChunkType, MetadataType> from_chunks(
        std::vector<ChunkType> c) {
        ChunkVerificationUtilityInput<ChunkType, MetadataType> input;
        input.chunks = std::move(c);
        return input;
    }

    ChunkVerificationUtilityInput<ChunkType, MetadataType>& with_metadata(
        std::vector<MetadataType> m) {
        metadata = std::move(m);
        return *this;
    }

    bool operator==(
        const ChunkVerificationUtilityInput<ChunkType, MetadataType>& other)
        const {
        return chunks == other.chunks && metadata == other.metadata;
    }
};

/**
 * @brief Output from chunk verification.
 */
struct ChunkVerificationUtilityOutput {
    bool passed = false;
    std::uint64_t input_hash = 0;
    std::uint64_t output_hash = 0;
    std::string error_message;

    ChunkVerificationUtilityOutput() = default;

    static ChunkVerificationUtilityOutput success(std::uint64_t input_h,
                                                  std::uint64_t output_h) {
        ChunkVerificationUtilityOutput result;
        result.passed = true;
        result.input_hash = input_h;
        result.output_hash = output_h;
        return result;
    }

    static ChunkVerificationUtilityOutput failure(std::uint64_t input_h,
                                                  std::uint64_t output_h,
                                                  const std::string& error) {
        ChunkVerificationUtilityOutput result;
        result.passed = false;
        result.input_hash = input_h;
        result.output_hash = output_h;
        result.error_message = error;
        return result;
    }

    bool operator==(const ChunkVerificationUtilityOutput& other) const {
        return passed == other.passed && input_hash == other.input_hash &&
               output_hash == other.output_hash &&
               error_message == other.error_message;
    }
};

/**
 * @brief Generic chunk verifier that compares input and output events.
 *
 * @tparam ChunkType Type of chunks (e.g., ChunkResult)
 * @tparam MetadataType Type of metadata (e.g., FileMetadata)
 * @tparam EventType Type of events (e.g., EventId)
 */
template <typename ChunkType, typename MetadataType, typename EventType>
class ChunkVerifierUtility
    : public utilities::Utility<
          ChunkVerificationUtilityInput<ChunkType, MetadataType>,
          ChunkVerificationUtilityOutput, utilities::tags::NeedsContext> {
   public:
    using InputHashFn =
        std::function<std::uint64_t(const std::vector<MetadataType>&)>;
    using EventCollectorFn =
        std::function<std::vector<EventType>(CoroScope&, const ChunkType&)>;
    using EventHashFn =
        std::function<std::uint64_t(const std::vector<EventType>&)>;

   private:
    InputHashFn input_hasher_;
    EventCollectorFn event_collector_;
    EventHashFn event_hasher_;

   public:
    /**
     * @brief Construct verifier with hash and collection functions.
     *
     * @param input_hasher Function to compute hash from metadata
     * @param event_collector Function to collect events from chunks
     * @param event_hasher Function to compute hash from events
     */
    ChunkVerifierUtility(InputHashFn input_hasher,
                         EventCollectorFn event_collector,
                         EventHashFn event_hasher)
        : input_hasher_(std::move(input_hasher)),
          event_collector_(std::move(event_collector)),
          event_hasher_(std::move(event_hasher)) {}

    /**
     * @brief Verify that output chunks contain the same events as input.
     *
     * @param input Verification input with chunks and metadata
     * @return Verification result with pass/fail and hashes
     */
    ChunkVerificationUtilityOutput process(
        const ChunkVerificationUtilityInput<ChunkType, MetadataType>& input)
        override {
        // Step 1: Compute input hash
        std::uint64_t input_hash = input_hasher_(input.metadata);

        // Step 2: Get CoroScope for parallel event collection
        CoroScope& ctx = this->context();

        // Collect events from all chunks sequentially
        std::vector<EventType> output_events;
        for (const auto& chunk : input.chunks) {
            auto events = event_collector_(ctx, chunk);
            output_events.insert(output_events.end(), events.begin(),
                                 events.end());
        }

        // Step 5: Sort events for consistent hashing
        std::sort(output_events.begin(), output_events.end());

        // Step 6: Compute output hash
        std::uint64_t output_hash = event_hasher_(output_events);

        // Step 7: Compare hashes
        if (input_hash == output_hash) {
            return ChunkVerificationUtilityOutput::success(input_hash,
                                                           output_hash);
        } else {
            return ChunkVerificationUtilityOutput::failure(
                input_hash, output_hash,
                "Hash mismatch: input and output events differ");
        }
    }
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_CHUNK_VERIFIER_UTILITY_H
