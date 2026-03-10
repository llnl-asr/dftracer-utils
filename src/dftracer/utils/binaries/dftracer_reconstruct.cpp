#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reconstruction_planner.h>
#include <dftracer/utils/utilities/composites/file_compressor_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::composites::dft;
using namespace dftracer::utils::utilities::composites::dft::reorganize;

namespace {

// A segment interval for binary search during
// streaming
struct SegmentInterval {
    int line_start;
    int line_end;
    std::string original_path;
    int source_checkpoint;
};

// Find which segment a global line number belongs to
const SegmentInterval* find_segment(
    const std::vector<SegmentInterval>& intervals, int line_number) {
    // Binary search: find last interval where
    // line_start <= line_number
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

// Extract a clean output filename from an original
// path.
// "/path/to/trace.pfw.gz" -> "trace.pfw"
// "/path/to/trace.pfw" -> "trace.pfw"
std::string output_filename(const std::string& original_path) {
    auto p = fs::path(original_path).filename().string();
    // Strip .gz suffix if present
    if (p.size() > 3 && p.substr(p.size() - 3) == ".gz") {
        p = p.substr(0, p.size() - 3);
    }
    return p;
}

}  // namespace

static coro::CoroTask<int> run_reconstruct(const std::string& directory,
                                           const std::string& output_dir,
                                           const std::string& index_dir,
                                           std::size_t checkpoint_size,
                                           bool no_compress) {
    // Step 1: Scan for reorganized files
    std::printf(
        "========================================"
        "==\n");
    std::printf("DFTracer Trace Reconstructor\n");
    std::printf(
        "========================================"
        "==\n");

    std::vector<std::string> reorg_files;
    if (fs::exists(directory)) {
        filesystem::PatternDirectoryScannerUtility scanner;
        filesystem::PatternDirectoryScannerUtilityInput scan_input{
            directory, {".pfw", ".pfw.gz"}, false};
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
    std::printf(
        "========================================"
        "==\n\n");

    auto start_time = std::chrono::high_resolution_clock::now();

    // Step 2: Build reconstruction plan
    std::printf(
        "Step 1: Building reconstruction "
        "plan...\n");
    ReconstructionPlannerUtility planner;
    ReconstructionPlannerInput planner_input;
    planner_input.reorganized_files = reorg_files;
    planner_input.index_dir = index_dir;

    ReconstructionPlan plan;
    try {
        plan = co_await planner.process(planner_input);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Planning failed: %s", e.what());
        co_return 1;
    }

    if (plan.files.empty()) {
        std::printf(
            "No files with provenance found. "
            "Nothing to reconstruct.\n");
        co_return 0;
    }

    std::printf(
        "  Original files to reconstruct: "
        "%zu\n",
        plan.files.size());
    std::printf("  Total segments: %zu\n", plan.total_segments);
    std::printf("  Total events: %zu\n", plan.total_events);

    for (const auto& [path, recon] : plan.files) {
        std::printf("    %s (%d checkpoints)\n", output_filename(path).c_str(),
                    recon.num_checkpoints);
    }

    // Step 3: Extract lines from reorganized files
    std::printf(
        "\nStep 2: Extracting lines from "
        "reorganized files...\n");

    // Buffers: original_path -> checkpoint -> lines
    std::map<std::string, std::map<int, std::vector<std::string>>> buffers;

    std::size_t total_lines_extracted = 0;
    std::size_t files_processed = 0;

    // Collect all segments per reorganized file for
    // efficient single-pass extraction
    std::map<std::string, std::vector<SegmentInterval>> per_reorg_segments;

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

    // Sort each file's segments by line_start
    for (auto& [file, segs] : per_reorg_segments) {
        std::sort(segs.begin(), segs.end(),
                  [](const SegmentInterval& a, const SegmentInterval& b) {
                      return a.line_start < b.line_start;
                  });
    }

    // Stream each reorganized file once
    for (const auto& [reorg_file, intervals] : per_reorg_segments) {
        std::printf("  Processing: %s\n",
                    fs::path(reorg_file).filename().c_str());

        // Get file metadata for byte range
        std::string idx_path =
            internal::determine_index_path(reorg_file, index_dir);

        MetadataCollectorUtility meta_collector;
        auto meta_input = MetadataCollectorUtilityInput::from_file(reorg_file)
                              .with_index(idx_path)
                              .with_checkpoint_size(checkpoint_size);
        auto meta = co_await meta_collector.process(meta_input);
        if (!meta.success) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to get metadata for %s: "
                "%s",
                reorg_file.c_str(), meta.error_message.c_str());
            continue;
        }

        // Create reader
        auto reader_input = IndexedReadInput::from_file(reorg_file)
                                .with_index(idx_path)
                                .with_checkpoint_size(checkpoint_size);
        IndexedFileReaderUtility reader_utility;
        std::shared_ptr<reader::internal::Reader> reader;
        try {
            reader = co_await reader_utility.process(reader_input);
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to create reader for %s: "
                "%s",
                reorg_file.c_str(), e.what());
            continue;
        }

