#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/utilities.h>

#include <chrono>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites;

class MergeArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{cli::DirMode::DEFAULT_DOT,
                                 "Directory containing .pfw or .pfw.gz files"};
    cli::PipelineArgs pipeline;
    cli::WatchdogArgs watchdog;

    bool force = false;
    std::string output;
    bool compress = false;
    bool gzip_only = false;
    bool verify = false;
    std::size_t channel_capacity = 100;
    std::size_t batch_size_kb = 256;

    explicit MergeArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        schema(directory, pipeline, watchdog);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("-f", "--force")
            .help("Override existing output file and force index recreation")
            .flag();

        parser()
            .add_argument("-o", "--output")
            .help("Output file path (should have .pfw extension)")
            .default_value<std::string>("combined.pfw");

        parser()
            .add_argument("-c", "--compress")
            .help("Compress output file with gzip")
            .flag();

        parser()
            .add_argument("-g", "--gzip-only")
            .help("Process only .pfw.gz files")
            .flag();

        parser()
            .add_argument("--verify")
            .help("Verify merged output by comparing input/output hashes")
            .flag();

        parser()
            .add_argument("--channel-capacity")
            .help("Channel buffer capacity for batch streaming (default: 100)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(100));

        parser()
            .add_argument("--batch-size")
            .help("Batch byte budget in KB (default: 256)")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(256));
    }

    void post_parse() override {
        force = parser().get<bool>("--force");
        output = parser().get<std::string>("--output");
        compress = parser().get<bool>("--compress");
        gzip_only = parser().get<bool>("--gzip-only");
        verify = parser().get<bool>("--verify");
        channel_capacity = parser().get<std::size_t>("--channel-capacity");
        batch_size_kb = parser().get<std::size_t>("--batch-size");
    }
};

static int run_merge(const MergeArgParse& cli);

int main(int argc, char** argv) {
    return cli::cli_main<MergeArgParse>(
        argc, argv, "dftracer_merge",
        "Merge DFTracer .pfw or .pfw.gz files into a single JSON array file "
        "using streaming producer-consumer pattern",
        [](MergeArgParse& cli) { return run_merge(cli); });
}

