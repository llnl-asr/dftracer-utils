#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/scoped_fd.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/coro/async_semaphore.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/runtime/pipeline/pipeline.h>
#include <dftracer/utils/runtime/task_graph/task_graph.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <dftracer/utils/trace/chunk_extractor_utility.h>
#include <dftracer/utils/trace/chunk_verifier_utility.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <ctime>
#include <mutex>

using namespace dftracer::utils;
using namespace dftracer::utils::task_graph;
using ExtractInput = trace::ChunkExtractorUtilityInput;
using ExtractResult = trace::ChunkExtractorUtilityOutput;
using ExtractChunksOutput = std::vector<ExtractResult>;
namespace gzc = utilities::fileio::compress;

class SplitArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory;
    cli::PipelineArgs pipeline;
    cli::IndexingArgs member;
    cli::MemoryArgs memory;
    cli::WatchdogArgs watchdog;

    std::string app_name = "app";
    std::string output_dir = "./split";
    std::size_t chunk_size_bytes = 4ull * 1024 * 1024;
    bool compress = true;
    bool verify = false;

    explicit SplitArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        member.with_index_dir = false;
        member.with_force = false;
        member.group = "Output";
        member.checkpoint_help =
            "Gzip member size of the output files in uncompressed bytes "
            "(default: " +
            std::to_string(constants::indexer::DEFAULT_CHECKPOINT_SIZE) +
            "). Accepts units, e.g. 512KB, 4MB";
        memory.help =
            "Most memory the chunk writers hold, with units (e.g. 64GB). A "
            "group of files starts only while the budget holds its writer, "
            "and one group always runs. 0 means a third of the available "
            "memory";
        schema(directory, pipeline, member, memory, watchdog);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("-n", "--app-name")
            .help("Application name for output files")
            .default_value<std::string>("app");

        parser()
            .add_argument("-o", "--output")
            .help("Output directory for split files")
            .default_value<std::string>("./split");

        parser()
            .add_argument("-s", "--chunk-size")
            .help(
                "Output file size, approximate compressed/on-disk (default: "
                "4MB). A bare number is MB; suffixed values (e.g. 512KB) are "
                "absolute")
            .default_value(std::string("4"));

        parser()
            .add_argument("-c", "--compress")
            .help("Compress output files with gzip")
            .flag()
            .default_value(true);

        parser()
            .add_argument("--verify")
            .help("Verify output chunks match input by comparing event IDs")
            .flag();
    }

    void post_parse() override {
        app_name = parser().get<std::string>("--app-name");
        output_dir = parser().get<std::string>("--output");
        chunk_size_bytes =
            cli::get_bytes_arg(parser(), "--chunk-size", 1024ull * 1024);
        compress = parser().get<bool>("--compress");
        verify = parser().get<bool>("--verify");
    }
};

namespace {

/// A trace file ready to read and what its first gzip member says about it.
struct FileProbe {
    std::string path;    ///< The rechunked copy when the input was rechunked
    std::uint64_t bytes = 0;
    double ratio = 1.0;  ///< Uncompressed bytes per compressed byte
    bool ok = false;
};

/// Files that one writer streams, in order, into chunk files.
struct FileGroup {
    std::vector<std::string> files;
    double ratio = 1.0;
};

/// Compressed bytes of a file decoded to estimate its compression ratio.
constexpr std::uint64_t RATIO_SAMPLE_BYTES = 1 << 20;

/// A group ends with a chunk that is usually short, so a group holds several
/// chunks of input, and not so many that a large input has too few groups.
constexpr std::size_t GROUP_MIN_CHUNKS = 4;
constexpr std::size_t GROUP_MAX_CHUNKS = 16;
constexpr double MAX_DEFLATE_RATIO = 1032.0;

struct RechunkDir {
    std::mutex mu;
    std::string path;

