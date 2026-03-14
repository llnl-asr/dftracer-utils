#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/utilities.h>

#include <argparse/argparse.hpp>
#include <chrono>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites;

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_merge",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Merge DFTracer .pfw or .pfw.gz files into a single JSON array file "
        "using streaming producer-consumer pattern");

    program.add_argument("-d", "--directory")
        .help("Directory containing .pfw or .pfw.gz files")
        .default_value<std::string>(".");

    program.add_argument("-o", "--output")
        .help("Output file path (should have .pfw extension)")
        .default_value<std::string>("combined.pfw");

    program.add_argument("-f", "--force")
        .help("Override existing output file and force index recreation")
        .flag();

    program.add_argument("-c", "--compress")
        .help("Compress output file with gzip")
        .flag();

    program.add_argument("-v", "--verbose").help("Enable verbose mode").flag();

    program.add_argument("-g", "--gzip-only")
        .help("Process only .pfw.gz files")
        .flag();

    program.add_argument("--executor-threads")
        .help(
            "Number of executor threads for parallel processing (default: "
            "number of CPU cores)")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    program.add_argument("--verify")
        .help("Verify merged output by comparing input/output hashes")
        .flag();

    program.add_argument("--channel-capacity")
        .help("Channel buffer capacity for batch streaming (default: 100)")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(100));

    program.add_argument("--batch-size")
        .help("Number of events per batch (default: 1000)")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(1000));

    program.add_argument("--disable-watchdog")
        .help("Disable watchdog for hang detection")
        .flag();

    program.add_argument("--watchdog-global-timeout")
        .help(
            "Watchdog global timeout for pipeline execution in seconds (0 = no "
            "timeout)")
        .scan<'d', int>()
        .default_value(0);

    program.add_argument("--watchdog-task-timeout")
        .help("Watchdog default task timeout in seconds (0 = no timeout)")
        .scan<'d', int>()
        .default_value(0);

    program.add_argument("--watchdog-interval")
        .help("Watchdog check interval in seconds")
        .scan<'d', int>()
        .default_value(1);

    program.add_argument("--watchdog-warning-threshold")
        .help("Watchdog long-running task warning threshold in seconds")
        .scan<'d', int>()
        .default_value(300);

    program.add_argument("--watchdog-idle-timeout")
        .help("Watchdog idle timeout in seconds (0 = use default)")
        .scan<'d', int>()
        .default_value(300);

    program.add_argument("--watchdog-deadlock-timeout")
        .help("Watchdog deadlock timeout in seconds (0 = use default)")
        .scan<'d', int>()
        .default_value(600);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error occurred: %s", err.what());
        std::cerr << program << std::endl;
        return 1;
    }

    std::string input_dir = program.get<std::string>("--directory");
    std::string output_file = program.get<std::string>("--output");
    bool force_override = program.get<bool>("--force");
    bool compress_output = program.get<bool>("--compress");
    [[maybe_unused]] bool verbose = program.get<bool>("--verbose");
    bool gzip_only = program.get<bool>("--gzip-only");
    bool verify = program.get<bool>("--verify");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");
    std::size_t channel_capacity =
        program.get<std::size_t>("--channel-capacity");
    std::size_t batch_size = program.get<std::size_t>("--batch-size");
    bool disable_watchdog = program.get<bool>("--disable-watchdog");
    int global_timeout = program.get<int>("--watchdog-global-timeout");
    int task_timeout = program.get<int>("--watchdog-task-timeout");
    int watchdog_interval = program.get<int>("--watchdog-interval");
    int warning_threshold = program.get<int>("--watchdog-warning-threshold");
    int idle_timeout = program.get<int>("--watchdog-idle-timeout");
    int deadlock_timeout = program.get<int>("--watchdog-deadlock-timeout");

    input_dir = fs::absolute(input_dir).string();
    output_file = fs::absolute(output_file).string();

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
    std::printf("  Channel capacity: %zu batches\n", channel_capacity);
    std::printf("  Batch size: %zu events\n", batch_size);
    std::printf("  Executor threads: %zu\n", executor_threads);
    std::printf("==========================================\n\n");

    auto start_time = std::chrono::high_resolution_clock::now();

    // Step 1: Create channel for streaming batches
    auto channel =
        coro::make_channel<StreamingMergeBatchUtility>(channel_capacity);

    std::vector<StreamingFileProducerOutput> producer_results;
    producer_results.resize(input_files.size());
    StreamingFileConsumerOutput consumer_result;

    // Step 2: Create pipeline
    auto pipeline_config =
        PipelineConfig()
            .with_name("DFTracer Merge")
            .with_compute_threads(executor_threads)
            .with_watchdog(!disable_watchdog)
            .with_global_timeout(std::chrono::seconds(global_timeout))
            .with_task_timeout(std::chrono::seconds(task_timeout))
            .with_watchdog_interval(std::chrono::seconds(watchdog_interval))
            .with_warning_threshold(std::chrono::seconds(warning_threshold))
            .with_executor_idle_timeout(std::chrono::seconds(idle_timeout))
            .with_executor_deadlock_timeout(
                std::chrono::seconds(deadlock_timeout));

    Pipeline pipeline(pipeline_config);

    // Step 3: Create producer tasks
    std::vector<std::shared_ptr<Task>> producer_tasks;
    for (std::size_t i = 0; i < input_files.size(); ++i) {
        auto* input_files_ptr = &input_files;
        auto* producer_results_ptr = &producer_results;
        auto producer_task = make_task(
            [i, input_files_ptr, batch_size, verify, channel,
             ch = channel->producer(),
             producer_results_ptr]([[maybe_unused]] CoroScope& ctx) mutable
                -> coro::CoroTask<StreamingFileProducerOutput> {
                auto guard = ch.guard();

                StreamingFileProducerUtility producer(channel);
                auto input =
                    StreamingFileProducerInput::from_file((*input_files_ptr)[i])
                        .with_batch_size(batch_size)
                        .with_verify(verify);

                auto result = co_await producer.process_async(ctx, input);
                (*producer_results_ptr)[i] = result;

                co_return result;
            },
            "Producer-" + std::to_string(i));
        producer_tasks.push_back(producer_task);
    }

    // Step 4: Create consumer task
    auto* consumer_result_ptr = &consumer_result;
    auto consumer_task = make_task(
        [channel, output_file, compress_output,
         consumer_result_ptr]([[maybe_unused]] CoroScope& ctx)
            -> coro::CoroTask<StreamingFileConsumerOutput> {
            StreamingFileConsumerUtility consumer(channel);

            auto input = StreamingFileConsumerInput::with_output(output_file)
                             .with_compression(compress_output);

            *consumer_result_ptr = co_await consumer.process_async(ctx, input);
            co_return *consumer_result_ptr;
        },
        "Consumer");

    // Step 5: Execute pipeline
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

    std::printf("  Status: %s\n",
                consumer_result.success ? "SUCCESS" : "FAILED");
    std::printf("==========================================\n");

    return (successful_files == input_files.size() && consumer_result.success)
               ? 0
               : 1;
}
