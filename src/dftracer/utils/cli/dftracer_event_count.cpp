// SPDX-License-Identifier: MIT
#include <dftracer/utils/cli/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/trace/index/build/resolver.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <fcntl.h>
#include <simdjson.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index::build;

class EventCountArgParse : public cli::ArgParse {
   public:
    cli::InputArgs inputs{cli::DirMode::DefaultDot,
                          "Trace files or a folder of trace files"};
    cli::RuntimeArgs runtime_args;
    cli::IndexingArgs indexing;

    explicit EventCountArgParse(cli::ArgParser& p) : ArgParse(p) {
        indexing.index_dir_help =
            "Directory that holds .dftindex stores to read counts from "
            "(never written)";
        indexing.force_help =
            "Ignore any index and stream every file to count its events";
        indexing.with_checkpoint = false;
        schema(inputs, runtime_args, indexing);
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
    dstd::Vector<dstd::String> to_stream;
};

dstd::Result<IndexedCounts> read_index_counts(
    dstd::StringView index_path, const dstd::Vector<ResolvedFile>& entries) {
    IndexedCounts counts;
    auto snapshot =
        dftracer::utils::trace::index::plan::open_snapshot(index_path);
    if (!snapshot) return snapshot.error();
    for (const auto& entry : entries) {
        const auto file =
            snapshot->file(fileio::get_logical_path(entry.file_path));
        bool counted = false;
        if (file) {
            auto scalar =
                dftracer::utils::trace::index::scalar_stats(*snapshot, *file);
            if (!scalar) return scalar.error();
            if (*scalar && (*scalar)->chunk_count > 0) {
                counts.total_events +=
                    static_cast<std::size_t>((*scalar)->stats.total_events);
                counts.files_counted++;
                counted = true;
            }
        }
        if (!counted) counts.to_stream.push_back(entry.file_path);
    }
    return counts;
}

IndexedCounts count_from_index(dstd::StringView index_path,
                               const dstd::Vector<ResolvedFile>& entries) {
    auto counts = read_index_counts(index_path, entries);
    if (counts) return std::move(*counts);
    DFTRACER_UTILS_LOG_WARN("Cannot read index {} ({}); streaming its files",
                            index_path, counts.error().c_str());
    IndexedCounts fallback;
    for (const auto& entry : entries)
        fallback.to_stream.push_back(entry.file_path);
    return fallback;
}

class StreamCounter {
   public:
    explicit StreamCounter(bool path_decoder) : path_decoder_(path_decoder) {}

    std::size_t count() const { return count_; }
    std::size_t dropped_count() const { return dropped_; }

    // Whole lines go into work_ because parse_lines needs SIMDJSON_PADDING
    // after them and strip_array_delimiters writes in place.
    // A line the DOM parser refuses is read on demand, as in the index build.
    // A last line with no newline is never parsed, as in the index build.
    void feed(dstd::StringView chunk) {
        const std::size_t cut = chunk.rfind('\n');
        if (cut == dstd::StringView::npos) {
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
            [&](simdjson::dom::element root, dstd::StringView) {
                if (path_decoder_ ? root.is_object()
                                  : trace::views::detail::counts_as_event(root))
                    ++count_;
            },
            [&](dstd::StringView line, std::size_t capacity) {
                auto parsed = document_.parse_in_place(line.data(), line.size(),
                                                       capacity);
                if (!parsed) {
                    ++dropped_;
                    return;
                }
                // The path decoder keeps a record the index build reads whole.
                if (path_decoder_)
                    trace::views::detail::decode_record_into(
                        ev_, *parsed, intern_, true, nullptr, nullptr, nullptr,
                        trace::views::detail::INDEX_MAX_CHILDREN);
                const bool counted =
                    path_decoder_
                        ? true
                        : trace::views::detail::counts_as_event(*parsed);
                if (json::detail::ValueAccess::broken(*parsed))
                    ++dropped_;
                else if (counted)
                    ++count_;
            });
    }