        // Stream full file
        auto stream = reader->stream(
            reader::internal::StreamConfig()
                .stream_type(reader::internal::StreamType::MULTI_LINES_BYTES)
                .range_type(reader::internal::RangeType::BYTE_RANGE)
                .buffer_size(4 * 1024 * 1024)
                .from(0)
                .to(meta.uncompressed_size));

        int line_number = 0;
        while (!stream->done()) {
            auto chunk = co_await stream->read_async();
            if (chunk.empty()) break;

            const char* data = chunk.data();
            std::size_t bytes_read = chunk.size();
            std::size_t pos = 0;

            while (pos < bytes_read) {
                const char* line_start = data + pos;
                const char* newline = static_cast<const char*>(
                    std::memchr(line_start, '\n', bytes_read - pos));
                if (!newline) break;
                std::size_t line_len =
                    static_cast<std::size_t>(newline - line_start);

                const auto* seg = find_segment(intervals, line_number);
                if (seg) {
                    buffers[seg->original_path][seg->source_checkpoint]
                        .emplace_back(line_start, line_len);
                    total_lines_extracted++;
                }

                pos = static_cast<std::size_t>(newline - data) + 1;
                line_number++;
            }
        }

        files_processed++;
        std::printf("    Lines: %d total\n", line_number);
    }

    std::printf("  Files processed: %zu\n", files_processed);
    std::printf("  Lines extracted: %zu\n", total_lines_extracted);

    // Step 4: Write reconstructed files
    std::printf(
        "\nStep 3: Writing reconstructed "
        "files...\n");

    std::size_t files_written = 0;
    for (const auto& [orig_path, recon] : plan.files) {
        std::string fname = output_filename(orig_path);
        std::string out_pfw = output_dir + "/" + fname;

        ssize_t open_result = co_await io::open(
            out_pfw.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (open_result < 0) {
            DFTRACER_UTILS_LOG_ERROR("Failed to open output: %s",
                                     out_pfw.c_str());
            continue;
        }
        int fd = static_cast<int>(open_result);

        // 256KB write buffer
        constexpr std::size_t WRITE_BUFFER_SIZE = 256 * 1024;
        std::vector<char> write_buf;

        std::size_t lines_written = 0;
        auto buf_it = buffers.find(orig_path);
        if (buf_it != buffers.end()) {
            // Write in checkpoint order
            for (const auto& [ckpt, lines] : buf_it->second) {
                for (const auto& line : lines) {
                    write_buf.insert(write_buf.end(), line.data(),
                                     line.data() + line.size());
                    write_buf.push_back('\n');
                    if (write_buf.size() >= WRITE_BUFFER_SIZE) {
                        co_await io::write(fd, write_buf.data(),
                                           write_buf.size());
                        write_buf.clear();
                    }
                    lines_written++;
                }
            }
        }

        // Flush remaining write buffer
        if (!write_buf.empty()) {
            co_await io::write(fd, write_buf.data(), write_buf.size());
            write_buf.clear();
        }

        co_await io::close(fd);
        std::printf("  %s: %zu lines\n", fname.c_str(), lines_written);
        files_written++;
    }

    // Step 5: Compress
    if (!no_compress) {
        std::printf(
            "\nStep 4: Compressing output "
            "files...\n");
        for (const auto& [orig_path, recon] : plan.files) {
            std::string fname = output_filename(orig_path);
            std::string out_pfw = output_dir + "/" + fname;

            if (!fs::exists(out_pfw) || fs::file_size(out_pfw) == 0) {
                continue;
            }

            FileCompressorUtility compressor;
            auto comp_result = co_await compressor.process(
                FileCompressionUtilityInput::from_file(out_pfw));

            if (comp_result.success) {
                fs::remove(out_pfw);
                std::printf(
                    "  %s: %.2f MB -> %.2f MB "
                    "(%.1f%% reduction)\n",
                    fname.c_str(),
                    static_cast<double>(comp_result.original_size) /
                        (1024.0 * 1024.0),
                    static_cast<double>(comp_result.compressed_size) /
                        (1024.0 * 1024.0),
                    comp_result.compression_percentage());
            } else {
                DFTRACER_UTILS_LOG_ERROR(
                    "Compression failed for %s: "
                    "%s",
                    out_pfw.c_str(), comp_result.error_message.c_str());
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    std::printf(
        "\n========================================"
        "==\n");
    std::printf("Reconstruction Complete\n");
    std::printf(
        "========================================"
        "==\n");
    std::printf("  Time: %.2f seconds\n", duration.count() / 1000.0);
    std::printf("  Reorganized files read: %zu\n", files_processed);
    std::printf(
        "  Original files reconstructed: "
        "%zu\n",
        files_written);
    std::printf("  Total lines extracted: %zu\n", total_lines_extracted);

    // List output files
    std::printf("  Output files:\n");
    for (const auto& [orig_path, recon] : plan.files) {
        std::string fname = output_filename(orig_path);
        std::string out_pfw = output_dir + "/" + fname;
        std::string out_gz = out_pfw + ".gz";
        if (!no_compress && fs::exists(out_gz)) {
            std::printf(
                "    %s (%.2f MB)\n", out_gz.c_str(),
                static_cast<double>(fs::file_size(out_gz)) / (1024.0 * 1024.0));
        } else if (fs::exists(out_pfw)) {
            std::printf("    %s (%.2f MB)\n", out_pfw.c_str(),
                        static_cast<double>(fs::file_size(out_pfw)) /
                            (1024.0 * 1024.0));
        }
    }
    std::printf(
        "========================================"
        "==\n");

    co_return 0;
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_reconstruct",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Reconstruct original trace files from "
        "reorganized files using provenance "
        "tracking in .midx sidecars.");

    program.add_argument("-d", "--directory")
        .help(
            "Directory containing reorganized "
            "files")
        .required();

    program.add_argument("-o", "--output")
        .help("Output directory (required)")
        .required();

    program.add_argument("--index-dir")
        .help("Directory for sidecar files")
        .default_value<std::string>("");

    program.add_argument("--checkpoint-size")
        .help("Checkpoint size for indexing")
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
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
        std::cerr << program;
        return 1;
    }

    std::string directory = program.get<std::string>("--directory");
    std::string output_dir = program.get<std::string>("--output");
    std::string index_dir = program.get<std::string>("--index-dir");
    std::size_t checkpoint_size = program.get<std::size_t>("--checkpoint-size");
    bool no_compress = program.get<bool>("--no-compress");

    if (index_dir.empty()) {
        index_dir = directory;
    }

    fs::create_directories(output_dir);
    return run_reconstruct(directory, output_dir, index_dir, checkpoint_size,
                           no_compress)
        .get();
}
