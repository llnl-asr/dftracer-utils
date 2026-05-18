#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/spawn_future.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reconstruction_planner.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reconstructor_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/fileio/chunk_writer.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_visitor.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <span>
#include <unordered_map>

namespace dftracer::utils::utilities::composites::dft::reorganize {

using fileio::ChunkWriter;
using fileio::ChunkWriterConfig;
using indexer::SharedLineBuffer;

ReconstructorInput& ReconstructorInput::with_input_dir(std::string dir) {
    input_dir = std::move(dir);
    return *this;
}

ReconstructorInput& ReconstructorInput::with_output_dir(std::string dir) {
    output_dir = std::move(dir);
    return *this;
}

ReconstructorInput& ReconstructorInput::with_checkpoint_size(std::size_t sz) {
    checkpoint_size = sz;
    return *this;
}

ReconstructorInput& ReconstructorInput::with_parallelism(std::size_t n) {
    parallelism = n;
    return *this;
}

ReconstructorInput& ReconstructorInput::with_compress(bool c) {
    compress = c;
    return *this;
}

namespace {

struct SegmentInterval {
    int line_start;
    int line_end;
    std::size_t original_idx;  // Index into original files vector
};

const SegmentInterval* find_segment(
    const std::vector<SegmentInterval>& intervals, int line_number) {
    auto it = std::upper_bound(
        intervals.begin(), intervals.end(), line_number,
        [](int ln, const SegmentInterval& seg) { return ln < seg.line_start; });
    if (it != intervals.begin()) {
        --it;
        if (line_number >= it->line_start && line_number <= it->line_end) {
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

struct ReconstructLineRecord {
    SharedLineBuffer buffer;
    std::string_view line;
};

struct ReconstructLineBatch {
    std::vector<ReconstructLineRecord> lines;

    void reserve(std::size_t n) { lines.reserve(n); }
    std::size_t size() const { return lines.size(); }
    bool empty() const { return lines.empty(); }
    void clear() { lines.clear(); }
};

struct WriterContext {
    std::string output_dir;
    bool compress;
    std::atomic<std::size_t>* total_events;
    std::atomic<std::size_t>* total_bytes;
    std::vector<ReconstructedFileInfo>* file_results;
    std::mutex* results_mutex;
};

struct ReaderContext {
    std::size_t checkpoint_size;
    const std::vector<std::string>* original_paths;
    const std::vector<std::shared_ptr<coro::Channel<ReconstructLineBatch>>>*
        channels;
};

// Named coroutine for writer task (CP.51 - no capturing lambda coroutines)
static coro::CoroTask<void> run_writer(
    std::shared_ptr<coro::Channel<ReconstructLineBatch>> channel,
    std::string orig_path, WriterContext wctx) {
    std::string fname = output_filename(orig_path);
    std::string base = fname;
    if (base.size() > 4 && base.substr(base.size() - 4) == ".pfw") {
        base = base.substr(0, base.size() - 4);
    }

    auto config = ChunkWriterConfig()
                      .with_output_dir(wctx.output_dir)
                      .with_base_name(base)
                      .with_chunk_size(std::numeric_limits<std::size_t>::max())
                      .with_compression(wctx.compress);

    ChunkWriter writer(config);
    co_await writer.open();

    while (auto batch_opt = co_await channel->receive()) {
        auto& batch = *batch_opt;
        for (const auto& record : batch.lines) {
            co_await writer.write_line(
                ByteView(record.line.data(), record.line.size()));
        }
    }

    co_await writer.close();

    ReconstructedFileInfo info;
    info.original_path = std::move(orig_path);
    info.events_written = writer.total_events_written();
    info.bytes_written = writer.total_bytes_written();
    if (!writer.chunks().empty()) {
        info.output_path = writer.chunks().front().path;
    }

    wctx.total_events->fetch_add(info.events_written);
    wctx.total_bytes->fetch_add(info.bytes_written);

    {
        std::lock_guard<std::mutex> lock(*wctx.results_mutex);
        wctx.file_results->push_back(std::move(info));
    }
}

// Named coroutine for reader/producer task (CP.51)
static coro::CoroTask<void> run_reader(
    CoroScope& scope, std::string reorg_file,
    const std::vector<SegmentInterval>* intervals, ReaderContext rctx) {
    std::string index_path = internal::determine_index_path(reorg_file, "");

    MetadataCollectorUtility meta_collector;
    auto meta_input = MetadataCollectorUtilityInput::from_file(reorg_file)
                          .with_index(index_path)
                          .with_checkpoint_size(rctx.checkpoint_size);
    auto meta = co_await meta_collector.process(meta_input);

    auto reader_input = IndexedReadInput::from_file(reorg_file)
                            .with_index(index_path)
                            .with_checkpoint_size(rctx.checkpoint_size);
    IndexedFileReaderUtility reader_utility;
    auto reader = co_await reader_utility.process(reader_input);

    auto stream = reader->stream(
        reader::internal::StreamConfig()
            .stream_type(reader::internal::StreamType::MULTI_LINES_BYTES)
            .range_type(reader::internal::RangeType::BYTE_RANGE)
            .buffer_size(4 * 1024 * 1024)
            .from(0)
            .to(meta.uncompressed_size));

    constexpr std::size_t BATCH_SIZE = 1024;
    std::unordered_map<std::size_t, ReconstructLineBatch> pending_batches;

    int event_number = 0;

    while (!stream->done()) {
        std::span<const char> chunk = co_await stream->read_async();
        if (chunk.empty()) break;

        auto buffer = std::make_shared<std::string>(
            chunk.data(), static_cast<std::string::size_type>(chunk.size()));

        const char* data = buffer->data();
        std::size_t bytes_read = buffer->size();
        std::size_t pos = 0;

        while (pos < bytes_read) {
            const char* line_start = data + pos;
            const char* newline = static_cast<const char*>(
                std::memchr(line_start, '\n', bytes_read - pos));
            if (!newline) break;

            std::size_t line_len =
                static_cast<std::size_t>(newline - line_start);

            if (line_len > 0 && line_start[0] == '{') {
                const auto* seg = find_segment(*intervals, event_number);
                if (seg) {
                    auto& batch = pending_batches[seg->original_idx];
                    ReconstructLineRecord record;
                    record.buffer = buffer;
                    record.line = std::string_view(line_start, line_len);
                    batch.lines.push_back(std::move(record));

                    if (batch.size() >= BATCH_SIZE) {
                        auto& channel = (*rctx.channels)[seg->original_idx];
                        co_await channel->send(std::move(batch));
                        batch.clear();
                    }
                }
                event_number++;
            }

            pos = static_cast<std::size_t>(newline - data) + 1;
        }
    }

    for (auto& [idx, batch] : pending_batches) {
        if (!batch.empty()) {
            auto& channel = (*rctx.channels)[idx];
            co_await channel->send(std::move(batch));
        }
    }

    (void)scope;
}

}  // namespace

coro::CoroTask<ReconstructorResult> ReconstructorUtility::process(
    const ReconstructorInput& input) {
    ReconstructorResult result;

    if (!has_context()) {
        result.error_message = "No context bound";
        co_return result;
    }
    CoroScope& ctx = context();

    std::vector<std::string> reorg_files;
    if (fs::exists(input.input_dir)) {
        filesystem::PatternDirectoryScannerUtility scanner;
        filesystem::PatternDirectoryScannerUtilityInput scan_input{
            input.input_dir, {".pfw", ".pfw.gz"}, true};
        auto matched = co_await scanner.process(scan_input);
        for (const auto& entry : matched) {
            reorg_files.push_back(entry.path.string());
        }
    }

    if (reorg_files.empty()) {
        result.error_message = "No reorganized files found";
        co_return result;
    }

    ReconstructionPlannerUtility planner;
    ReconstructionPlannerInput planner_input;
    planner_input.reorganized_files = reorg_files;
    planner_input.index_dir = "";

    ReconstructionPlan plan;
    try {
        plan = co_await planner.process(planner_input);
    } catch (const std::exception& e) {
        result.error_message = std::string("Planning failed: ") + e.what();
        co_return result;
    }

    if (plan.files.empty()) {
        result.success = true;
        co_return result;
    }

    result.total_segments = plan.total_segments;

    fs::create_directories(input.output_dir);

    // Build original paths vector and index map
    std::vector<std::string> original_paths;
    std::unordered_map<std::string, std::size_t> path_to_idx;
    for (const auto& [orig_path, recon] : plan.files) {
        path_to_idx[orig_path] = original_paths.size();
        original_paths.push_back(orig_path);
    }

    // Build segment intervals using indices instead of strings
    std::unordered_map<std::string, std::vector<SegmentInterval>>
        per_reorg_segments;
    for (const auto& [orig_path, recon] : plan.files) {
        std::size_t orig_idx = path_to_idx[orig_path];
        for (const auto& [ckpt, segs] : recon.checkpoint_segments) {
            for (const auto& seg : segs) {
                SegmentInterval si;
                si.line_start = seg.output_line_start;
                si.line_end = seg.output_line_end;
                si.original_idx = orig_idx;
                per_reorg_segments[seg.reorg_file].push_back(si);
            }
        }
    }

    for (auto& [file, segs] : per_reorg_segments) {
        std::sort(segs.begin(), segs.end(),
                  [](const SegmentInterval& a, const SegmentInterval& b) {
                      return a.line_start < b.line_start;
                  });
    }

    // Create channels indexed by original file index
    std::vector<std::shared_ptr<coro::Channel<ReconstructLineBatch>>> channels;
    channels.reserve(original_paths.size());
    for (std::size_t i = 0; i < original_paths.size(); ++i) {
        channels.push_back(
            std::make_shared<coro::Channel<ReconstructLineBatch>>(16));
    }

    std::atomic<std::size_t> total_events{0};
    std::atomic<std::size_t> total_bytes{0};
    std::vector<ReconstructedFileInfo> file_results;
    std::mutex results_mutex;

    WriterContext wctx;
    wctx.output_dir = input.output_dir;
    wctx.compress = input.compress;
    wctx.total_events = &total_events;
    wctx.total_bytes = &total_bytes;
    wctx.file_results = &file_results;
    wctx.results_mutex = &results_mutex;

    // Spawn writers (consumers)
    for (std::size_t i = 0; i < original_paths.size(); ++i) {
        ctx.spawn([channel = channels[i], orig_path = original_paths[i],
                   wctx](CoroScope&) -> coro::CoroTask<void> {
            co_await run_writer(channel, std::move(orig_path), wctx);
        });
    }

    auto parallelism = input.parallelism > 0
                           ? input.parallelism
                           : dftracer_utils_hardware_concurrency();

    ReaderContext rctx;
    rctx.checkpoint_size = input.checkpoint_size;
    rctx.original_paths = &original_paths;
    rctx.channels = &channels;

    auto* per_reorg_ptr = &per_reorg_segments;
    auto* rctx_ptr = &rctx;

    co_await ctx.scope([per_reorg_ptr, rctx_ptr, parallelism](
                           CoroScope& producer_scope) -> coro::CoroTask<void> {
        auto permits = coro::make_channel<bool>(parallelism * 2);
        for (std::size_t i = 0; i < parallelism * 2; ++i) {
            permits->try_send(true);
        }

        for (auto& [reorg_file, intervals] : *per_reorg_ptr) {
            const auto* intervals_ptr = &intervals;
            auto reorg_file_copy = reorg_file;

            producer_scope.spawn(
                [reorg_file_copy, intervals_ptr, rctx_ptr,
                 permits](CoroScope& s) -> coro::CoroTask<void> {
                    co_await s.receive(permits);
                    try {
                        co_await run_reader(s, std::move(reorg_file_copy),
                                            intervals_ptr, *rctx_ptr);
                        permits->try_send(true);
                    } catch (...) {
                        permits->try_send(true);
                        throw;
                    }
                });
        }

        co_return;
    });

    // Producers done: close channels
    for (auto& channel : channels) {
        channel->close();
    }

    // Wait for writers
    co_await ctx.join_all();

    result.files = std::move(file_results);
    result.total_events = total_events.load();
    result.total_bytes = total_bytes.load();
    result.success = true;

    co_return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::reorganize