   private:
    simdjson::dom::parser parser_;
    json::Document document_;
    dstd::String carry_;
    dstd::String work_;
    dftracer::utils::dstd::StringIntern intern_;
    trace::views::detail::FoldEvent ev_;
    std::size_t count_ = 0;
    std::size_t dropped_ = 0;
    bool path_decoder_;
};

coro::CoroTask<dstd::Result<std::size_t>> stream_count(dstd::String path) {
    const dstd::OpenOptions open_options;
    auto opening = runtime::io::open_file(dstd::PathView(path), open_options);
    auto file = co_await std::move(opening);
    if (!file)
        co_return common::make_error<"cannot open {}">(common::ErrorCode::Io,
                                                       path);
    const int fd = file->native_handle();
    struct stat st;
    const ssize_t stat_result = co_await runtime::io::fstat(fd, &st);
    if (stat_result != 0)
        co_return common::make_error<"cannot stat {}">(common::ErrorCode::Io,
                                                       path);
    const auto size = static_cast<std::uint64_t>(st.st_size);
    auto detected = trace::index::detect_file_schema(path, *file, size,
                                                     common::stat_mtime_ns(st));
    if (!detected) co_return detected.error();
    const auto& schema = **detected;
    StreamCounter counter(schema.decoder == trace::index::Decoder::Path);

    unsigned char magic[2] = {0, 0};
    const auto magic_read = co_await runtime::io::pread(fd, magic, 2, 0);
    if (magic_read == 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
        auto members =
            fileio::compress::decode_gzip_members(fd, size, INFLATE_STEP_BYTES);
        for (;;) {
            auto chunk = co_await members.next();
            if (!chunk) break;
            if (!*chunk) co_return chunk->error();
            counter.feed(**chunk);
        }
    } else {
        dstd::String buf(PLAIN_READ_BYTES, '\0');
        std::uint64_t offset = 0;
        while (offset < size) {
            const auto got =
                co_await runtime::io::pread(fd, buf.data(), buf.size(), offset);
            if (got < 0)
                co_return common::make_error<"cannot read {}">(
                    common::ErrorCode::Io, path);
            if (got == 0) break;
            counter.feed(
                dstd::StringView(buf.data(), static_cast<std::size_t>(got)));
            offset += static_cast<std::uint64_t>(got);
        }
    }
    if (counter.dropped_count() > 0)
        DFTRACER_UTILS_LOG_WARN("{}: {} line(s) could not be read in full",
                                path, counter.dropped_count());
    co_return counter.count();
}

}  // namespace

int main(int argc, char** argv) {
    return cli::cli_main<EventCountArgParse>(
        argc, argv, "dftracer_event_count",
        "Count valid events in DFTracer .pfw.gz files using composable "
        "utilities",
        [](EventCountArgParse& cli) { return run_event_count(&cli); });
}

static int run_event_count(const EventCountArgParse* cli) {
    dstd::Path log_dir;
    if (!cli->inputs.directory.empty()) {
        auto absolute = cli::absolute_path(cli->inputs.directory);
        if (!absolute) return cli::report_error(absolute.error());
        log_dir = std::move(*absolute);
    }
    const dstd::String index_dir(cli->indexing.index_dir);
    const auto force = cli->indexing.force;
    const auto executor_threads = cli->runtime_args.executor_threads;

    Resolver resolver;
    ResolverInput resolve_input;
    resolve_input.directory = dstd::String(log_dir.str());
    for (const auto file : cli->inputs.files) {
        auto absolute = cli::absolute_path(file);
        if (!absolute) return cli::report_error(absolute.error());
        resolve_input.files.emplace_back(absolute->str());
    }
    resolve_input.index_dir = index_dir;
    resolve_input.require_checkpoints = true;

    auto resolved = resolver(resolve_input).get();
    if (!resolved) {
        DFTRACER_UTILS_LOG_ERROR("{}", resolved.error().format());
        return 1;
    }
    auto& resolve_result = *resolved;

    if (resolve_result.all_files.empty()) {
        DFTRACER_UTILS_LOG_ERROR("No trace files found in: {}", log_dir);
        return 1;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    std::atomic<std::size_t> total_events{0};
    std::atomic<std::size_t> files_processed{0};
    std::atomic<bool> failed{false};

    dstd::Vector<dstd::String> stream_items;
    dstd::HashMap<dstd::String, dstd::Vector<ResolvedFile>> index_groups;
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

    auto ran = cli::run_on_pool(
        "DFTracer Event Count", cli->runtime_args,
        [&index_groups, &stream_items, &total_events, &files_processed, &failed,
         executor_threads](coro::Scope&) -> coro::CoroTask<void> {
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
            co_await coro::with_scope(
                [&](coro::Scope& scope) -> coro::CoroTask<void> {
                    cli::parallel_for_each(
                        scope, std::move(stream_items), executor_threads,
                        [total_events_ptr = &total_events,
                         files_processed_ptr = &files_processed,
                         failed_ptr = &failed](
                            const dstd::String& path) -> coro::CoroTask<void> {
                            auto counting = stream_count(path);
                            auto counted = co_await std::move(counting);
                            if (counted) {
                                total_events_ptr->fetch_add(
                                    *counted, std::memory_order_relaxed);
                                files_processed_ptr->fetch_add(
                                    1, std::memory_order_relaxed);
                            } else {
                                DFTRACER_UTILS_LOG_ERROR(
                                    "Cannot count {}: {}", path,
                                    counted.error().c_str());
                                failed_ptr->store(true,
                                                  std::memory_order_relaxed);
                            }
                        });
                    co_return;
                });
            co_return;
        });
    if (!ran) return cli::report_error(ran.error());

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    if (failed.load()) return 1;

    std::printf("%zu\n", total_events.load());

    DFTRACER_UTILS_LOG_DEBUG("Completed in {:.2f} ms", duration.count());
    DFTRACER_UTILS_LOG_DEBUG("Files processed: {}", files_processed.load());

    return 0;
}
