// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/combinators.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/stage.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/scope_exit.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/io/io.h>
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
#include <exception>
#include <mutex>

using namespace dftracer::utils;
using ExtractInput = trace::ChunkExtractorUtilityInput;
using ExtractResult = trace::ChunkExtractorUtilityOutput;
using ExtractChunksOutput = dstd::Vector<ExtractResult>;

class SplitArgParse : public binaries::ArgParse {
   public:
    binaries::InputArgs inputs{
        binaries::DirMode::DefaultDot,
        "Folder of trace files to split (default: the current folder)"};
    binaries::RuntimeArgs runtime_args;
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
        member.checkpoint_help = dstd::fmt<
            "Gzip member size of the output files in uncompressed bytes "
            "(default: {}). Accepts units, e.g. 512KB, 4MB">(
            member.checkpoint_default);
        memory.help =
            "Most memory the chunk writers hold, with units (e.g. 64GB). A "
            "group of files starts only while the budget holds its writer, "
            "and one group always runs. 0 means a third of the available "
            "memory";
        inputs.folder_only = true;
        schema(inputs, runtime_args, member, memory, watchdog);
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
    dstd::String path;   ///< The rechunked copy when the input was rechunked
    std::uint64_t bytes = 0;
    double ratio = 1.0;  ///< Uncompressed bytes per compressed byte
    bool ok = false;
    /// The probe's open descriptor of `path`, kept for the extraction when the
    /// open-file budget allows, else null.
    dstd::SharedPtr<dstd::File> fd;
};

struct FileGroup {
    dstd::Vector<dstd::String> files;
    // use std types (tasks step)
    dstd::Vector<dstd::SharedPtr<dstd::File>> open_files;
    // (tasks step)
    double ratio = 1.0;
};

using FileGroups = dstd::Vector<FileGroup>;
using FileProbes = dstd::Vector<FileProbe>;

constexpr std::uint64_t RATIO_SAMPLE_BYTES = 1 << 20;

/// A group ends with a short chunk, so it holds several chunks of input, but
/// not so many that a large input has too few groups.
constexpr std::size_t GROUP_MIN_CHUNKS = 4;
constexpr std::size_t GROUP_MAX_CHUNKS = 16;
constexpr double MAX_DEFLATE_RATIO = 1032.0;

struct RechunkDir {
    std::mutex mu;
    dstd::Path path;

