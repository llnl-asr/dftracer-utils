#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/json/line.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/trace/chunk_extractor_utility.h>
#include <dftracer/utils/utilities/fileio/chunk_writer.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <algorithm>

namespace dftracer::utils::trace {

using utilities::fileio::ChunkWriter;
using utilities::fileio::ChunkWriterConfig;
namespace lines = utilities::fileio::lines;

std::string_view event_of_line(std::string_view line) {
    const char* trimmed;
    std::size_t trimmed_length;
    if (json::trim_and_validate(line.data(), line.length(), trimmed,
                                trimmed_length) &&
        trimmed_length > 8) {
        return std::string_view(trimmed, trimmed_length);
    }
    return {};
}

ExtractionBudget plan_extraction_budget(std::uint64_t total_bytes,
                                        std::size_t max_parallel,
                                        std::size_t threads,
                                        std::size_t member_size_bytes) {
    const std::uint64_t member = member_size_bytes;
    const std::uint64_t floor = 4 * member;
    const std::uint64_t ceiling =
        floor * (std::max<std::size_t>(threads, 1) + 2);
    const std::uint64_t share =
        total_bytes / std::max<std::size_t>(max_parallel, 1);
    ExtractionBudget budget;
    budget.writer_bytes = std::clamp(share, floor, ceiling);
    budget.reserve_bytes =
        budget.writer_bytes + ChunkWriterConfig{}.io_flush_bytes;
    return budget;
}

ExtractionAdmission::ExtractionAdmission(std::uint64_t total_bytes,
                                         std::uint64_t reserve_bytes,
                                         std::size_t max_parallel)
    : semaphore_(std::min<std::uint64_t>(
          total_bytes, reserve_bytes * std::max<std::size_t>(max_parallel, 1))),
      held_(std::min(reserve_bytes, total_bytes)) {}

coro::CoroTask<void> ExtractionAdmission::enter() {
    co_await semaphore_.acquire(held_);
    const std::size_t running =
        in_flight_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::size_t seen = peak_in_flight_.load(std::memory_order_relaxed);
    while (running > seen && !peak_in_flight_.compare_exchange_weak(
                                 seen, running, std::memory_order_relaxed)) {
    }
}

void ExtractionAdmission::leave() {
    in_flight_.fetch_sub(1, std::memory_order_relaxed);
    semaphore_.release(held_);
}

coro::CoroTask<ChunkExtractorUtilityOutput> ChunkExtractorUtility::operator()(
    const ChunkExtractorUtilityInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("extract chunks");
    ChunkExtractorUtilityOutput result;
    result.chunk_index = input.chunk_index;

    try {
        co_return co_await extract_and_write(input);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to extract chunks of group %d: %s",
                                 input.chunk_index, e.what());
        co_return result;
    }
}

coro::CoroTask<ChunkExtractorUtilityOutput>
ChunkExtractorUtility::extract_and_write(
    const ChunkExtractorUtilityInput& input) {
    ChunkExtractorUtilityOutput result;
    result.chunk_index = input.chunk_index;

    ChunkWriterConfig writer_config;
    writer_config.output_dir = input.output_dir;
    writer_config.base_name =
        input.app_name + "-" + std::to_string(input.chunk_index);
    writer_config.chunk_size_bytes = input.chunk_size_bytes;
    writer_config.member_size_bytes = input.member_size_bytes;
    writer_config.compress = input.compress;
    writer_config.memory_budget = input.memory_budget;

    ChunkWriter writer(writer_config);
    co_await writer.open();

    std::size_t content_hash = 0;
    utilities::hash::HasherUtility hasher;

    for (const auto& file : input.files) {
        auto line_gen = lines::sources::async_streaming_gz_lines(file);
        for (;;) {
            auto line_opt = co_await line_gen.next();
            if (!line_opt) break;
            const auto& line = *line_opt;
            const std::string_view event = event_of_line(
                std::string_view(line.content.data(), line.content.length()));
            if (!event.empty()) {
                co_await writer.write_line(
                    ByteView(event.data(), event.size()));

                if (input.compute_hash) {
                    hasher.reset();
                    hasher.update(event);
                    content_hash += hasher.get_hash().value;
                }
            }
        }
    }

    co_await writer.close();

    result.chunks = writer.chunks();
    result.events = writer.total_events_written();
    result.event_hash = content_hash;
    result.peak_bytes = writer.peak_reserved_bytes();
    result.success = true;

    DFTRACER_UTILS_LOG_DEBUG(
        "Group %d: %zu events in %zu chunk files (hash=0x%zx)",
        input.chunk_index, result.events, result.chunks.size(),
        result.event_hash);

    co_return result;
}

}  // namespace dftracer::utils::trace
