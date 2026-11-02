// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/runtime/pipeline/pipeline.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_member_reader.h>
#include <fcntl.h>
#include <simdjson.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::index::build;

class EventCountArgParse : public binaries::ArgParse {
   public:
    binaries::DirectoryArgs directory;
    binaries::PipelineArgs pipeline;
    binaries::IndexingArgs indexing;

    explicit EventCountArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        indexing.index_dir_help =
            "Directory that holds .dftindex stores to read counts from "
            "(never written)";
        indexing.force_help =
            "Ignore any index and stream every file to count its events";
        indexing.with_checkpoint = false;
        schema(directory, pipeline, indexing);
    }
};

static int run_event_count(const EventCountArgParse* cli);

namespace {

constexpr std::size_t PLAIN_READ_BYTES = std::size_t{4} << 20;
// Each in-flight file holds this much inflated text plus its copy. With many
// threads 1 MiB beats the decoder's 32 MiB default in time and peak memory.
constexpr std::size_t INFLATE_STEP_BYTES = std::size_t{1} << 20;

struct IndexedCounts {
    std::size_t total_events = 0;
    std::size_t files_counted = 0;
    std::vector<std::string> to_stream;
};

IndexedCounts count_from_index(const std::string& index_path,
                               const std::vector<ResolvedFile>& entries) {
    IndexedCounts counts;
    try {
        const auto snapshot = common::unwrap(
            dftracer::utils::index::plan::open_snapshot(index_path));
        for (const auto& entry : entries) {
            const auto file = snapshot.file(
                dftracer::utils::index::store::internal::get_logical_path(
                    entry.file_path));
            const auto stats =
                file ? common::unwrap(dftracer::utils::index::scalar_stats(
                           snapshot, *file))
                     : std::nullopt;
            if (stats && stats->num_chunks > 0) {
                counts.total_events +=
                    static_cast<std::size_t>(stats->stats.total_events);
                counts.files_counted++;
            } else {
                counts.to_stream.push_back(entry.file_path);
            }
        }
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_WARN(
            "Cannot read index %s (%s); streaming its files",
            index_path.c_str(), e.what());
        counts = IndexedCounts{};
        for (const auto& entry : entries)
            counts.to_stream.push_back(entry.file_path);
    }
    return counts;
}

class StreamCounter {
   public:
    explicit StreamCounter(bool path_decoder) : path_decoder_(path_decoder) {}

    std::size_t count() const { return count_; }

    // Whole lines go into work_ because parse_lines needs SIMDJSON_PADDING
    // after them and strip_array_delimiters writes in place.
    // A last line with no newline is never parsed, as in the index build.
    void feed(std::string_view chunk) {
        const std::size_t cut = chunk.rfind('\n');
        if (cut == std::string_view::npos) {
            carry_.append(chunk);
            return;
        }
        const std::size_t whole = cut + 1;
        work_.assign(carry_);
        work_.append(chunk.data(), whole);
        const std::size_t total = work_.size();
        carry_.assign(chunk.data() + whole, chunk.size() - whole);
        trace::strip_array_delimiters(work_.data(), total);
        work_.resize(total + simdjson::SIMDJSON_PADDING, '\0');
        trace::parse_lines(
            parser_, work_.data(), total,
            [&](simdjson::dom::element root, std::string_view) {
                if (path_decoder_ ? root.is_object()
                                  : trace::views::detail::counts_as_event(root))
                    ++count_;
            });
    }

   private:
    simdjson::dom::parser parser_;
    std::string carry_;
    std::string work_;
    std::size_t count_ = 0;
    bool path_decoder_;
};

coro::CoroTask<std::size_t> stream_count(std::string path) {
    dstd::ScopedFd fd(::open(path.c_str(), O_RDONLY));
    if (fd.get() < 0)
        throw common::DFTUtilsException::cat(common::ErrorCode::Io,
                                             "cannot open ", path);
    struct stat st;
    if (::fstat(fd.get(), &st) != 0)
        throw common::DFTUtilsException::cat(common::ErrorCode::Io,
                                             "cannot stat ", path);
    const auto size = static_cast<std::uint64_t>(st.st_size);
    const auto& schema = index::detect_file_schema(path, fd.get(), size,
                                                   common::stat_mtime_ns(st));
    StreamCounter counter(schema.decoder == index::Decoder::Path);

    unsigned char magic[2] = {0, 0};
    const auto magic_read = co_await io::pread(fd.get(), magic, 2, 0);
    if (magic_read == 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
        auto members = utilities::fileio::compress::decode_gzip_members(
            fd.get(), size, INFLATE_STEP_BYTES);
        for (;;) {
            auto chunk = co_await members.next();
            if (!chunk) break;
            counter.feed(*chunk);
        }
    } else {
        std::string buf(PLAIN_READ_BYTES, '\0');
        std::uint64_t offset = 0;
        while (offset < size) {
            const auto got = co_await io::pread(
                fd.get(), buf.data(), buf.size(), static_cast<off_t>(offset));
            if (got < 0)
                throw common::DFTUtilsException::cat(common::ErrorCode::Io,
                                                     "cannot read ", path);
            if (got == 0) break;
            counter.feed(
                std::string_view(buf.data(), static_cast<std::size_t>(got)));
            offset += static_cast<std::uint64_t>(got);
        }
    }
    co_return counter.count();
}

}  // namespace