    dstd::Result<dstd::Path> get() {
        std::lock_guard<std::mutex> lock(mu);
        if (path.empty()) {
            const dstd::String name = dstd::fmt<"dftracer_rechunk_{}_{}">(
                std::time(nullptr), getpid());
            path = dstd::fs::temp_directory().view() / dstd::PathView(name);
            auto created = dstd::fs::create_directories(path.view());
            if (!created.has_value()) {
                path = dstd::Path();
                return created.error();
            }
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

coro::CoroTask<dstd::Result<std::size_t>> hash_events(
    const dstd::String& path) {
    // std::string (utilities step)
    std::size_t content_hash = 0;
    utilities::hash::HasherUtility hasher;
    auto line_gen = utilities::fileio::lines::sources::async_streaming_gz_lines(
        dstd::String(path));
    for (;;) {
        auto line_opt = co_await line_gen.next();
        if (!line_opt) break;
        if (!*line_opt) co_return line_opt->error();
        const auto& line = **line_opt;
        const dstd::StringView event = trace::event_of_line(
            dstd::StringView(line.content.data(), line.content.length()));
        if (!event.empty()) {
            hasher.reset();
            hasher.update(event);
            content_hash += hasher.hash().value;
        }
    }
    co_return content_hash;
}

struct SplitSettings {
    dstd::String app_name;
    std::size_t chunk_size_bytes = 0;
    bool compress = false;
    bool verify = false;
    std::size_t member_size_bytes = 0;
    std::size_t executor_threads = 0;
    std::uint64_t memory_budget = 0;
};

void print_banner(const SplitSettings& settings, const dstd::Path& log_dir,
                  const dstd::Path& output_dir) {
    const double chunk_size_mb =
        static_cast<double>(settings.chunk_size_bytes) / (1024.0 * 1024.0);
    std::printf("==========================================\n");
    std::printf("DFTracer Split\n");
    std::printf("==========================================\n");
    std::printf("Arguments:\n");
    std::printf("  App name: %s\n", settings.app_name.c_str());
    std::printf("  Compress: %s\n", settings.compress ? "true" : "false");
    std::printf("  Data dir: %s\n", log_dir.c_str());
    std::printf("  Output dir: %s\n", output_dir.c_str());
    std::printf("  Chunk size: %.2f MB (compressed)\n", chunk_size_mb);
    std::printf("  Gzip member size: %zu bytes\n", settings.member_size_bytes);
    std::printf("  Executor threads: %zu\n", settings.executor_threads);
    std::printf("  Memory budget: %" PRIu64 " bytes\n", settings.memory_budget);
    std::printf("==========================================\n\n");
}

dstd::Result<dstd::Vector<dstd::String>> collect_inputs(
    const SplitArgParse& cli, const dstd::Path& log_dir,
    const dstd::Path& output_dir) {
    dstd::Vector<dstd::String> input_files;
    auto matched =
        binaries::scan_trace_files_on_pool(cli.runtime_args, log_dir.str());
    if (!matched) return dstd::Unexpected<dstd::Error>(matched.error());
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
    for (const auto& entry : *matched) {
        if (inside(entry.path.view(), out_canonical.view()) ||
            inside(entry.path.view(), out_walked.view()))
            continue;
        input_files.emplace_back(entry.path.str());
    }
    return input_files;
}

// False when there is nothing to split. Sorts the inputs.
bool check_inputs(dstd::Vector<dstd::String>& input_files,
                  const dstd::Path& log_dir) {
    if (input_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No trace files found in {}", log_dir);
        return false;
    }
    std::sort(input_files.begin(), input_files.end());
    return true;
}

// The read paths decode a whole member at once, so a single huge gzip
// member is rechunked to bounded multi-member gzip first. The stage keeps
// pointers to `rechunk_dir` and `open_count`, which must outlive it.
class ProbeStage {
   public:
    using Input = dstd::Vector<dstd::String>;
    using Output = dstd::Result<FileProbes>;
    static constexpr const char* NAME = "ProbeFile";

    ProbeStage(const SplitSettings& settings, RechunkDir& rechunk_dir,
               std::atomic<std::size_t>& open_count)
        : member_size_bytes_(settings.member_size_bytes),
          executor_threads_(settings.executor_threads),
          open_budget_(binaries::open_file_budget()),
          rechunk_dir_(&rechunk_dir),
          open_count_(&open_count) {}

    coro::CoroTask<Output> run(Input input_files) const {
        auto probing = coro::parallel_map(
            std::move(input_files),
            [this](dstd::String path) { return probe_file(std::move(path)); },
            {.max_in_flight = executor_threads_});
        auto results = co_await std::move(probing);
        FileProbes probes;
        probes.reserve(results.size());
        for (auto& result : results) {
            if (!result) co_return result.error();
            probes.push_back(std::move(*result));
        }
        co_return probes;
    }

   private:
    coro::CoroTask<dstd::Result<FileProbe>> probe_file(
        dstd::String path) const {
        FileProbe probe;
        probe.path = std::move(path);
        auto opened = co_await runtime::io::open_file(probe.path, {});
        if (!opened) {
            DFTRACER_UTILS_LOG_ERROR("Cannot open {}", probe.path);
            co_return probe;
        }
        dstd::File sfd = std::move(*opened);
        const auto size = sfd.size();
        if (!size || *size == 0) {
            co_return probe;
        }
        probe.bytes = *size;
        const utilities::fileio::compress::FirstMember first =
            co_await utilities::fileio::compress::gzip_first_member(
                sfd.native_handle(), probe.bytes,
                utilities::fileio::compress::RECHUNK_MEMBER_CAP,
                RATIO_SAMPLE_BYTES);
        if (first.compressed > 0) {
            probe.ratio = std::clamp(static_cast<double>(first.uncompressed) /
                                         static_cast<double>(first.compressed),
                                     1.0, MAX_DEFLATE_RATIO);
        }
        if (!first.exceeds_cap &&
            open_count_->fetch_add(1, std::memory_order_relaxed) <
                open_budget_) {
            probe.fd = dstd::make_shared<dstd::File>(std::move(sfd));
        } else {
            sfd.close().discard();
        }
        if (first.exceeds_cap) {
            auto rechunk_path = rechunk_dir_->get();
            if (!rechunk_path) co_return rechunk_path.error();
            dstd::String out(
                (rechunk_path->view() / dstd::PathView(probe.path).filename())
                    .str());
            DFTRACER_UTILS_LOG_WARN(
                "Input {} is a single large gzip member; rechunking to bounded "
                "members at {}",
                probe.path, out);
            auto rechunking =
                utilities::fileio::compress::gzip_rechunk_to_members(
                    dstd::String(probe.path), dstd::String(out),
                    member_size_bytes_, 6);
            auto rechunked = co_await std::move(rechunking);
            if (!rechunked) co_return rechunked.error();
            probe.path = out;
            auto rechunked_size = dstd::fs::file_size(dstd::PathView(out));
            if (!rechunked_size) co_return rechunked_size.error();
            probe.bytes = *rechunked_size;
        }
        probe.ok = true;
        co_return probe;
    }

    std::size_t member_size_bytes_;
    std::size_t executor_threads_;
    std::size_t open_budget_;
    RechunkDir* rechunk_dir_;
    std::atomic<std::size_t>* open_count_;
};

class GroupStage {
   public:
    using Input = FileProbes;
    using Output = FileGroups;
    static constexpr const char* NAME = "GroupFiles";

    explicit GroupStage(const SplitSettings& settings)
        : chunk_size_bytes_(settings.chunk_size_bytes),
          executor_threads_(settings.executor_threads) {}

    coro::CoroTask<Output> run(Input probes) const {
        auto groups = group_files(
            dstd::Span<const FileProbe>(probes.data(), probes.size()),
            chunk_size_bytes_, executor_threads_);
        std::size_t file_count = 0;
        for (const auto& group : groups) file_count += group.files.size();
        DFTRACER_UTILS_LOG_INFO("Split {} files in {} groups", file_count,
                                groups.size());
        co_return groups;
    }

   private:
    std::size_t chunk_size_bytes_;
    std::size_t executor_threads_;
};

// `settings` and `output_dir` must outlive the stage.
class ExtractStage {
   public:
    using Input = FileGroups;
    using Output = ExtractChunksOutput;
    static constexpr const char* NAME = "ExtractChunks";

    ExtractStage(const SplitSettings& settings, const dstd::String& output_dir)
        : settings_(&settings), output_dir_(&output_dir) {}

    coro::CoroTask<Output> run(Input groups) const {
        const auto& settings = *settings_;
        DFTRACER_UTILS_LOG_INFO("Splitting {} groups in parallel...",
                                groups.size());

        // A running group holds its writer's buffers and its compress workers
        // use every core, so few groups run at once, within the memory budget.
        const std::size_t max_parallel =
            std::max<std::size_t>(2, settings.executor_threads / 4);
        const auto extraction = trace::plan_extraction_budget(
            settings.memory_budget, max_parallel, settings.executor_threads,
            settings.member_size_bytes);
        trace::ExtractionAdmission admission(
            settings.memory_budget, extraction.reserve_bytes, max_parallel);
        auto* admission_ptr = &admission;

        dstd::Vector<ExtractInput> inputs;
        inputs.reserve(groups.size());
        for (std::size_t i = 0; i < groups.size(); ++i) {
            // Chunk files are sized by compressed bytes and the writer counts
            // uncompressed ones.
            const double ratio = settings.compress ? groups[i].ratio : 1.0;
            inputs.push_back(
                ExtractInput::from_files(static_cast<int>(i + 1),
                                         std::move(groups[i].files))
                    .with_output_dir(*output_dir_)
                    .with_open_files(std::move(groups[i].open_files))
                    .with_app_name(settings.app_name)
                    .with_compression(settings.compress)
                    .with_compute_hash(settings.verify)
                    .with_member_size(settings.member_size_bytes)
                    .with_memory_budget(extraction.writer_bytes)
                    .with_chunk_size(chunk_uncompressed_bytes(
                        settings.chunk_size_bytes, ratio,
                        settings.member_size_bytes)));
        }

        auto extract_one =
            [admission_ptr](
                ExtractInput input) -> coro::CoroTask<ExtractResult> {
            co_await admission_ptr->enter();
            auto leave = dstd::defer([&] { admission_ptr->leave(); });
            trace::ChunkExtractorUtility extractor;
            co_return co_await extractor(input);
        };
        auto extracts =
            coro::parallel_map(std::move(inputs), std::move(extract_one),
                               {.max_in_flight = max_parallel});
        auto results = co_await std::move(extracts);

        std::sort(results.begin(), results.end(),
                  [](const ExtractResult& a, const ExtractResult& b) {
                      return a.chunk_index < b.chunk_index;
                  });
        co_return results;
    }

   private:
    const SplitSettings* settings_;
    const dstd::String* output_dir_;
};

struct VerifyInput {
    const ExtractChunksOutput* chunks;  ///< Must outlive the run
    dstd::Vector<dstd::String> files;   ///< The split input files
};

class VerifyStage {
   public:
    using Input = VerifyInput;
    using Output = dstd::Result<trace::ChunkVerificationUtilityOutput>;
    static constexpr const char* NAME = "VerifyChunks";

    explicit VerifyStage(const SplitSettings& settings)
        : executor_threads_(settings.executor_threads) {}

    coro::CoroTask<Output> run(Input input) const {
        std::size_t output_hash = 0;
        for (const auto& chunk : *input.chunks) {
            output_hash += chunk.event_hash;
        }

        auto hashing = coro::parallel_map(
            std::move(input.files),
            [](dstd::String path) -> coro::CoroTask<dstd::Result<std::size_t>> {
                auto hashed = hash_events(path);
                co_return co_await std::move(hashed);
            },
            {.max_in_flight = executor_threads_});
        auto file_hashes = co_await std::move(hashing);
        std::size_t input_hash = 0;
        for (const auto& file_hash : file_hashes) {
            if (!file_hash) co_return file_hash.error();
            input_hash += *file_hash;
        }

        co_return trace::ChunkVerificationUtilityOutput::success(
            static_cast<std::uint64_t>(input_hash),
            static_cast<std::uint64_t>(output_hash));
    }

   private:
    std::size_t executor_threads_;
};

static_assert(coro::BatchStage<ProbeStage>);
static_assert(coro::BatchStage<GroupStage>);
static_assert(coro::BatchStage<ExtractStage>);
static_assert(coro::BatchStage<VerifyStage>);

int print_report(const ExtractChunksOutput& split_results,
                 std::size_t input_count, double duration_ms,
                 const dstd::Optional<trace::ChunkVerificationUtilityOutput>&
                     verify_result) {
    std::printf("\n");
    std::printf("==========================================\n");
    std::printf("Split Results\n");
    std::printf("==========================================\n");
    std::printf("  Execution time: %.2f seconds\n", duration_ms / 1000.0);
    std::printf("  Input: %zu files\n", input_count);

    int exit_code = 0;

    std::size_t chunk_files = 0;
    std::size_t total_events = 0;
    for (const auto& result : split_results) {
        if (result.success) {
            chunk_files += result.chunks.size();
            total_events += result.events;
        } else {
            DFTRACER_UTILS_LOG_ERROR("Failed to split group {}",
                                     result.chunk_index);
            exit_code = 1;
        }
    }
    std::printf("  Output: %zu chunk files, %zu events\n", chunk_files,
                total_events);
    if (chunk_files == 0) {
        DFTRACER_UTILS_LOG_ERROR("{}", "No chunk files were written");
        exit_code = 1;
    }

    if (verify_result) {
        if (verify_result->input_hash == verify_result->output_hash) {
            std::printf(
                "  Verification: PASSED - all events present in output\n");
        } else {
            std::printf("  Verification: FAILED - event mismatch detected\n");
            exit_code = 1;
        }
        std::printf("    Input hash:  0x%016" PRIx64 "\n",
                    verify_result->input_hash);
        std::printf("    Output hash: 0x%016" PRIx64 "\n",
                    verify_result->output_hash);
    }

    std::printf("==========================================\n");
    return exit_code;
}

}  // namespace

static coro::CoroTask<int> run_split(const SplitArgParse* cli) {
    auto absolute_log_dir = binaries::absolute_path(cli->inputs.directory);
    if (!absolute_log_dir)
        co_return binaries::report_error(absolute_log_dir.error());
    const auto log_dir = std::move(*absolute_log_dir);
    auto absolute_output_dir = binaries::absolute_path(cli->output_dir);
    if (!absolute_output_dir)
        co_return binaries::report_error(absolute_output_dir.error());
    const auto output_dir = std::move(*absolute_output_dir);
    SplitSettings settings;
    settings.app_name = dstd::String(cli->app_name);
    settings.chunk_size_bytes = cli->chunk_size_bytes;
    settings.compress = cli->compress;
    settings.verify = cli->verify;
    settings.member_size_bytes = cli->member.checkpoint_size;
    settings.executor_threads = cli->runtime_args.executor_threads;
    settings.memory_budget =
        memory::resolve_spill_budget(cli->memory.memory_budget);

    print_banner(settings, log_dir, output_dir);

    auto output_exists = dstd::fs::exists(dstd::PathView(output_dir));
    if (!output_exists.has_value())
        co_return binaries::report_error(output_exists.error());
    if (!*output_exists) {
        auto created = dstd::fs::create_directories(dstd::PathView(output_dir));
        if (!created.has_value())
            co_return binaries::report_error(created.error());
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    DFTRACER_UTILS_LOG_INFO("{}", "Discovering input files...");

    auto collected = collect_inputs(*cli, log_dir, output_dir);
    if (!collected) co_return binaries::report_error(collected.error());
    dstd::Vector<dstd::String> input_files = std::move(*collected);
    if (!check_inputs(input_files, log_dir)) co_return 1;

    DFTRACER_UTILS_LOG_INFO("Found {} input files", input_files.size());

    const std::size_t input_count = input_files.size();
    RechunkDir rechunk_dir;
    std::atomic<std::size_t> open_count{0};
    const dstd::String output_dir_text(output_dir.str());
    ExtractChunksOutput split_results;
    dstd::Optional<trace::ChunkVerificationUtilityOutput> verification;

    const ProbeStage probe_stage(settings, rechunk_dir, open_count);
    const GroupStage group_stage(settings);
    const ExtractStage extract_stage(settings, output_dir_text);
    const VerifyStage verify_stage(settings);

    DFTRACER_UTILS_LOG_INFO("{}", "Running...");

    auto ran = binaries::run_on_pool(
        "DFTracer Split", cli->runtime_args, cli->watchdog,
        [&](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
            auto probing = probe_stage.run(std::move(input_files));
            auto probes = co_await coro::or_fail(std::move(probing));
            auto groups = co_await group_stage.run(std::move(probes));
            dstd::Vector<dstd::String> split_files;
            for (const auto& group : groups) {
                split_files.insert(split_files.end(), group.files.begin(),
                                   group.files.end());
            }
            split_results = co_await extract_stage.run(std::move(groups));
            if (settings.verify) {
                VerifyInput verify_input{&split_results,
                                         std::move(split_files)};
                auto verifying = verify_stage.run(std::move(verify_input));
                verification = co_await coro::or_fail(std::move(verifying));
            }
            co_return dstd::Result<void>();
        });
    if (!ran) co_return binaries::report_error(ran.error());

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    const int exit_code = print_report(split_results, input_count,
                                       duration.count(), verification);

    if (!rechunk_dir.path.empty()) {
        auto rechunk_exists = dstd::fs::exists(rechunk_dir.path.view());
        if (!rechunk_exists.has_value())
            co_return binaries::report_error(rechunk_exists.error());
        if (*rechunk_exists) {
            DFTRACER_UTILS_LOG_INFO(
                "Cleaning up temporary rechunk directory: {}",
                rechunk_dir.path);
            auto removed = dstd::fs::remove_all(rechunk_dir.path.view());
            if (!removed.has_value())
                co_return binaries::report_error(removed.error());
        }
    }

    co_return exit_code;
}

int main(int argc, char** argv) {
    return binaries::cli_main<SplitArgParse>(
        argc, argv, "dftracer_split",
        "Split DFTracer traces into equal-sized chunks in parallel",
        [](SplitArgParse& cli) { return run_split(&cli).get(); });
}