    const std::string& get() {
        std::lock_guard<std::mutex> lock(mu);
        if (path.empty()) {
            path = fs::temp_directory_path() /
                   ("dftracer_rechunk_" + std::to_string(std::time(nullptr)) +
                    "_" + std::to_string(getpid()));
            fs::create_directories(path);
        }
        return path;
    }
};

// A group takes its files until it holds `target` compressed bytes. The target
// is a small share of the input when there are many threads, so that every
// thread gets a group.
std::vector<FileGroup> group_files(const std::vector<FileProbe>& probes,
                                   std::size_t chunk_bytes,
                                   std::size_t threads) {
    std::uint64_t total = 0;
    for (const auto& probe : probes) {
        if (probe.ok) total += probe.bytes;
    }
    const std::uint64_t target = std::clamp<std::uint64_t>(
        total / (threads * 2), chunk_bytes * GROUP_MIN_CHUNKS,
        chunk_bytes * GROUP_MAX_CHUNKS);

    std::vector<FileGroup> groups;
    FileGroup group;
    double group_bytes = 0;
    double group_plain = 0;
    auto close_group = [&] {
        group.ratio = group_plain / group_bytes;
        groups.push_back(std::move(group));
        group = FileGroup();
        group_bytes = 0;
        group_plain = 0;
    };
    for (const auto& probe : probes) {
        if (!probe.ok) continue;
        group.files.push_back(probe.path);
        group_bytes += static_cast<double>(probe.bytes);
        group_plain += static_cast<double>(probe.bytes) * probe.ratio;
        if (group_bytes >= static_cast<double>(target)) close_group();
    }
    if (!group.files.empty()) close_group();
    return groups;
}

// A chunk file ends at the first member end at or past its size, so aiming half
// a member low puts the end at the member end nearest to the target.
std::size_t chunk_uncompressed_bytes(std::size_t chunk_bytes, double ratio,
                                     std::size_t member_bytes) {
    const double aim = static_cast<double>(chunk_bytes) * ratio -
                       static_cast<double>(member_bytes) / 2;
    return static_cast<std::size_t>(std::max(aim, 1.0));
}

coro::CoroTask<std::size_t> hash_events(const std::string& path) {
    std::size_t content_hash = 0;
    utilities::hash::HasherUtility hasher;
    auto line_gen =
        utilities::fileio::lines::sources::async_streaming_gz_lines(path);
    for (;;) {
        auto line_opt = co_await line_gen.next();
        if (!line_opt) break;
        const auto& line = *line_opt;
        const std::string_view event = trace::event_of_line(
            std::string_view(line.content.data(), line.content.length()));
        if (!event.empty()) {
            hasher.reset();
            hasher.update(event);
            content_hash += hasher.get_hash().value;
        }
    }
    co_return content_hash;
}

}  // namespace