int main(int argc, char** argv) {
    return binaries::cli_main<EventCountArgParse>(
        argc, argv, "dftracer_event_count",
        "Count valid events in DFTracer .pfw.gz files using composable "
        "utilities and pipeline processing",
        [](EventCountArgParse& cli) { return run_event_count(&cli); });
}

static int run_event_count(const EventCountArgParse* cli) {
    const auto log_dir = binaries::absolute_path(cli->directory.value);
    const auto index_dir = cli->indexing.index_dir;
    const auto force = cli->indexing.force;
    const auto executor_threads = cli->pipeline.executor_threads;

    Resolver resolver;
    ResolverInput resolve_input;
    resolve_input.directory = log_dir;
    resolve_input.index_dir = index_dir;
    resolve_input.require_checkpoints = true;

    auto resolve_result = resolver(resolve_input).get();

    if (resolve_result.all_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No trace files found in: %s",
                                 log_dir.c_str());
        return 1;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    std::atomic<std::size_t> total_events{0};
    std::atomic<std::size_t> files_processed{0};
    std::atomic<bool> failed{false};

    std::vector<std::string> stream_items;
    std::map<std::string, std::vector<ResolvedFile>> index_groups;
    for (auto& entry : resolve_result.cached) {
        if (force) {
            stream_items.push_back(std::move(entry.file_path));
            continue;
        }
        auto index_path =
            trace::internal::determine_index_path(entry.file_path, index_dir);
        index_groups[index_path].push_back(std::move(entry));
    }
    for (auto& item : resolve_result.needs_checkpoint)
        stream_items.push_back(std::move(item.file_path));

    auto pipeline_config =
        binaries::build_pipeline_config("DFTracer Event Count", cli->pipeline);
    Pipeline pipeline(pipeline_config);

    auto count_task = make_task(
        [&index_groups, &stream_items, &total_events, &files_processed, &failed,
         executor_threads](CoroScope& ctx) -> coro::CoroTask<void> {
            for (const auto& [index_path, entries] : index_groups) {
                auto counts = count_from_index(index_path, entries);
                total_events.fetch_add(counts.total_events,
                                       std::memory_order_relaxed);
                files_processed.fetch_add(counts.files_counted,
                                          std::memory_order_relaxed);
                for (auto& path : counts.to_stream)
                    stream_items.push_back(std::move(path));
            }

            if (stream_items.empty()) co_return;
            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                binaries::parallel_for_each(
                    scope, std::move(stream_items), executor_threads,
                    [total_events_ptr = &total_events,
                     files_processed_ptr = &files_processed,
                     failed_ptr = &failed](
                        const std::string& path) -> coro::CoroTask<void> {
                        try {
                            const std::size_t n = co_await stream_count(path);
                            total_events_ptr->fetch_add(
                                n, std::memory_order_relaxed);
                            files_processed_ptr->fetch_add(
                                1, std::memory_order_relaxed);
                        } catch (const std::exception& e) {
                            DFTRACER_UTILS_LOG_ERROR("Cannot count %s: %s",
                                                     path.c_str(), e.what());
                            failed_ptr->store(true, std::memory_order_relaxed);
                        }
                    });
                co_return;
            });
            co_return;
        },
        "Count");

    pipeline.set_source(count_task);
    pipeline.set_destination(count_task);
    pipeline.execute();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    if (failed.load()) return 1;

    std::printf("%zu\n", total_events.load());

    DFTRACER_UTILS_LOG_DEBUG("Completed in %.2f ms", duration.count());
    DFTRACER_UTILS_LOG_DEBUG("Files processed: %zu", files_processed.load());

    return 0;
}
