// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/runtime/pipeline/pipeline.h>
#include <dftracer/utils/runtime/task_graph/task_graph.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <dftracer/utils/trace/chunk_extractor_utility.h>
#include <dftracer/utils/trace/chunk_verifier_utility.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <ctime>
#include <exception>
#include <mutex>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::task_graph;
using ExtractInput = trace::ChunkExtractorUtilityInput;
using ExtractResult = trace::ChunkExtractorUtilityOutput;
// keep std: Task decodes a vector input parameter only as std::vector
using ExtractChunksOutput = std::vector<ExtractResult>;
namespace gzc = utilities::fileio::compress;

class SplitArgParse : public binaries::ArgParse {
   public:
    binaries::InputArgs inputs{
        binaries::DirMode::DefaultDot,
        "Folder of trace files to split (default: the current folder)"};
    binaries::PipelineArgs pipeline;
    binaries::IndexingArgs member;
    binaries::MemoryArgs memory;
    binaries::WatchdogArgs watchdog;

    dstd::StringView app_name = "app";
    dstd::StringView output_dir = "./split";
    std::size_t chunk_size_bytes = 4ull * 1024 * 1024;
    bool compress = true;
    bool verify = false;

    explicit SplitArgParse(binaries::ArgParser& p) : ArgParse(p) {
        member.with_index_dir = false;
        member.with_force = false;
        member.group = "Output";
        member.checkpoint_help =
            "Gzip member size of the output files in uncompressed bytes "
            "(default: " +
            member.checkpoint_default + "). Accepts units, e.g. 512KB, 4MB";
        memory.help =
            "Most memory the chunk writers hold, with units (e.g. 64GB). A "
            "group of files starts only while the budget holds its writer, "
            "and one group always runs. 0 means a third of the available "
            "memory";
        inputs.folder_only = true;
        schema(inputs, pipeline, member, memory, watchdog);
    }

   protected:
    void register_args() override {
        parser().option(app_name, {"--app-name"},
                        "Application name for output files");

        parser().option(output_dir, {"-o", "--output"},
                        "Output folder for the split files");

        parser().bytes_with_unit(
            chunk_size_bytes, {"--chunk-size"},
            "Output file size, approximate compressed/on-disk, with a unit "
            "(default: 4MB, e.g. 512KB, 4MB)",
            "4MB");

        parser().flag(compress, {"--compress"},
                      "Compress output files with gzip");

        parser().flag(
            verify, {"--verify"},
            "Verify output chunks match input by comparing event IDs");
    }
};

namespace {

struct FileProbe {
    dstd::String path;  ///< The rechunked copy when the input was rechunked  //
                        ///< keep std: gzip and extractor APIs take dstd::String
                        ///< (utilities step)
    std::uint64_t bytes = 0;
    double ratio = 1.0;  ///< Uncompressed bytes per compressed byte
    bool ok = false;
    /// The probe's open descriptor of `path`, kept for the extraction when the
    /// open-file budget allows, else null.
    dstd::SharedPtr<dstd::ScopedFd> fd;
};

struct FileGroup {
    dstd::Vector<dstd::String> files;
    // use std types (tasks step)
    dstd::Vector<dstd::SharedPtr<dstd::ScopedFd>> open_files;
    // (tasks step)
    double ratio = 1.0;
};

// keep std: Task decodes a vector input parameter only as std::vector
using FileGroups = std::vector<FileGroup>;
// keep std: TaskGraph::reduce hands the reducer a std::vector
using FileProbes = std::vector<FileProbe>;

constexpr std::uint64_t RATIO_SAMPLE_BYTES = 1 << 20;

/// A group ends with a short chunk, so it holds several chunks of input, but
/// not so many that a large input has too few groups.
constexpr std::size_t GROUP_MIN_CHUNKS = 4;
constexpr std::size_t GROUP_MAX_CHUNKS = 16;
constexpr double MAX_DEFLATE_RATIO = 1032.0;

struct RechunkDir {
    std::mutex mu;
    dstd::Path path;

