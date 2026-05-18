#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reconstructor_utility.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "common_cli.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::reorganize;

class ReconstructArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{cli::DirMode::REQUIRED,
                                 "Directory containing reorganized files"};
    cli::PipelineArgs pipeline;

    std::size_t checkpoint_size = 0;
    std::string output_dir;
    bool no_compress = false;

    explicit ReconstructArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        schema(directory, pipeline);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("--checkpoint-size")
            .help("Checkpoint size for gzip indexing in bytes (default: " +
                  std::to_string(constants::indexer::DEFAULT_CHECKPOINT_SIZE) +
                  ")")
            .scan<'d', std::size_t>()
            .default_value(static_cast<std::size_t>(
                constants::indexer::DEFAULT_CHECKPOINT_SIZE));

        parser()
            .add_argument("-o", "--output")
            .help("Output directory")
            .required();

        parser()
            .add_argument("--no-compress")
            .help("Write plain .pfw instead of .pfw.gz")
            .flag();
    }

    void post_parse() override {
        checkpoint_size = parser().get<std::size_t>("--checkpoint-size");
        output_dir = parser().get<std::string>("--output");
        no_compress = parser().get<bool>("--no-compress");
    }
};

static coro::CoroTask<int> run_reconstruct(const ReconstructArgParse* cli,
                                           CoroScope& scope) {
    std::printf("==========================================\n");
    std::printf("DFTracer Trace Reconstructor\n");
    std::printf("==========================================\n");
    std::printf("  Input directory: %s\n", cli->directory.value.c_str());
    std::printf("  Output directory: %s\n", cli->output_dir.c_str());
    std::printf("  Compress: %s\n", cli->no_compress ? "false" : "true");
    std::printf("  Executor threads: %zu\n", cli->pipeline.executor_threads);
    std::printf("==========================================\n\n");

    auto start_time = std::chrono::high_resolution_clock::now();

    ReconstructorInput input;
    input.input_dir = cli->directory.value;
    input.output_dir = cli->output_dir;
    input.checkpoint_size = cli->checkpoint_size;
    input.parallelism = cli->pipeline.executor_threads;
    input.compress = !cli->no_compress;

    ReconstructorUtility reconstructor;
    auto result = co_await scope.spawn(reconstructor, std::move(input));

    if (!result.success) {
        DFTRACER_UTILS_LOG_ERROR("Reconstruction failed: %s",
                                 result.error_message.c_str());
        co_return 1;
    }

    for (const auto& file : result.files) {
        std::string fname = fs::path(file.output_path).filename().string();
        std::printf("  %s: %zu events\n", fname.c_str(), file.events_written);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::printf("\n==========================================\n");
    std::printf("Reconstruction Complete\n");
    std::printf("==========================================\n");
    std::printf("  Time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Files reconstructed: %zu\n", result.files.size());
    std::printf("  Total events: %zu\n", result.total_events);
    std::printf("==========================================\n");

    co_return 0;
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_reconstruct",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Reconstruct original trace files from reorganized output.");

    ReconstructArgParse cli(program);
    cli.setup();
    if (!cli.parse(argc, argv)) return 1;

    fs::create_directories(cli.output_dir);

    auto pipeline_config =
        cli::build_pipeline_config("Reconstruct", cli.pipeline);
    Pipeline pipeline(pipeline_config);

    int exit_code = 0;
    auto* cli_ptr = &cli;
    auto task = make_task(
        [cli_ptr, &exit_code](CoroScope& scope) -> coro::CoroTask<void> {
            exit_code = co_await run_reconstruct(cli_ptr, scope);
        },
        "ReconstructMain");

    pipeline.set_source(task);
    pipeline.set_destination(task);
    pipeline.execute();

    return exit_code;
}