static int run_merge(const MergeArgParse& cli) {
    const auto input_dir = fs::absolute(cli.directory.value).string();
    const auto output_file = fs::absolute(cli.output).string();
    const auto force_override = cli.force;
    const auto compress_output = cli.compress;
    const auto gzip_only = cli.gzip_only;
    const auto verify = cli.verify;
    const auto channel_capacity = cli.channel_capacity;
    const auto batch_size_kb = cli.batch_size_kb;
    std::size_t batch_byte_budget = batch_size_kb * 1024;

    if (output_file.size() < 4 ||
        output_file.substr(output_file.size() - 4) != ".pfw") {
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "Output file should have .pfw extension");
        return 1;
    }

    std::string final_output =
        compress_output ? output_file + ".gz" : output_file;
    if (fs::exists(final_output) && !force_override) {
        DFTRACER_UTILS_LOG_ERROR(
            "Output file %s exists and force override is disabled",
            final_output.c_str());
        return 1;
    }

    if (force_override) {
        if (fs::exists(output_file)) fs::remove(output_file);
        if (fs::exists(output_file + ".gz")) fs::remove(output_file + ".gz");
    }

    std::vector<std::string> input_files;
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (entry.is_regular_file()) {
            std::string path = entry.path().string();
            const std::string pfw_gz_suffix = ".pfw.gz";
            const std::string pfw_suffix = ".pfw";

            if (path.size() >= pfw_gz_suffix.size() &&
                path.compare(path.size() - pfw_gz_suffix.size(),
                             pfw_gz_suffix.size(), pfw_gz_suffix) == 0) {
                input_files.push_back(path);
            } else if (!gzip_only && path.size() >= pfw_suffix.size() &&
                       path.compare(path.size() - pfw_suffix.size(),
                                    pfw_suffix.size(), pfw_suffix) == 0) {
                input_files.push_back(path);
            }
        }
    }

    if (input_files.empty()) {
        const char* file_types = gzip_only ? ".pfw.gz" : ".pfw or .pfw.gz";
        DFTRACER_UTILS_LOG_ERROR("No %s files found in directory: %s",
                                 file_types, input_dir.c_str());
        return 1;
    }

    std::printf("==========================================\n");
    std::printf("DFTracer Merge (Streaming Channel)\n");
    std::printf("==========================================\n");
    std::printf("Arguments:\n");
    std::printf("  Input dir: %s\n", input_dir.c_str());
    std::printf("  Output file: %s\n", final_output.c_str());
    std::printf("  Files found: %zu\n", input_files.size());
    std::printf("  Override: %s\n", force_override ? "true" : "false");
    std::printf("  Compress: %s\n", compress_output ? "true" : "false");
    std::printf("  Verify: %s\n", verify ? "true" : "false");
    std::printf("  Channel capacity: %zu\n", channel_capacity);
    std::printf("  Batch size: %zu KB\n", batch_size_kb);
    std::printf("  Executor threads: %zu\n", cli.pipeline.executor_threads);
    std::printf("==========================================\n\n");

    auto start_time = std::chrono::high_resolution_clock::now();

    auto channel = coro::make_channel<StreamingMergeBatch>(channel_capacity);
    std::size_t pool_size = channel_capacity + input_files.size();
    auto buf_pool =
        make_buffer_pool<std::string>(pool_size, [batch_byte_budget]() {
            std::string s;
            s.reserve(batch_byte_budget);
            return s;
        });

    std::vector<StreamingFileProducerOutput> producer_results;
    producer_results.resize(input_files.size());
    StreamingFileConsumerOutput consumer_result;
    bool consumer_success = false;

    auto pipeline_config = cli::build_pipeline_config(
        "DFTracer Merge", cli.pipeline, cli.watchdog);
    Pipeline pipeline(pipeline_config);

    std::vector<std::shared_ptr<Task>> producer_tasks;
    for (std::size_t i = 0; i < input_files.size(); ++i) {
        auto* input_files_ptr = &input_files;
        auto* producer_results_ptr = &producer_results;
        auto producer_task = make_task(
            [i, input_files_ptr, verify, batch_byte_budget, channel, buf_pool,
             ch = channel->producer(),
             producer_results_ptr]([[maybe_unused]] CoroScope& ctx) mutable
                -> coro::CoroTask<StreamingFileProducerOutput> {
                auto guard = ch.guard();

                StreamingFileProducerUtility producer(channel, buf_pool);
                auto input =
                    StreamingFileProducerInput::from_file((*input_files_ptr)[i])
                        .with_batch_byte_budget(batch_byte_budget)
                        .with_verify(verify);

                auto result = co_await producer.process_async(ctx, input);
                (*producer_results_ptr)[i] = result;

                co_return result;
            },
            "Producer-" + std::to_string(i));
        producer_tasks.push_back(producer_task);
    }

    auto* consumer_result_ptr = &consumer_result;
    auto* consumer_success_ptr = &consumer_success;
    auto consumer_task = make_task(
        [channel, buf_pool, output_file, compress_output, consumer_result_ptr,
         consumer_success_ptr]([[maybe_unused]] CoroScope& ctx)
            -> coro::CoroTask<StreamingFileConsumerOutput> {
            StreamingFileConsumerUtility consumer(channel, buf_pool);

            auto input = StreamingFileConsumerInput::with_output(output_file)
                             .with_compression(compress_output);

            auto result = co_await consumer.process_async(ctx, input);
            if (result) {
                *consumer_result_ptr = *std::move(result);
                *consumer_success_ptr = true;
            }
            co_return *consumer_result_ptr;
        },
        "Consumer");

    std::vector<std::shared_ptr<Task>> all_tasks;
    all_tasks.insert(all_tasks.end(), producer_tasks.begin(),
                     producer_tasks.end());
    all_tasks.push_back(consumer_task);

    pipeline.set_source(all_tasks);
    pipeline.execute();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::size_t input_hash = 0;
    std::size_t successful_files = 0;
    std::size_t total_events_sent = 0;

    for (const auto& result : producer_results) {
        if (result.success) {
            successful_files++;
            total_events_sent += result.events_sent;
            input_hash += result.input_hash;
        }
    }

    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Merge Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Processed: %zu/%zu files\n", successful_files,
                input_files.size());
    std::printf("  Events streamed: %zu\n", total_events_sent);
    std::printf("  Output: %s\n", consumer_result.output_path.c_str());
    std::printf("  Total events in output: %zu\n",
                consumer_result.total_events);

    if (verify) {
        if (input_hash == consumer_result.output_hash) {
            std::printf(
                "  \u2713 Verification: PASSED - all input events present in "
                "merged output\n");
        } else {
            std::printf(
                "  \u2717 Verification: FAILED - event mismatch detected\n");
        }
        std::printf("    Input hash:  0x%016zx\n", input_hash);
        std::printf("    Output hash: 0x%016zx\n", consumer_result.output_hash);
    }

    std::printf("  Status: %s\n", consumer_success ? "SUCCESS" : "FAILED");
    std::printf("==========================================\n");

    return (successful_files == input_files.size() && consumer_success) ? 0 : 1;
}