    const dstd::Path& get() {
        std::lock_guard<std::mutex> lock(mu);
        if (path.empty()) {
            path = dstd::fs::temp_directory().view() /
                   ("dftracer_rechunk_" + dstd::to_string(std::time(nullptr)) +
                    "_" + dstd::to_string(getpid()));
            common::unwrap(dstd::fs::create_directories(path.view()));
        }
        return path;
    }
};

// The target is a small share of the input when there are many threads, so
// that every thread gets a group.
FileGroups group_files(dstd::Span<const FileProbe> probes,
                       std::size_t chunk_bytes, std::size_t threads) {
    std::uint64_t total = 0;
    for (const auto& probe : probes) {
        if (probe.ok) total += probe.bytes;
    }
    const std::uint64_t target = std::clamp<std::uint64_t>(
        total / (threads * 2), chunk_bytes * GROUP_MIN_CHUNKS,
        chunk_bytes * GROUP_MAX_CHUNKS);

    FileGroups groups;
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
        group.open_files.push_back(probe.fd);
        group_bytes += static_cast<double>(probe.bytes);
        group_plain += static_cast<double>(probe.bytes) * probe.ratio;
        if (group_bytes >= static_cast<double>(target)) close_group();
    }
    if (!group.files.empty()) close_group();
    return groups;
}

// A chunk file ends at the first member end at or past its size, so aiming half
// a member low ends it nearest the target.
std::size_t chunk_uncompressed_bytes(std::size_t chunk_bytes, double ratio,
                                     std::size_t member_bytes) {
    const double aim = static_cast<double>(chunk_bytes) * ratio -
                       static_cast<double>(member_bytes) / 2;
    return static_cast<std::size_t>(std::max(aim, 1.0));
}

