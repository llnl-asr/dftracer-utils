#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/event_router.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/provenance_tracker.h>
#include <dftracer/utils/utilities/fileio/chunk_writer.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>

#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::reorganize {

using fileio::ChunkWriter;
using fileio::ChunkWriterConfig;

namespace {

struct SourceResult {
    std::size_t events_written = 0;
    std::size_t bytes_written = 0;
    std::size_t chunks_created = 0;
    std::vector<std::string> output_files;
    bool success = false;
};

struct Cursor {
    const ExtractionTask* task;
    std::size_t pos = 0;
};

coro::CoroTask<SourceResult> process_source(
    std::size_t source_idx, const EventRouterConfig& config,
    const std::vector<const ExtractionTask*>& tasks) {
    SourceResult result;
    const auto& plan = config.plan;
    const auto& src = plan.source_files[source_idx];

    std::unordered_map<std::string_view, std::size_t> group_idx;
    std::vector<std::unique_ptr<ChunkWriter>> writers;
    std::vector<ProvenanceTracker> trackers;

    writers.resize(plan.groups.size());
    trackers.resize(plan.groups.size());

    for (std::size_t gi = 0; gi < plan.groups.size(); ++gi) {
        const auto& g = plan.groups[gi];
        group_idx[g.name] = gi;
        auto writer_config =
            ChunkWriterConfig()
                .with_output_dir(config.output_dir + "/" + g.name)
                .with_base_name("src" + std::to_string(source_idx))
                .with_chunk_size(config.chunk_size_bytes)
                .with_compression(config.compress);
        writers[gi] = std::make_unique<ChunkWriter>(writer_config);
    }

    for (auto& writer : writers) {
        co_await writer->open();
    }

    struct CheckpointKey {
        std::uint64_t checkpoint_idx;
        bool operator==(const CheckpointKey& o) const {
            return checkpoint_idx == o.checkpoint_idx;
        }
    };
    struct CheckpointKeyHash {
        std::size_t operator()(const CheckpointKey& k) const {
            return std::hash<std::uint64_t>{}(k.checkpoint_idx);
        }
    };

    std::unordered_map<std::uint64_t, std::vector<const ExtractionTask*>>
        checkpoint_tasks;
    for (const auto* task : tasks) {
        checkpoint_tasks[task->checkpoint_idx].push_back(task);
    }

    reader::TraceReader trace_reader(reader::TraceReaderConfig{
        .file_path = src.file_path,
        .index_dir = config.index_dir,
        .checkpoint_size = config.checkpoint_size > 0 ? config.checkpoint_size
                                                      : src.checkpoint_size});

    for (const auto& [ckpt_idx, ckpt_tasks] : checkpoint_tasks) {
        std::vector<Cursor> cursors;
        for (const auto* task : ckpt_tasks) {
            if (!task->line_numbers.empty()) {
                cursors.push_back({task, 0});
            }
        }

        reader::ReadConfig read_config;
        read_config.start_byte = ckpt_tasks[0]->start_byte;
        read_config.end_byte = ckpt_tasks[0]->end_byte;

        auto line_gen = trace_reader.read_lines(read_config);

        std::uint32_t line_number = 0;
        std::vector<int> chunk_line_counts(plan.groups.size(), 0);
        std::vector<int> ckpt_event_counts(plan.groups.size(), 0);

        while (auto line_opt = co_await line_gen.next()) {
            const auto& line = *line_opt;
            if (line.content.empty()) {
                line_number++;
                continue;
            }

            const char* trimmed;
            std::size_t trimmed_length;
            bool valid =
                json_trim_and_validate(line.content.data(), line.content.size(),
                                       trimmed, trimmed_length);

            for (auto& c : cursors) {
                if (c.pos < c.task->line_numbers.size() &&
                    c.task->line_numbers[c.pos] == line_number) {
                    c.pos++;

                    if (valid && trimmed_length > 2) {
                        auto gi = group_idx[c.task->target_group];
                        co_await writers[gi]->write_line(
                            ByteView(trimmed, trimmed_length));
                        chunk_line_counts[gi]++;
                        ckpt_event_counts[gi]++;
                    }
                }
            }

            line_number++;
        }

        for (std::size_t gi = 0; gi < plan.groups.size(); ++gi) {
            int count = ckpt_event_counts[gi];
            if (count == 0) continue;
            int line_end = chunk_line_counts[gi];
            int line_start = line_end - count;
            trackers[gi].record(static_cast<int>(source_idx),
                                static_cast<int>(ckpt_idx),
                                writers[gi]->current_chunk_index(), line_start,
                                line_end, count);
            ckpt_event_counts[gi] = 0;
        }
    }

    for (std::size_t gi = 0; gi < plan.groups.size(); ++gi) {
        co_await writers[gi]->close();
        result.events_written += writers[gi]->total_events_written();
        result.bytes_written += writers[gi]->total_bytes_written();
        result.chunks_created += writers[gi]->chunks().size();

        for (const auto& chunk : writers[gi]->chunks()) {
            result.output_files.push_back(chunk.path);
        }

        co_await trackers[gi].flush_to_db(
            plan, plan.groups[gi].name, plan.groups[gi].query,
            writers[gi]->chunks(), config.output_dir);
    }

    result.success = true;
    co_return result;
}

}  // namespace

coro::CoroTask<Result<EventRouterResult>> route_events(
    CoroScope& scope, const EventRouterConfig& config) {
    EventRouterResult result;
    const auto& plan = config.plan;

    std::unordered_map<std::size_t, std::vector<const ExtractionTask*>>
        tasks_by_source;
    for (const auto& task : plan.tasks) {
        tasks_by_source[task.source_file_idx].push_back(&task);
    }

    try {
        auto permits = coro::make_channel<bool>(config.executor_threads * 2);
        for (std::size_t i = 0; i < config.executor_threads * 2; ++i) {
            permits->try_send(true);
        }

        std::vector<coro::SpawnFuture<SourceResult>> futures;
        futures.reserve(tasks_by_source.size());

        for (const auto& [src_idx, src_tasks] : tasks_by_source) {
            auto* config_ptr = &config;
            futures.push_back(
                scope.spawn([src_idx, config_ptr, tasks = src_tasks, permits](
                                CoroScope& s) -> coro::CoroTask<SourceResult> {
                    co_await s.receive(permits);
                    try {
                        auto r = co_await process_source(src_idx, *config_ptr,
                                                         tasks);
                        permits->try_send(true);
                        co_return r;
                    } catch (...) {
                        permits->try_send(true);
                        throw;
                    }
                }));
        }

        for (auto& future : futures) {
            auto src_result = co_await future;
            if (src_result.success) {
                result.total_events_written += src_result.events_written;
                result.total_bytes_written += src_result.bytes_written;
                result.chunks_created += src_result.chunks_created;
                result.source_files_processed++;
                result.output_files.insert(result.output_files.end(),
                                           src_result.output_files.begin(),
                                           src_result.output_files.end());
            }
        }

        if (result.source_files_processed != tasks_by_source.size()) {
            co_return make_error(
                ErrorCode::INTERNAL,
                "route_events: only " +
                    std::to_string(result.source_files_processed) + " of " +
                    std::to_string(tasks_by_source.size()) +
                    " source files were routed successfully");
        }
        co_return result;
    } catch (const std::exception& e) {
        co_return make_error(
            ErrorCode::IO,
            std::string("route_events failed while routing events: ") +
                e.what());
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::reorganize
