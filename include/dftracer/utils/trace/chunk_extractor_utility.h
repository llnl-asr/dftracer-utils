#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_CHUNK_EXTRACTOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_CHUNK_EXTRACTOR_UTILITY_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/scoped_fd.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/runtime/coro/async_semaphore.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/utilities/fileio/chunk_writer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::trace {

/**
 * @brief Input for DFTracer chunk extraction.
 *
 * `files` are gzip traces, read once and in order through one rolling writer.
 */
struct ChunkExtractorUtilityInput {
    int chunk_index = 0;  ///< Group number, part of the output file names
    std::vector<std::string> files;
    /// If not empty, parallel to `files`: an open descriptor of each file, read
    /// instead of opening it. A null entry opens the file.
    std::vector<dstd::SharedPtr<ScopedFd>> open_files;
    std::string output_dir;
    std::string app_name;
    bool compress = false;
    bool compute_hash = true;
    /// Uncompressed bytes per gzip member.
    std::size_t member_size_bytes = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    /// A chunk file ends at the first member boundary at or past this many
    /// uncompressed bytes.
    std::size_t chunk_size_bytes = std::numeric_limits<std::size_t>::max();
    /// Bytes the chunk writer may hold, as ChunkWriterConfig::memory_budget.
    std::uint64_t memory_budget = 0;

    static ChunkExtractorUtilityInput from_files(int index,
                                                 std::vector<std::string> f) {
        ChunkExtractorUtilityInput input;
        input.chunk_index = index;
        input.files = std::move(f);
        return input;
    }

    ChunkExtractorUtilityInput& with_open_files(
        std::vector<dstd::SharedPtr<ScopedFd>> fds) {
        open_files = std::move(fds);
        return *this;
    }

    ChunkExtractorUtilityInput& with_output_dir(std::string dir) {
        output_dir = std::move(dir);
        return *this;
    }

    ChunkExtractorUtilityInput& with_app_name(std::string name) {
        app_name = std::move(name);
        return *this;
    }

    ChunkExtractorUtilityInput& with_compression(bool enabled) {
        compress = enabled;
        return *this;
    }

    ChunkExtractorUtilityInput& with_compute_hash(bool enabled) {
        compute_hash = enabled;
        return *this;
    }

    ChunkExtractorUtilityInput& with_member_size(std::size_t bytes) {
        member_size_bytes = bytes;
        return *this;
    }

    ChunkExtractorUtilityInput& with_chunk_size(std::size_t bytes) {
        chunk_size_bytes = bytes;
        return *this;
    }

    ChunkExtractorUtilityInput& with_memory_budget(std::uint64_t bytes) {
        memory_budget = bytes;
        return *this;
    }
};

/**
 * @brief Result of DFTracer chunk extraction.
 */
struct ChunkExtractorUtilityOutput {
    int chunk_index = 0;
    std::vector<utilities::fileio::ChunkInfo> chunks;  ///< Files written
    std::size_t events = 0;                            ///< JSON events written
    std::size_t event_hash = 0;
    std::uint64_t peak_bytes = 0;  ///< Most bytes the chunk writer reserved
    bool success = false;
};

/// What one extraction takes of a memory budget.
struct ExtractionBudget {
    std::uint64_t writer_bytes = 0;   ///< ChunkExtractorUtilityInput budget
    std::uint64_t reserve_bytes = 0;  ///< The writer plus its flush buffer
};

/**
 * @brief Splits `total_bytes` among extractions that run at once.
 *
 * An extraction gets `total_bytes / max_parallel`, at least the four members a
 * writer needs to make progress and at most what a writer with one worker per
 * thread can use. Extractions that reserve this much fit in `total_bytes`
 * about `max_parallel` at a time, and only one at a time when it is smaller.
 */
ExtractionBudget plan_extraction_budget(std::uint64_t total_bytes,
                                        std::size_t max_parallel,
                                        std::size_t threads,
                                        std::size_t member_size_bytes);

/**
 * @brief Admits extractions while their reservations fit in the budget, and
 * at most `max_parallel` at once.
 *
 * `enter` waits for `reserve_bytes`; a reservation above the budget is clamped
 * and runs alone. Every `enter` is paired with one `leave`.
 */
class ExtractionAdmission {
   public:
    ExtractionAdmission(std::uint64_t total_bytes, std::uint64_t reserve_bytes,
                        std::size_t max_parallel);

    coro::CoroTask<void> enter();
    void leave();

    std::size_t peak_in_flight() const { return peak_in_flight_; }
    std::uint64_t peak_reserved_bytes() const {
        return peak_in_flight_ * held_;
    }

   private:
    coro::CoroSemaphore semaphore_;
    std::uint64_t held_;  ///< Bytes one extraction takes, after the clamp
    std::atomic<std::size_t> in_flight_{0};
    std::atomic<std::size_t> peak_in_flight_{0};
};

/// The trimmed event in a trace line, or an empty view when the line is not an
/// event (a blank line, a bracket or invalid JSON).
std::string_view event_of_line(std::string_view line);

/**
 * @brief Streams DFTracer gzip traces into chunk files of about a given size.
 *
 * Usage:
 * @code
 * ChunkExtractorUtility extractor;
 *
 * auto input = ChunkExtractorUtilityInput::from_files(1, {"a.pfw.gz"})
 *                  .with_output_dir("/output")
 *                  .with_app_name("myapp")
 *                  .with_compression(true)
 *                  .with_chunk_size(256 * 1024 * 1024);
 *
 * auto result = extractor(input).get();
 * if (result.success) {
 *     std::cout << "Wrote " << result.events << " events\n";
 * }
 * @endcode
 */
class ChunkExtractorUtility {
   public:
    coro::CoroTask<ChunkExtractorUtilityOutput> operator()(
        const ChunkExtractorUtilityInput& input);

   private:
    coro::CoroTask<ChunkExtractorUtilityOutput> extract_and_write(
        const ChunkExtractorUtilityInput& input);
};

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_CHUNK_EXTRACTOR_UTILITY_H