coro::CoroTask<std::size_t> hash_events(const dstd::String& path) {
    // std::string (utilities step)
    std::size_t content_hash = 0;
    utilities::hash::HasherUtility hasher;
    auto line_gen = utilities::fileio::lines::sources::async_streaming_gz_lines(
        dstd::String(path));
    for (;;) {
        auto line_opt = co_await line_gen.next();
        if (!line_opt) break;
        const auto& line = *line_opt;
        const dstd::StringView event = trace::event_of_line(
            dstd::StringView(line.content.data(), line.content.length()));
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
    const auto log_dir = binaries::absolute_path(cli->inputs.directory);
    const auto output_dir = binaries::absolute_path(cli->output_dir);
    const dstd::String app_name(cli->app_name);
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

    if (!common::unwrap(dstd::fs::exists(dstd::PathView(output_dir)))) {
        common::unwrap(
            dstd::fs::create_directories(dstd::PathView(output_dir)));
    }

    auto pipeline_config = binaries::build_pipeline_config(
        "DFTracer Split", cli->pipeline, cli->watchdog);
    Pipeline pipeline(pipeline_config);

    auto start_time = std::chrono::high_resolution_clock::now();

    DFTRACER_UTILS_LOG_INFO("%s", "Discovering input files...");

    dstd::Vector<dstd::String> input_files;
    {
        auto matched =
            binaries::scan_trace_files_on_pool(cli->pipeline, log_dir.str());
        // Never read back this run's own output. A file is tested by its walked
        // path, so a symlink into the output directory from outside its
        // spelling is not caught.
        // A root that cannot be resolved excludes nothing.
        const auto canonical =
            dstd::fs::weakly_canonical(dstd::PathView(output_dir));
        const dstd::Path out_canonical = canonical ? *canonical : dstd::Path();
        const auto walked = dstd::fs::absolute(dstd::PathView(output_dir));
        const dstd::Path out_walked =
            walked ? dstd::normalize(walked->view()) : dstd::Path();
        auto inside = [](dstd::PathView p, dstd::PathView root) {
            if (root.empty()) return false;
            const dstd::Path rel = dstd::relative(p, root);
            const dstd::StringView text = rel.str();
            return !text.empty() && text != ".." && !text.starts_with("../");
        };
        for (const auto& entry : matched) {
            if (!utilities::filesystem::is_trace_file(entry.path.str())) {
                continue;
            }
            if (inside(entry.path.view(), out_canonical.view()) ||
                inside(entry.path.view(), out_walked.view()))
                continue;
            input_files.emplace_back(entry.path.str());
        }
    }

    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No trace files found in %s", log_dir.c_str());
        co_return 1;
    }
    bool plain_input = false;
    for (const auto& path : input_files) {
        if (!path.ends_with(".gz")) {
            DFTRACER_UTILS_LOG_ERROR(
                "%s is not gzip; dftracer_split reads gzip traces only. Gzip "
                "it first with dftracer_pgzip",
                path.c_str());
            plain_input = true;
        }
    }
    if (plain_input) co_return 1;
    std::sort(input_files.begin(), input_files.end());

    DFTRACER_UTILS_LOG_INFO("Found %zu input files", input_files.size());

    auto graph = TaskGraph::builder(
        {.name = "DFTracerSplit", .max_concurrency = executor_threads});

    auto start_task = make_task(
        [](CoroScope&) -> coro::CoroTask<void> { co_return; }, "Start");
    graph.add(start_task);

    // The read paths decode a whole member at once, so a single huge gzip
    // member is rechunked to bounded multi-member gzip first.
    RechunkDir rechunk_dir;
    auto* rechunk_dir_ptr = &rechunk_dir;
    auto* input_files_ptr = &input_files;
    std::atomic<std::size_t> open_count{0};
    auto* open_count_ptr = &open_count;
    const std::size_t open_budget = binaries::open_file_budget();
    auto file_probes = graph.parallel<FileProbe>(
        input_files.size(),
        [input_files_ptr, rechunk_dir_ptr, member_size_bytes, open_count_ptr,
         open_budget](CoroScope&,
                      std::size_t idx) -> coro::CoroTask<FileProbe> {
            FileProbe probe;
            probe.path = (*input_files_ptr)[idx];
            ssize_t fd = co_await io::open(probe.path.c_str(), O_RDONLY);
            if (fd < 0) {
                DFTRACER_UTILS_LOG_ERROR("Cannot open %s", probe.path.c_str());
                co_return probe;
            }
            dstd::ScopedFd sfd(static_cast<int>(fd));
            struct stat st;
            if (::fstat(sfd.get(), &st) != 0 || st.st_size == 0) {
                co_return probe;
            }
            probe.bytes = static_cast<std::uint64_t>(st.st_size);
            const gzc::FirstMember first = co_await gzc::gzip_first_member(
                sfd.get(), probe.bytes, gzc::RECHUNK_MEMBER_CAP,
                RATIO_SAMPLE_BYTES);
            if (first.compressed > 0) {
                probe.ratio =
                    std::clamp(static_cast<double>(first.uncompressed) /
                                   static_cast<double>(first.compressed),
                               1.0, MAX_DEFLATE_RATIO);
            }
            if (!first.exceeds_cap &&
                open_count_ptr->fetch_add(1, std::memory_order_relaxed) <
                    open_budget) {
                probe.fd = dstd::make_shared<dstd::ScopedFd>(std::move(sfd));
            } else {
                sfd.reset();
            }
            if (first.exceeds_cap) {
                // (utilities step)
                dstd::String out((rechunk_dir_ptr->get().view() /
                                  dstd::PathView(probe.path).filename())
                                     .str());
                DFTRACER_UTILS_LOG_WARN(
                    "Input %s is a single large gzip member; rechunking to "
                    "bounded members at %s",
                    probe.path.c_str(), out.c_str());
                co_await gzc::gzip_rechunk_to_members(dstd::String(probe.path),
                                                      dstd::String(out),
                                                      member_size_bytes, 6);
                probe.path = out;
                probe.bytes =
                    common::unwrap(dstd::fs::file_size(dstd::PathView(out)));
            }
            probe.ok = true;
            co_return probe;
        },
        {.name = "ProbeFile"});

    for (const auto& probe_task : file_probes.tasks()) {
        probe_task->depends_on(start_task);
    }

    dstd::Vector<dstd::String> split_files;
    auto* split_files_ptr = &split_files;
    auto groups_task = graph.reduce<FileGroups>(
        file_probes, split_every{input_files.size()},
        [chunk_size_bytes, executor_threads, split_files_ptr](
            CoroScope&, FileProbes probes) -> coro::CoroTask<FileGroups> {
            auto groups = group_files(
                dstd::Span<const FileProbe>(probes.data(), probes.size()),
                chunk_size_bytes, executor_threads);
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
    const dstd::String output_dir_text(output_dir.str());
    // std::string (utilities step)
    auto* output_dir_ptr = &output_dir_text;

    ExtractChunksOutput split_results;
    auto* split_results_ptr = &split_results;

    auto task_extract_chunks = make_task(
        [app_name_ptr, output_dir_ptr, split_results_ptr, compress, verify,
         executor_threads, member_size_bytes, chunk_size_bytes, memory_budget](
            CoroScope&,
            FileGroups groups) -> coro::CoroTask<ExtractChunksOutput> {
            co_return co_await coro::with_scope(
                [&](coro::Scope& scope) -> coro::CoroTask<ExtractChunksOutput> {
                    DFTRACER_UTILS_LOG_INFO(
                        "Splitting %zu groups in parallel...", groups.size());

                    // A running group holds its writer's buffers and its
                    // compress workers use every core, so few groups run at
                    // once, within the memory budget.
                    const std::size_t max_parallel =
                        std::max<std::size_t>(2, executor_threads / 4);
                    const auto extraction = trace::plan_extraction_budget(
                        memory_budget, max_parallel, executor_threads,
                        member_size_bytes);
                    trace::ExtractionAdmission admission(
                        memory_budget, extraction.reserve_bytes, max_parallel);
                    auto* admission_ptr = &admission;

                    dstd::Vector<coro::SpawnFuture<ExtractResult>> futures;
                    futures.reserve(groups.size());

                    for (std::size_t i = 0; i < groups.size(); ++i) {
                        // Chunk files are sized by compressed bytes and the
                        // writer counts uncompressed ones.
                        const double ratio = compress ? groups[i].ratio : 1.0;
                        auto input =
                            ExtractInput::from_files(static_cast<int>(i + 1),
                                                     std::move(groups[i].files))
                                .with_output_dir(*output_dir_ptr)
                                .with_open_files(
                                    std::move(groups[i].open_files))
                                .with_app_name(*app_name_ptr)
                                .with_compression(compress)
                                .with_compute_hash(verify)
                                .with_member_size(member_size_bytes)
                                .with_memory_budget(extraction.writer_bytes)
                                .with_chunk_size(chunk_uncompressed_bytes(
                                    chunk_size_bytes, ratio,
                                    member_size_bytes));

                        futures.push_back(scope.spawn_future(
                            [input = std::move(input),
                             admission_ptr]() -> coro::CoroTask<ExtractResult> {
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

                    // Every future is awaited before a raise, since the
                    // children use `admission`, which dies with this frame.
                    ExtractChunksOutput results;
                    results.reserve(futures.size());
                    std::exception_ptr error;
                    for (auto& future : futures) {
                        try {
                            results.push_back(co_await future);
                        } catch (...) {
                            if (!error) error = std::current_exception();
                        }
                    }
                    if (error) std::rethrow_exception(error);

                    std::sort(
                        results.begin(), results.end(),
                        [](const ExtractResult& a, const ExtractResult& b) {
                            return a.chunk_index < b.chunk_index;
                        });

                    *split_results_ptr = results;
                    co_return results;
                });
        },
        "ExtractChunks");

    task_extract_chunks->depends_on(groups_task.task());
    graph.add(task_extract_chunks);

    dstd::SharedPtr<Task> final_task = task_extract_chunks;
    dstd::SharedPtr<Task> task_verify_chunks = nullptr;

    if (verify) {
        DFTRACER_UTILS_LOG_INFO("%s", "Configuring verification...");

        task_verify_chunks = make_task(
            [split_files_ptr, executor_threads](
                CoroScope&, const ExtractChunksOutput& chunks)
                -> coro::CoroTask<trace::ChunkVerificationUtilityOutput> {
                co_return co_await coro::with_scope(
                    [&](coro::Scope& scope)
                        -> coro::CoroTask<
                            trace::ChunkVerificationUtilityOutput> {
                        std::size_t output_hash = 0;
                        for (const auto& chunk : chunks) {
                            output_hash += chunk.event_hash;
                        }

                        coro::CoroSemaphore running(executor_threads);
                        auto* running_ptr = &running;
                        dstd::Vector<coro::SpawnFuture<std::size_t>> futures;
                        futures.reserve(split_files_ptr->size());
                        for (const auto& file : *split_files_ptr) {
                            const dstd::String* path = &file;
                            futures.push_back(scope.spawn_future(
                                [path,
                                 running_ptr]() -> coro::CoroTask<std::size_t> {
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

                        // Every future is awaited before a raise, since the
                        // children use `running`, which dies with this frame.
                        std::size_t input_hash = 0;
                        std::exception_ptr error;
                        for (auto& future : futures) {
                            try {
                                input_hash += co_await future;
                            } catch (...) {
                                if (!error) error = std::current_exception();
                            }
                        }
                        if (error) std::rethrow_exception(error);

                        co_return trace::ChunkVerificationUtilityOutput::
                            success(static_cast<std::uint64_t>(input_hash),
                                    static_cast<std::uint64_t>(output_hash));
                    });
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

    if (!rechunk_dir.path.empty() &&
        common::unwrap(dstd::fs::exists(rechunk_dir.path.view()))) {
        DFTRACER_UTILS_LOG_INFO("Cleaning up temporary rechunk directory: %s",
                                rechunk_dir.path.c_str());
        common::unwrap(dstd::fs::remove_all(rechunk_dir.path.view()));
    }

    co_return exit_code;
}

int main(int argc, char** argv) {
    return binaries::cli_main<SplitArgParse>(
        argc, argv, "dftracer_split",
        "Split DFTracer traces into equal-sized chunks using explicit pipeline "
        "with maximum parallelism",
        [](SplitArgParse& cli) { return run_split(&cli).get(); });
}
