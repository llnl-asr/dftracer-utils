#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/async_mutex.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reconstruction_planner.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/fileio/chunk_writer.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::reorganize;
using dftracer::utils::utilities::fileio::ChunkWriter;
using dftracer::utils::utilities::fileio::ChunkWriterConfig;

namespace {

struct SegmentInterval {
    int line_start;
    int line_end;
    std::string original_path;
    int source_checkpoint;
};

const SegmentInterval* find_segment(
    const std::vector<SegmentInterval>& intervals, int line_number) {
    auto it = std::upper_bound(
        intervals.begin(), intervals.end(), line_number,
        [](int ln, const SegmentInterval& seg) { return ln < seg.line_start; });
    if (it != intervals.begin()) {
        --it;
        if (line_number >= it->line_start && line_number < it->line_end) {
            return &(*it);
        }
    }
    return nullptr;
}

std::string output_filename(const std::string& original_path) {
    auto p = fs::path(original_path).filename().string();
    if (p.size() > 3 && p.substr(p.size() - 3) == ".gz") {
        p = p.substr(0, p.size() - 3);
    }
    return p;
}

}  // namespace

static coro::CoroTask<int> run_reconstruct(const std::string& directory,
                                           const std::string& output_dir,
                                           std::size_t checkpoint_size,
                                           bool no_compress,
                                           std::size_t executor_threads) {
    std::printf("==========================================\n");
    std::printf("DFTracer Trace Reconstructor\n");
    std::printf("==========================================\n");

    std::vector<std::string> reorg_files;
    if (fs::exists(directory)) {
        filesystem::PatternDirectoryScannerUtility scanner;
        filesystem::PatternDirectoryScannerUtilityInput scan_input{
            directory, {".pfw", ".pfw.gz"}, true};
        auto matched = co_await scanner.process(scan_input);
        for (const auto& entry : matched) {
            reorg_files.push_back(entry.path.string());
        }
    }

    if (reorg_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("%s", "No reorganized files found.");
        co_return 1;
    }

    std::printf("  Input directory: %s\n", directory.c_str());
    std::printf("  Reorganized files: %zu\n", reorg_files.size());
    std::printf("  Output directory: %s\n", output_dir.c_str());

    auto start_time = std::chrono::high_resolution_clock::now();

    std::printf("\nStep 1: Building reconstruction plan...\n");
    ReconstructionPlannerUtility planner;
    ReconstructionPlannerInput planner_input;
    planner_input.reorganized_files = reorg_files;
    planner_input.index_dir = "";

    ReconstructionPlan plan;
    try {
        plan = co_await planner.process(planner_input);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Planning failed: %s", e.what());
        co_return 1;
    }

    if (plan.files.empty()) {
        std::printf("No files with provenance found.\n");
        co_return 0;
    }

    std::printf("  Original files to reconstruct: %zu\n", plan.files.size());
    std::printf("  Total segments: %zu\n", plan.total_segments);
    std::printf("  Total events: %zu\n", plan.total_events);

    // Step 2: For each reorganized file, extract lines and write directly
    // to output via ChunkWriter (streaming, no full buffering)
    std::printf("\nStep 2: Extracting and writing...\n");

    // Build per-reorg-file segment intervals
    std::unordered_map<std::string, std::vector<SegmentInterval>>
        per_reorg_segments;
    for (const auto& [orig_path, recon] : plan.files) {
        for (const auto& [ckpt, segs] : recon.checkpoint_segments) {
            for (const auto& seg : segs) {
                SegmentInterval si;
                si.line_start = seg.output_line_start;
                si.line_end = seg.output_line_end;
                si.original_path = orig_path;
                si.source_checkpoint = seg.source_checkpoint;
                per_reorg_segments[seg.reorg_file].push_back(std::move(si));
            }
        }
    }

    for (auto& [file, segs] : per_reorg_segments) {
        std::sort(segs.begin(), segs.end(),
                  [](const SegmentInterval& a, const SegmentInterval& b) {
                      return a.line_start < b.line_start;
                  });
    }

    std::unordered_map<std::string, std::unique_ptr<ChunkWriter>> writers;
    std::unordered_map<std::string, std::unique_ptr<coro::AsyncMutex>>
        writer_mutexes;
    for (const auto& [orig_path, recon] : plan.files) {
        std::string fname = output_filename(orig_path);
        std::string base = fname;
        if (base.size() > 4 && base.substr(base.size() - 4) == ".pfw") {
            base = base.substr(0, base.size() - 4);
        }

        auto config =
            ChunkWriterConfig()
                .with_output_dir(output_dir)
                .with_base_name(base)
                .with_chunk_size(std::numeric_limits<std::size_t>::max())
                .with_compression(!no_compress);
        writers[orig_path] = std::make_unique<ChunkWriter>(config);
        writer_mutexes[orig_path] = std::make_unique<coro::AsyncMutex>();
    }

    for (auto& [path, writer] : writers) {
        co_await writer->open();
    }

    // Process reorganized files (parallel via pipeline)
    {
        auto pipeline_config = PipelineConfig()
                                   .with_name("Reconstruct: Extract")
                                   .with_compute_threads(executor_threads)
                                   .with_watchdog(false);
        Pipeline pipeline(pipeline_config);

        auto* per_reorg_ptr = &per_reorg_segments;
        auto* writers_ptr = &writers;
        auto* mutexes_ptr = &writer_mutexes;

        auto extract_task = make_task(
            [per_reorg_ptr, writers_ptr, mutexes_ptr, checkpoint_size,
             executor_threads](CoroScope& scope) -> coro::CoroTask<void> {
                auto permits = coro::make_channel<bool>(executor_threads * 2);
                for (std::size_t i = 0; i < executor_threads * 2; ++i) {
                    permits->try_send(true);
                }

                std::vector<coro::SpawnFuture<void>> futures;

                for (const auto& [reorg_file, intervals] : *per_reorg_ptr) {
                    auto* intervals_ptr = &intervals;
                    auto reorg_file_copy = reorg_file;
                    futures.push_back(scope.spawn([reorg_file_copy,
                                                   intervals_ptr, writers_ptr,
                                                   mutexes_ptr, checkpoint_size,
                                                   permits](CoroScope& s)
                                                      -> coro::CoroTask<void> {
                        co_await s.receive(permits);
                        try {
                            std::string idx_path =
                                internal::determine_index_path(reorg_file_copy,
                                                               "");

                            MetadataCollectorUtility meta_collector;
                            auto meta_input =
                                MetadataCollectorUtilityInput::from_file(
                                    reorg_file_copy)
                                    .with_index(idx_path)
                                    .with_checkpoint_size(checkpoint_size);
                            auto meta =
                                co_await meta_collector.process(meta_input);

                            auto reader_input =
                                IndexedReadInput::from_file(reorg_file_copy)
                                    .with_index(idx_path)
                                    .with_checkpoint_size(checkpoint_size);
                            IndexedFileReaderUtility reader_utility;
                            auto reader =
                                co_await reader_utility.process(reader_input);

                            auto stream = reader->stream(
                                reader::internal::StreamConfig()
                                    .stream_type(reader::internal::StreamType::
                                                     MULTI_LINES_BYTES)
                                    .range_type(
                                        reader::internal::RangeType::BYTE_RANGE)
                                    .buffer_size(4 * 1024 * 1024)
                                    .from(0)
                                    .to(meta.uncompressed_size));

                            struct PendingLine {
                                const char* data;
                                std::size_t len;
                            };
                            std::unordered_map<std::string,
                                               std::vector<PendingLine>>
                                batch;
                            int event_number = 0;

                            while (!stream->done()) {
                                auto chunk = co_await stream->read_async();
                                if (chunk.empty()) break;

                                const char* data = chunk.data();
                                std::size_t bytes_read = chunk.size();
                                std::size_t pos = 0;

                                while (pos < bytes_read) {
                                    const char* line_start = data + pos;
                                    const char* newline =
                                        static_cast<const char*>(
                                            std::memchr(line_start, '\n',
                                                        bytes_read - pos));
                                    if (!newline) break;
                                    std::size_t line_len =
                                        static_cast<std::size_t>(newline -
                                                                 line_start);

                                    if (line_len > 0 && line_start[0] == '{') {
                                        const auto* seg = find_segment(
                                            *intervals_ptr, event_number);
                                        if (seg) {
                                            batch[seg->original_path].push_back(
                                                {line_start, line_len});
                                        }
                                        event_number++;
                                    }

                                    pos = static_cast<std::size_t>(newline -
                                                                   data) +
                                          1;
                                }

                                for (auto& [orig, lines] : batch) {
                                    if (lines.empty()) continue;
                                    auto wit = writers_ptr->find(orig);
                                    auto mit = mutexes_ptr->find(orig);
                                    if (wit != writers_ptr->end() &&
                                        mit != mutexes_ptr->end()) {
                                        co_await mit->second->lock();
                                        for (const auto& l : lines) {
                                            co_await wit->second->write_line(
                                                ByteView(l.data, l.len));
                                        }
                                        mit->second->unlock();
                                    }
                                    lines.clear();
                                }
                            }

                            permits->try_send(true);
                        } catch (...) {
                            permits->try_send(true);
                            throw;
                        }
                    }));
                }

                for (auto& f : futures) {
                    co_await f;
                }
            },
            "ExtractLines");

        pipeline.set_source(extract_task);
        pipeline.set_destination(extract_task);
        pipeline.execute();
    }

    // Close all writers
    std::size_t files_written = 0;
    for (auto& [path, writer] : writers) {
        co_await writer->close();
        std::string fname = output_filename(path);
        std::printf("  %s: %zu events\n", fname.c_str(),
                    writer->total_events_written());
        files_written++;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::printf("\n==========================================\n");
    std::printf("Reconstruction Complete\n");
    std::printf("==========================================\n");
    std::printf("  Time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Files reconstructed: %zu\n", files_written);
    std::printf("==========================================\n");

    co_return 0;
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_reconstruct",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Reconstruct original trace files from reorganized output.");

    program.add_argument("-d", "--directory")
        .help("Directory containing reorganized files")
        .required();

    program.add_argument("-o", "--output").help("Output directory").required();

    program.add_argument("--checkpoint-size")
        .help("Checkpoint size for indexing in bytes")
        .scan<'d', std::size_t>()
        .default_value(static_cast<std::size_t>(
            indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE));

    program.add_argument("--no-compress")
        .help("Write plain .pfw instead of .pfw.gz")
        .flag();

    program.add_argument("--executor-threads")
        .help("Worker threads")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(dftracer_utils_hardware_concurrency()));

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
        std::cerr << program;
        return 1;
    }

    std::string directory = program.get<std::string>("--directory");
    std::string output_dir = program.get<std::string>("--output");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    bool no_compress = program.get<bool>("--no-compress");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");

    fs::create_directories(output_dir);

    return run_reconstruct(directory, output_dir, checkpoint_size, no_compress,
                           executor_threads)
        .get();
}
