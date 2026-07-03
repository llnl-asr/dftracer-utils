#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_STREAMING_FILE_MERGER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_STREAMING_FILE_MERGER_UTILITY_H

#include <dftracer/utils/core/common/buffer_pool.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/composites/dft/event_id_extractor_utility.h>
#include <dftracer/utils/utilities/composites/file_merger_utility.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <memory>
#include <string>

namespace dftracer::utils::utilities::composites {

/**
 * @brief Raw byte buffer sent through channel during streaming merge.
 *
 * Contains pre-stripped chunk data (no array delimiters) ready to write.
 * Each buf holds valid NDJSON lines separated by newlines.
 */
struct StreamingMergeBatch {
    std::string buf;
    std::size_t event_count{0};
    std::size_t batch_hash{0};
};

/**
 * @brief Input for streaming file producer.
 */
struct StreamingFileProducerInput {
    std::string file_path;
    std::size_t batch_byte_budget{256 * 1024};  // 256KB default
    bool verify{false};

    static StreamingFileProducerInput from_file(const std::string& path) {
        StreamingFileProducerInput input;
        input.file_path = path;
        return input;
    }

    StreamingFileProducerInput& with_batch_byte_budget(std::size_t bytes) {
        batch_byte_budget = bytes;
        return *this;
    }

    StreamingFileProducerInput& with_verify(bool v) {
        verify = v;
        return *this;
    }
};

/**
 * @brief Output from streaming file producer.
 */
struct StreamingFileProducerOutput {
    std::string file_path;
    bool success{false};
    std::size_t events_sent{0};
    std::size_t input_hash{0};
};

/**
 * @brief Producer utility that decompresses a file and sends raw chunks
 * to channel with array delimiters stripped.
 *
 * In verify mode, computes per-line byte hash for integrity checking.
 */
class StreamingFileProducerUtility {
   private:
    std::shared_ptr<coro::Channel<StreamingMergeBatch>> channel_;
    std::shared_ptr<BufferPool<std::string>> buf_pool_;

   public:
    StreamingFileProducerUtility(
        std::shared_ptr<coro::Channel<StreamingMergeBatch>> channel,
        std::shared_ptr<BufferPool<std::string>> buf_pool)
        : channel_(std::move(channel)), buf_pool_(std::move(buf_pool)) {}

    coro::CoroTask<StreamingFileProducerOutput> process_async(
        CoroScope& ctx, const StreamingFileProducerInput& input);
};

/**
 * @brief Input for streaming file consumer.
 */
struct StreamingFileConsumerInput {
    std::string output_file;
    bool compress{false};

    static StreamingFileConsumerInput with_output(const std::string& path) {
        StreamingFileConsumerInput input;
        input.output_file = path;
        return input;
    }

    StreamingFileConsumerInput& with_compression(bool enable) {
        compress = enable;
        return *this;
    }
};

/**
 * @brief Output from streaming file consumer.
 */
struct StreamingFileConsumerOutput {
    std::string output_path;
    std::size_t total_events{0};
    std::size_t output_hash{0};
};

/**
 * @brief Consumer utility that reads raw byte batches from channel and
 * writes to file, wrapping in JSON array delimiters.
 */
class StreamingFileConsumerUtility {
   private:
    std::shared_ptr<coro::Channel<StreamingMergeBatch>> channel_;
    std::shared_ptr<BufferPool<std::string>> buf_pool_;

   public:
    StreamingFileConsumerUtility(
        std::shared_ptr<coro::Channel<StreamingMergeBatch>> channel,
        std::shared_ptr<BufferPool<std::string>> buf_pool)
        : channel_(std::move(channel)), buf_pool_(std::move(buf_pool)) {}

    coro::CoroTask<Result<StreamingFileConsumerOutput>> process_async(
        CoroScope& ctx, const StreamingFileConsumerInput& input);
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_STREAMING_FILE_MERGER_UTILITY_H
