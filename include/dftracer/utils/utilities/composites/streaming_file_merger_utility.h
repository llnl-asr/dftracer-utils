#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_STREAMING_FILE_MERGER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_STREAMING_FILE_MERGER_UTILITY_H

#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/composites/dft/event_hasher_utility.h>
#include <dftracer/utils/utilities/composites/dft/event_id_extractor_utility.h>
#include <dftracer/utils/utilities/composites/file_merger_utility.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <atomic>
#include <memory>
#include <string>

namespace dftracer::utils::utilities::composites {

/**
 * @brief Batch of events sent through channel during streaming merge.
 *
 * Usage:
 * @code
 * StreamingMergeBatchUtility batch;
 * batch.add(json_content, event_id);
 * if (batch.size() >= 1000) {
 *     co_await channel->send(std::move(batch));
 *     batch = StreamingMergeBatchUtility{};
 * }
 * @endcode
 */
struct StreamingMergeBatchUtility {
    std::vector<std::string> contents;
    std::size_t batch_hash{0};

    StreamingMergeBatchUtility() = default;

    void add(std::string content, const dft::EventId& event_id) {
        contents.push_back(std::move(content));
        utilities::hash::HasherUtility hasher;
        hasher.update(event_id.id);
        hasher.update(event_id.pid);
        hasher.update(event_id.tid);
        batch_hash += hasher.get_hash().value;
    }

    void add_unchecked(std::string content) {
        contents.push_back(std::move(content));
    }

    std::size_t size() const { return contents.size(); }
    bool empty() const { return contents.empty(); }
    void clear() {
        contents.clear();
        batch_hash = 0;
    }
};

/**
 * @brief Input for streaming file producer.
 */
struct StreamingFileProducerInput {
    std::string file_path;
    std::size_t batch_size{1000};
    bool verify{false};

    static StreamingFileProducerInput from_file(const std::string& path) {
        StreamingFileProducerInput input;
        input.file_path = path;
        return input;
    }

    StreamingFileProducerInput& with_batch_size(std::size_t size) {
        batch_size = size;
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
 * @brief Producer utility that validates a file and sends events to channel.
 *
 * Usage:
 * @code
 * auto channel = coro::make_channel<StreamingMergeEvent>(1000);
 * StreamingFileProducerUtility producer(channel);
 * auto input = StreamingFileProducerInput::from_file("data.pfw.gz")
 *                  .with_index("/tmp/data.idx");
 * auto result = producer.process(input);
 * @endcode
 */
class StreamingFileProducerUtility {
   private:
    std::shared_ptr<coro::Channel<StreamingMergeBatchUtility>> channel_;

   public:
    explicit StreamingFileProducerUtility(
        std::shared_ptr<coro::Channel<StreamingMergeBatchUtility>> channel)
        : channel_(std::move(channel)) {}

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
    bool success{false};
    std::string output_path;
    std::size_t total_events{0};
    std::size_t output_hash{0};
};

/**
 * @brief Consumer utility that reads events from channel and writes to file.
 *
 * Usage:
 * @code
 * auto channel = coro::make_channel<StreamingMergeEvent>(1000);
 * StreamingFileConsumerUtility consumer(channel);
 * auto input = StreamingFileConsumerInput::with_output("output.pfw")
 *                  .with_compression(true);
 * auto result = consumer.process(input);
 * @endcode
 */
class StreamingFileConsumerUtility {
   private:
    std::shared_ptr<coro::Channel<StreamingMergeBatchUtility>> channel_;

   public:
    explicit StreamingFileConsumerUtility(
        std::shared_ptr<coro::Channel<StreamingMergeBatchUtility>> channel)
        : channel_(std::move(channel)) {}

    coro::CoroTask<StreamingFileConsumerOutput> process_async(
        CoroScope& ctx, const StreamingFileConsumerInput& input);
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_STREAMING_FILE_MERGER_UTILITY_H