static coro::CoroTask<int> run_split(const SplitArgParse* cli) {
    const auto log_dir = fs::absolute(cli->directory.value).string();
    const auto output_dir = fs::absolute(cli->output_dir).string();
    const auto& app_name = cli->app_name;
    const auto chunk_size_bytes = cli->chunk_size_bytes;
    const double chunk_size_mb =
        static_cast<double>(chunk_size_bytes) / (1024.0 * 1024.0);
    const auto compress = cli->compress;
    const auto verify = cli->verify;
    const std::size_t member_size_bytes = cli->member.checkpoint_size;
    const auto executor_threads = cli->pipeline.executor_threads;
    const std::uint64_t memory_budget =
        memory::resolve_spill_budget(cli->memory.memory_budget);

    std::printf("==========================================\n");
    std::printf("DFTracer Split (Explicit Pipeline)\n");
    std::printf("==========================================\n");
    std::printf("Arguments:\n");
    std::printf("  App name: %s\n", app_name.c_str());
    std::printf("  Compress: %s\n", compress ? "true" : "false");
    std::printf("  Data dir: %s\n", log_dir.c_str());
    std::printf("  Output dir: %s\n", output_dir.c_str());
    std::printf("  Chunk size: %.2f MB (compressed)\n", chunk_size_mb);
    std::printf("  Gzip member size: %zu bytes\n", member_size_bytes);
    std::printf("  Executor threads: %zu\n", executor_threads);
    std::printf("  Memory budget: %" PRIu64 " bytes\n", memory_budget);
    std::printf("==========================================\n\n");

    if (!fs::exists(output_dir)) {
        fs::create_directories(output_dir);
    }

    auto pipeline_config = cli::build_pipeline_config(
        "DFTracer Split", cli->pipeline, cli->watchdog);
    Pipeline pipeline(pipeline_config);

    auto start_time = std::chrono::high_resolution_clock::now();

    DFTRACER_UTILS_LOG_INFO("%s", "Discovering input files...");

    std::vector<std::string> input_files;
    for (const auto& entry : fs::directory_iterator(log_dir)) {
        if (entry.is_regular_file()) {
            std::string path = entry.path().string();
            if (utilities::filesystem::is_trace_file(path)) {
                input_files.push_back(path);
            }
        }
    }

    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No trace files found in %s", log_dir.c_str());
        co_return 1;
    }
    std::sort(input_files.begin(), input_files.end());

    DFTRACER_UTILS_LOG_INFO("Found %zu input files", input_files.size());

    auto graph = TaskGraph::builder(
        {.name = "DFTracerSplit", .max_concurrency = executor_threads});

    auto start_task = make_task(
        [](CoroScope&) -> coro::CoroTask<void> { co_return; }, "Start");
    graph.add(start_task);

    // A single huge gzip member cannot be read with bounded memory (the read
    // paths decode a whole member at once). Rechunk any such input to bounded
    // multi-member gzip first and split the rechunked copy. Regular
    // multi-member traces are left untouched.
    RechunkDir rechunk_dir;
    auto* rechunk_dir_ptr = &rechunk_dir;
    auto* input_files_ptr = &input_files;
    auto file_probes = graph.parallel<FileProbe>(
        input_files.size(),
        [input_files_ptr, rechunk_dir_ptr, member_size_bytes](
            CoroScope&, std::size_t idx) -> coro::CoroTask<FileProbe> {
            FileProbe probe;
            probe.path = (*input_files_ptr)[idx];
            if (!probe.path.ends_with(".gz")) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Not a gzip trace: %s (dftracer traces must be "
                    "gzip-compressed)",
                    probe.path.c_str());
                co_return probe;
            }
            ssize_t fd = co_await io::open(probe.path.c_str(), O_RDONLY);
            if (fd < 0) {
                DFTRACER_UTILS_LOG_ERROR("Cannot open %s", probe.path.c_str());
                co_return probe;
            }
            ScopedFd sfd(static_cast<int>(fd));
            struct stat st;
            if (::fstat(sfd.get(), &st) != 0 || st.st_size == 0) {
                co_return probe;
            }
            probe.bytes = static_cast<std::uint64_t>(st.st_size);
            const gzc::FirstMember first = co_await gzc::gzip_first_member(
                sfd.get(), probe.bytes, gzc::RECHUNK_MEMBER_CAP,
                RATIO_SAMPLE_BYTES);
            sfd.reset();
            if (first.compressed > 0) {
                probe.ratio =
                    std::clamp(static_cast<double>(first.uncompressed) /
                                   static_cast<double>(first.compressed),
                               1.0, MAX_DEFLATE_RATIO);
            }
            if (first.exceeds_cap) {
                std::string out = rechunk_dir_ptr->get() + "/" +
                                  fs::path(probe.path).filename().string();
                DFTRACER_UTILS_LOG_WARN(
                    "Input %s is a single large gzip member; rechunking to "
                    "bounded members at %s",
                    probe.path.c_str(), out.c_str());
                co_await gzc::gzip_rechunk_to_members(probe.path, out,
                                                      member_size_bytes, 6);
                probe.path = out;
                probe.bytes = fs::file_size(out);
            }
            probe.ok = true;
            co_return probe;
        },
        {.name = "ProbeFile"});

    for (const auto& probe_task : file_probes.tasks()) {
        probe_task->depends_on(start_task);
    }

    std::vector<std::string> split_files;
    auto* split_files_ptr = &split_files;
    auto groups_task = graph.reduce<std::vector<FileGroup>>(
        file_probes, split_every{input_files.size()},
        [chunk_size_bytes, executor_threads, split_files_ptr](
            CoroScope&, std::vector<FileProbe> probes)
            -> coro::CoroTask<std::vector<FileGroup>> {
            auto groups =
                group_files(probes, chunk_size_bytes, executor_threads);
            for (const auto& group : groups) {
                split_files_ptr->insert(split_files_ptr->end(),
                                        group.files.begin(), group.files.end());
            }
            DFTRACER_UTILS_LOG_INFO("Split %zu files in %zu groups",
                                    split_files_ptr->size(), groups.size());
            co_return groups;
        },
        {.name = "GroupFiles"});

    auto* app_name_ptr = &app_name;
    auto* output_dir_ptr = &output_dir;

    // The verify task takes over the result of the extract task, so the
    // report reads this copy.
    ExtractChunksOutput split_results;
    auto* split_results_ptr = &split_results;

    auto task_extract_chunks = make_task(
        [app_name_ptr, output_dir_ptr, split_results_ptr, compress, verify,
         executor_threads, member_size_bytes, chunk_size_bytes,
         memory_budget](CoroScope& scope, std::vector<FileGroup> groups)
            -> coro::CoroTask<ExtractChunksOutput> {
            DFTRACER_UTILS_LOG_INFO("Splitting %zu groups in parallel...",
                                    groups.size());

            // Each running group holds the buffers of its writer until it is
            // done, and its compress workers already use every core, so only
            // a few groups run at once, and only as many as the memory budget
            // holds.
            const std::size_t max_parallel =
                std::max<std::size_t>(2, executor_threads / 4);
            const auto extraction = trace::plan_extraction_budget(
                memory_budget, max_parallel, executor_threads,
                member_size_bytes);
            trace::ExtractionAdmission admission(
                memory_budget, extraction.reserve_bytes, max_parallel);
            auto* admission_ptr = &admission;

            std::vector<coro::SpawnFuture<ExtractResult>> futures;
            futures.reserve(groups.size());

            for (std::size_t i = 0; i < groups.size(); ++i) {
                // Chunk files are sized by compressed bytes and the writer
                // counts uncompressed ones.
                const double ratio = compress ? groups[i].ratio : 1.0;
                auto input =
                    ExtractInput::from_files(static_cast<int>(i + 1),
                                             std::move(groups[i].files))
                        .with_output_dir(*output_dir_ptr)
                        .with_app_name(*app_name_ptr)
                        .with_compression(compress)
                        .with_compute_hash(verify)
                        .with_member_size(member_size_bytes)
                        .with_memory_budget(extraction.writer_bytes)
                        .with_chunk_size(chunk_uncompressed_bytes(
                            chunk_size_bytes, ratio, member_size_bytes));

                futures.push_back(scope.spawn(
                    [input = std::move(input), admission_ptr](
                        CoroScope&) -> coro::CoroTask<ExtractResult> {
                        co_await admission_ptr->enter();
                        try {
                            trace::ChunkExtractorUtility extractor;
                            auto result = co_await extractor(input);
                            admission_ptr->leave();
                            co_return result;
                        } catch (...) {
                            admission_ptr->leave();
                            throw;
                        }
                    }));
            }

            ExtractChunksOutput results;
            results.reserve(futures.size());
            for (auto& future : futures) {
                results.push_back(co_await future);
            }

            std::sort(results.begin(), results.end(),
                      [](const ExtractResult& a, const ExtractResult& b) {
                          return a.chunk_index < b.chunk_index;
                      });

            *split_results_ptr = results;
            co_return results;
        },
        "ExtractChunks");

    task_extract_chunks->depends_on(groups_task.task());
    graph.add(task_extract_chunks);

    std::shared_ptr<Task> final_task = task_extract_chunks;
    std::shared_ptr<Task> task_verify_chunks = nullptr;

    if (verify) {
        DFTRACER_UTILS_LOG_INFO("%s", "Configuring verification...");

        task_verify_chunks = make_task(
            [split_files_ptr, executor_threads](
                CoroScope& scope, const ExtractChunksOutput& chunks)
                -> coro::CoroTask<trace::ChunkVerificationUtilityOutput> {
                std::size_t output_hash = 0;
                for (const auto& chunk : chunks) {
                    output_hash += chunk.event_hash;
                }

                coro::CoroSemaphore running(executor_threads);
                auto* running_ptr = &running;
                std::vector<coro::SpawnFuture<std::size_t>> futures;
                futures.reserve(split_files_ptr->size());
                for (const auto& file : *split_files_ptr) {
                    const std::string* path = &file;
                    futures.push_back(scope.spawn(
                        [path, running_ptr](
                            CoroScope&) -> coro::CoroTask<std::size_t> {
                            co_await running_ptr->acquire(1);
                            try {
                                const std::size_t file_hash =
                                    co_await hash_events(*path);
                                running_ptr->release(1);
                                co_return file_hash;
                            } catch (...) {
                                running_ptr->release(1);
                                throw;
                            }
                        }));
                }

                std::size_t input_hash = 0;
                for (auto& future : futures) {
                    input_hash += co_await future;
                }

                co_return trace::ChunkVerificationUtilityOutput::success(
                    static_cast<std::uint64_t>(input_hash),
                    static_cast<std::uint64_t>(output_hash));
            },
            "VerifyChunks");

        task_verify_chunks->depends_on(task_extract_chunks);
        graph.add(task_verify_chunks);
        final_task = task_verify_chunks;
    }

    DFTRACER_UTILS_LOG_INFO("%s", "Executing pipeline...");

    pipeline.set_source(start_task);
    pipeline.set_destination(final_task);
    pipeline.execute();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Split Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Input: %zu files\n", input_files.size());

    int exit_code = 0;

    std::size_t chunk_files = 0;
    std::size_t total_events = 0;
    for (const auto& result : split_results) {
        if (result.success) {
            chunk_files += result.chunks.size();
            total_events += result.events;
        } else {
            DFTRACER_UTILS_LOG_ERROR("Failed to split group %d",
                                     result.chunk_index);
            exit_code = 1;
        }
    }
    std::printf("  Output: %zu chunk files, %zu events\n", chunk_files,
                total_events);
    if (chunk_files == 0) {
        DFTRACER_UTILS_LOG_ERROR("%s", "No chunk files were written");
        exit_code = 1;
    }

    if (verify && task_verify_chunks) {
        auto verify_result =
            task_verify_chunks->get<trace::ChunkVerificationUtilityOutput>();

        if (verify_result.input_hash == verify_result.output_hash) {
            std::printf(
                "  Verification: PASSED - all events present in output\n");
        } else {
            std::printf("  Verification: FAILED - event mismatch detected\n");
            exit_code = 1;
        }
        std::printf("    Input hash:  0x%016" PRIx64 "\n",
                    verify_result.input_hash);
        std::printf("    Output hash: 0x%016" PRIx64 "\n",
                    verify_result.output_hash);
    }

    std::printf("==========================================\n");

    if (!rechunk_dir.path.empty() && fs::exists(rechunk_dir.path)) {
        DFTRACER_UTILS_LOG_INFO("Cleaning up temporary rechunk directory: %s",
                                rechunk_dir.path.c_str());
        fs::remove_all(rechunk_dir.path);
    }

    co_return exit_code;
}

int main(int argc, char** argv) {
    return cli::cli_main<SplitArgParse>(
        argc, argv, "dftracer_split",
        "Split DFTracer traces into equal-sized chunks using explicit pipeline "
        "with maximum parallelism",
        [](SplitArgParse& cli) { return run_split(&cli).get(); });
}
