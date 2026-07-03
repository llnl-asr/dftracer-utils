#include <dftracer/utils/core/common/checkpointer.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/indexer/error.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/index_visitor.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint_size.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_checkpointer.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_inflater.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_member_scanner.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/transaction_scope.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::indexer::internal::gzip {

using dftracer::utils::utilities::indexer::IndexDatabase;

namespace {

void finalize_checkpoints(std::vector<IndexerCheckpoint>& checkpoints,
                          std::uint64_t total_uc_size,
                          std::uint64_t total_lines,
                          std::uint64_t tail_line_count) {
    for (std::size_t i = 0; i < checkpoints.size(); ++i) {
        auto& checkpoint = checkpoints[i];
        const std::uint64_t next_uc_offset = (i + 1 < checkpoints.size())
                                                 ? checkpoints[i + 1].uc_offset
                                                 : total_uc_size;
        const std::uint64_t next_c_offset = (i + 1 < checkpoints.size())
                                                ? checkpoints[i + 1].c_offset
                                                : checkpoint.c_offset;
        checkpoint.uc_size = next_uc_offset - checkpoint.uc_offset;
        checkpoint.c_size = next_c_offset - checkpoint.c_offset;
    }

    if (tail_line_count > 0 && total_lines > 0 && !checkpoints.empty()) {
        auto& last = checkpoints.back();
        last.last_line_num = total_lines;
        last.num_lines += tail_line_count;
    }
}

static dftracer::utils::coro::CoroTask<bool> process_chunks_serial(
    int fd, std::uint64_t ckpt_size, std::uint64_t& total_lines,
    std::uint64_t& total_uc_size, std::uint64_t& tail_line_count,
    std::vector<IndexerCheckpoint>& checkpoints,
    const Indexer::VisitorList& visitors) {
    GzipInflater inflater;
    off_t offset = 0;
    if (!(co_await inflater.initialize(fd))) {
        co_return false;
    }

    std::uint64_t checkpoint_idx = 0;
    std::uint64_t current_uc_offset = 0;
    std::uint64_t next_ckpt_offset = ckpt_size;
    std::uint64_t line_count_in_chunk = 0;
    std::uint64_t first_line_in_chunk = total_lines + 1;

    const bool has_visitors = !visitors.empty();

    while (true) {
        GzipInflaterResult result;
        if (!(co_await inflater.read(fd, offset, result))) {
            if (result.bytes_read == 0) {
                break;
            }
            co_return false;
        }

        if (result.bytes_read == 0) {
            break;
        }

        current_uc_offset += result.bytes_read;
        total_lines += result.lines_found;
        line_count_in_chunk += result.lines_found;

        if (has_visitors) {
            const char* data =
                reinterpret_cast<const char*>(inflater.out_buffer());
            for (auto& visitor : visitors) {
                co_await visitor.get().on_chunk(data, result.bytes_read,
                                                checkpoint_idx);
            }
            for (auto& visitor : visitors) {
                if (visitor.get().wants_drain()) {
                    co_await visitor.get().drain_pending();
                }
            }
        }

        if (current_uc_offset >= next_ckpt_offset && result.at_block_boundary) {
            const std::size_t chunk_start_uc = current_uc_offset;
            const std::size_t chunk_start_c =
                inflater.get_total_input_consumed();

            GzipCheckpointer checkpointer(inflater, chunk_start_uc);
            if (checkpointer.create(chunk_start_c)) {
                std::vector<unsigned char> compressed_dict;
                if (checkpointer.compress(compressed_dict)) {
                    IndexerCheckpoint checkpoint{
                        .checkpoint_idx = checkpoint_idx++,
                        .uc_offset = chunk_start_uc,
                        .uc_size = 0,
                        .c_offset = chunk_start_c,
                        .c_size = 0,
                        .bits = checkpointer.bits,
                        .dict_compressed = std::move(compressed_dict),
                        .num_lines = line_count_in_chunk,
                        .first_line_num = first_line_in_chunk,
                        .last_line_num = total_lines,
                    };
                    checkpoints.push_back(std::move(checkpoint));

                    if (has_visitors) {
                        for (auto& visitor : visitors) {
                            co_await visitor.get().on_checkpoint(
                                checkpoint_idx - 1);
                        }
                    }

                    line_count_in_chunk = 0;
                    first_line_in_chunk = total_lines + 1;
                    next_ckpt_offset = current_uc_offset + ckpt_size;
                }
            }
        }
    }

    if (has_visitors) {
        for (auto& visitor : visitors) {
            co_await visitor.get().flush();
        }
    }

    total_uc_size = current_uc_offset;
    tail_line_count = line_count_in_chunk;
    co_return true;
}

// -- Parallel path ---------------------------------------------------------
//
// When the file is multi-member gzip (the dftracer runtime format), divide
// the members across N worker coroutines and stream inflated chunks through
// per-worker channels to a single dispatcher.
//
// Checkpoints are emitted by workers at mid-range deflate-block boundaries
// using GzipCheckpointer (identical semantics to the serial path). The
// dispatcher finalises each checkpoint with global uc_offset / line numbers
// and pushes it into the shared `checkpoints` vector in order.

struct ParallelInflateMsg {
    std::unique_ptr<std::vector<unsigned char>> data;
    // Per-worker monotonic sequence. Load-bearing: moodycamel (our channel
    // backend) does not guarantee strict FIFO without producer tokens, so
    // the dispatcher reorders by this before handing chunks to visitors.
    std::uint64_t seq = 0;
    std::uint64_t lines = 0;
    bool has_checkpoint = false;
    std::vector<unsigned char> dict_compressed;
    int bits = 0;
    std::uint64_t ckpt_c_offset = 0;
};

using ParallelChan = dftracer::utils::coro::Channel<ParallelInflateMsg>;

static dftracer::utils::coro::CoroTask<bool> parallel_worker(
    int fd, std::uint64_t range_c_start, std::uint64_t range_c_end,
    std::uint64_t ckpt_size, bool strip_leading_partial,
    bool extend_to_newline_past_end,
    dftracer::utils::coro::ChannelProducer<ParallelInflateMsg> producer) {
    auto guard = producer.guard();

    GzipInflater inflater;
    if (!(co_await inflater.initialize(fd, range_c_start))) {
        co_return false;
    }

    off_t offset = static_cast<off_t>(range_c_start);
    const std::uint64_t range_c_size = range_c_end - range_c_start;
    std::uint64_t local_uc = 0;
    std::uint64_t last_ckpt_uc = 0;
    std::uint64_t seq = 0;
    bool leading_partial_done = !strip_leading_partial;
    // When extending past end: once get_total_input_consumed() hits
    // range_c_size, we keep reading (uncapped) until the uncompressed
    // output contains a `\n`; the byte right after that `\n` belongs
    // to the next slice, so we truncate this final chunk there. This
    // captures the single split line that straddles the slice boundary
    // so it isn't double-counted (the next slice strips its own leading
    // partial prefix).
    bool extending = false;
    bool emit_done = false;

    while (true) {
        GzipInflaterResult result;
        // Cap pread at range_c_size normally. While extending (slice's
        // last worker that must consume the straddling split line) the
        // cap is lifted so we can read into the next slice's bytes far
        // enough to find a `\n`.
        const std::size_t input_cap = extending ? 0 : range_c_size;
        if (!(co_await inflater.read(fd, offset, result, input_cap))) {
            co_return false;
        }
        if (result.bytes_read == 0) {
            // `bytes_read==0` here can mean either true EOF or that the
            // input cap was hit mid-inflate (inflater break'd out with
            // no new output). For the latter, if this worker still
            // needs to swallow the straddling split line, flip into
            // extend mode and retry with an uncapped read.
            if (extend_to_newline_past_end && !emit_done && !extending) {
                extending = true;
                continue;
            }
            break;
        }

        // First-chunk leading-partial-line strip: when this worker is
        // the first of a mid-file slice (range_c_start at a non-initial
        // member's header), the first inflated bytes continue a line
        // from the previous member (not owned by this slice). Skip up
        // to and including the first `\n` so the dispatcher sees a
        // clean line-boundary start.
        std::size_t skip_prefix = 0;
        if (!leading_partial_done) {
            const unsigned char* out = inflater.out_buffer();
            for (std::size_t i = 0; i < result.bytes_read; ++i) {
                if (out[i] == '\n') {
                    skip_prefix = i + 1;
                    leading_partial_done = true;
                    break;
                }
            }
            if (!leading_partial_done) {
                // No newline in this chunk; entire chunk is continuation
                // of the previous slice's line. Count bytes but emit
                // nothing and loop for more.
                local_uc += result.bytes_read;
                if (inflater.get_total_input_consumed() >= range_c_size) break;
                continue;
            }
        }

        std::size_t emit_len = result.bytes_read - skip_prefix;
        local_uc += result.bytes_read;

        // Extending past end: truncate the emit buffer at the first `\n`
        // at/after the boundary. Everything after that belongs to the
        // next slice.
        if (extending && !emit_done) {
            const unsigned char* p = inflater.out_buffer() + skip_prefix;
            for (std::size_t i = 0; i < emit_len; ++i) {
                if (p[i] == '\n') {
                    emit_len = i + 1;
                    emit_done = true;
                    break;
                }
            }
        }

        ParallelInflateMsg msg;
        msg.seq = seq++;
        // Line count adjusted for stripped prefix: approximate by
        // counting newlines in the emitted region (cheap enough; pipeline
        // uses counts only for statistics, not for correctness).
        if (skip_prefix == 0) {
            msg.lines = result.lines_found;
        } else {
            std::uint64_t lines = 0;
            const unsigned char* p = inflater.out_buffer() + skip_prefix;
            for (std::size_t i = 0; i < emit_len; ++i) {
                if (p[i] == '\n') ++lines;
            }
            msg.lines = lines;
        }
        msg.data = std::make_unique<std::vector<unsigned char>>(
            inflater.out_buffer() + skip_prefix,
            inflater.out_buffer() + skip_prefix + emit_len);

        if (result.at_block_boundary && ckpt_size > 0 &&
            local_uc - last_ckpt_uc >= ckpt_size) {
            const std::uint64_t absolute_c =
                range_c_start + inflater.get_total_input_consumed();
            GzipCheckpointer cp(inflater, static_cast<std::size_t>(local_uc));
            if (cp.create(static_cast<std::size_t>(absolute_c))) {
                std::vector<unsigned char> dict;
                if (cp.compress(dict)) {
                    msg.has_checkpoint = true;
                    msg.dict_compressed = std::move(dict);
                    msg.bits = cp.bits;
                    msg.ckpt_c_offset = absolute_c;
                    last_ckpt_uc = local_uc;
                }
            }
        }

        if (!(co_await producer.send(std::move(msg)))) break;

        if (inflater.get_total_input_consumed() >= range_c_size) {
            // Normal worker: stop at slice boundary.
            // Extending worker: stop after we've emitted up to and
            // including the first `\n` past the boundary.
            if (!extend_to_newline_past_end) break;
            if (emit_done) break;
            extending = true;
        }
    }

    co_return true;
}

static dftracer::utils::coro::CoroTask<bool> parallel_dispatcher(
    const std::vector<std::shared_ptr<ParallelChan>>& chans,
    std::uint64_t checkpoint_idx_base, std::uint64_t& total_lines,
    std::uint64_t& total_uc_size, std::uint64_t& tail_line_count,
    std::vector<IndexerCheckpoint>& checkpoints,
    const Indexer::VisitorList& visitors) {
    const bool has_visitors = !visitors.empty();
    std::uint64_t checkpoint_idx = checkpoint_idx_base;
    std::uint64_t global_uc = 0;
    std::uint64_t line_count_in_chunk = 0;
    std::uint64_t first_line_in_chunk = total_lines + 1;

    std::uint64_t total_chunks_received = 0;

    auto process_msg =
        [&](ParallelInflateMsg& msg) -> dftracer::utils::coro::CoroTask<void> {
        const std::size_t data_len = msg.data ? msg.data->size() : 0;
        ++total_chunks_received;

        if (has_visitors && data_len > 0) {
            const char* data = reinterpret_cast<const char*>(msg.data->data());
            for (auto& v : visitors) {
                co_await v.get().on_chunk(data, data_len, checkpoint_idx);
            }
        }

        global_uc += data_len;
        total_lines += msg.lines;
        line_count_in_chunk += msg.lines;

        if (msg.has_checkpoint) {
            IndexerCheckpoint checkpoint{
                .checkpoint_idx = checkpoint_idx++,
                .uc_offset = global_uc,
                .uc_size = 0,
                .c_offset = msg.ckpt_c_offset,
                .c_size = 0,
                .bits = msg.bits,
                .dict_compressed = std::move(msg.dict_compressed),
                .num_lines = line_count_in_chunk,
                .first_line_num = first_line_in_chunk,
                .last_line_num = total_lines,
            };
            checkpoints.push_back(std::move(checkpoint));

            if (has_visitors) {
                for (auto& v : visitors) {
                    co_await v.get().on_checkpoint(checkpoint_idx - 1);
                }
            }

            line_count_in_chunk = 0;
            first_line_in_chunk = total_lines + 1;
        }
        co_return;
    };

    // Per-worker reorder buffer: moodycamel::ConcurrentQueue (backing our
    // coro::Channel) does not guarantee strict FIFO without explicit
    // producer tokens, so we re-sort by msg.seq here. Channel capacity is
    // bounded so the buffer is also bounded (~channel capacity entries).
    auto drain_visitors = [&]() -> dftracer::utils::coro::CoroTask<void> {
        for (auto& v : visitors) {
            if (v.get().wants_drain()) {
                co_await v.get().drain_pending();
            }
        }
    };

    for (auto& chan : chans) {
        std::uint64_t expected_seq = 0;
        std::map<std::uint64_t, ParallelInflateMsg> pending;
        while (auto msg_opt = co_await chan->receive()) {
            auto& incoming = *msg_opt;
            if (incoming.seq == expected_seq) {
                co_await process_msg(incoming);
                co_await drain_visitors();
                ++expected_seq;
                auto it = pending.find(expected_seq);
                while (it != pending.end()) {
                    co_await process_msg(it->second);
                    co_await drain_visitors();
                    pending.erase(it);
                    ++expected_seq;
                    it = pending.find(expected_seq);
                }
            } else {
                pending.emplace(incoming.seq, std::move(incoming));
            }
        }
        while (!pending.empty()) {
            auto it = pending.begin();
            if (it->first != expected_seq) break;
            co_await process_msg(it->second);
            co_await drain_visitors();
            pending.erase(it);
            ++expected_seq;
        }
    }

    if (has_visitors) {
        for (auto& v : visitors) co_await v.get().flush();
    }
    total_uc_size = global_uc;
    tail_line_count = line_count_in_chunk;
    co_return true;
}

static dftracer::utils::coro::CoroTask<bool> process_chunks_parallel(
    CoroScope* scope, int fd, std::uint64_t slice_c_end,
    std::uint64_t file_size, std::vector<GzipMember> members,
    std::uint64_t ckpt_size, std::uint64_t checkpoint_idx_base,
    bool strip_slice_leading_partial, std::uint64_t& total_lines,
    std::uint64_t& total_uc_size, std::uint64_t& tail_line_count,
    std::vector<IndexerCheckpoint>& checkpoints,
    const Indexer::VisitorList& visitors) {
    // Cap worker count at member count and a reasonable default.
    constexpr std::size_t DEFAULT_MAX_WORKERS = 16;
    constexpr std::size_t CHAN_CAP = 4;
    const std::size_t num_workers =
        std::min<std::size_t>(DEFAULT_MAX_WORKERS, members.size());

    std::vector<std::shared_ptr<ParallelChan>> chans;
    chans.reserve(num_workers);
    for (std::size_t i = 0; i < num_workers; ++i) {
        chans.push_back(
            dftracer::utils::coro::make_channel<ParallelInflateMsg>(CHAN_CAP));
    }

    // Partition members contiguously, remainder spread over the first few
    // workers so range counts differ by at most 1.
    std::vector<std::pair<std::size_t, std::size_t>> ranges(num_workers);
    {
        const std::size_t per = members.size() / num_workers;
        const std::size_t rem = members.size() % num_workers;
        std::size_t cursor = 0;
        for (std::size_t w = 0; w < num_workers; ++w) {
            const std::size_t count = per + (w < rem ? 1 : 0);
            ranges[w] = {cursor, cursor + count};
            cursor += count;
        }
    }

    bool dispatcher_ok = true;
    std::shared_ptr<std::vector<GzipMember>> members_shared =
        std::make_shared<std::vector<GzipMember>>(std::move(members));

    co_await scope->scope([&](CoroScope& child)
                              -> dftracer::utils::coro::CoroTask<void> {
        for (std::size_t w = 0; w < num_workers; ++w) {
            const auto [rs, re] = ranges[w];
            const std::uint64_t c_start = (*members_shared)[rs].c_offset;
            const std::uint64_t c_end = (re < members_shared->size())
                                            ? (*members_shared)[re].c_offset
                                            : slice_c_end;
            auto producer = chans[w]->producer();
            // Only the very first worker of a mid-file slice needs
            // to strip the leading partial line; subsequent workers
            // see contiguous (whole-line-aligned) data from their
            // predecessor's stream.
            const bool strip_this = strip_slice_leading_partial && (w == 0);
            // The LAST worker of a NON-LAST slice extends past
            // `slice_c_end` to capture the line that straddles the
            // slice boundary. If this slice's end is file end, the
            // slice IS the last one -- no extension needed.
            const bool extend_this =
                (w + 1 == num_workers) && (slice_c_end < file_size);
            child.spawn([fd, c_start, c_end, ckpt_size, strip_this, extend_this,
                         producer = std::move(producer)](CoroScope&) mutable
                            -> dftracer::utils::coro::CoroTask<void> {
                co_await parallel_worker(fd, c_start, c_end, ckpt_size,
                                         strip_this, extend_this,
                                         std::move(producer));
            });
        }

        child.spawn([&chans, checkpoint_idx_base, &total_lines, &total_uc_size,
                     &tail_line_count, &checkpoints, &visitors, &dispatcher_ok](
                        CoroScope&) -> dftracer::utils::coro::CoroTask<void> {
            dispatcher_ok = co_await parallel_dispatcher(
                chans, checkpoint_idx_base, total_lines, total_uc_size,
                tail_line_count, checkpoints, visitors);
        });

        co_return;
    });

    co_return dispatcher_ok;
}

static dftracer::utils::coro::CoroTask<bool> process_chunks(
    CoroScope* scope, int fd, std::uint64_t ckpt_size,
    const GzipMemberSlice* slice, std::uint64_t& total_lines,
    std::uint64_t& total_uc_size, std::uint64_t& tail_line_count,
    std::vector<IndexerCheckpoint>& checkpoints,
    const Indexer::VisitorList& visitors) {
    // Pre-scanned slice path: caller supplied the member map and a range.
    // Used by the MPI/distributed indexer to split one file across ranks
    // without re-scanning. `checkpoint_idx_base` disambiguates keys so
    // multiple slices of the same file_id produce disjoint SST entries.
    if (scope != nullptr && slice != nullptr && slice->members != nullptr &&
        slice->member_end > slice->member_begin) {
        struct stat st;
        if (::fstat(fd, &st) != 0) co_return false;
        const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);
        const auto& all = *slice->members;
        const std::size_t mb = slice->member_begin;
        const std::size_t me = slice->member_end;
        if (me > all.size() || mb >= me) co_return false;
        // Slice end: next-member offset if this isn't the last slice of
        // the file, else EOF. Crucial: a non-last slice's workers must
        // not inflate past this boundary into another slice's bytes.
        const std::uint64_t slice_c_end =
            (me < all.size()) ? all[me].c_offset : file_size;
        std::vector<GzipMember> sliced(all.begin() + mb, all.begin() + me);
        const bool strip_leading = (mb > 0);
        co_return co_await process_chunks_parallel(
            scope, fd, slice_c_end, file_size, std::move(sliced), ckpt_size,
            slice->checkpoint_idx_base, strip_leading, total_lines,
            total_uc_size, tail_line_count, checkpoints, visitors);
    }

    // Try to discover member boundaries so we can parallelise. The scan is
    // zero-copy, sequential, and fast relative to inflate; for single-member
    // files we fall through to the scan-then-resume path which captures
    // internal deflate-block checkpoints to fan out anyway.
    if (scope != nullptr) {
        struct stat st;
        if (::fstat(fd, &st) == 0 && st.st_size >= 18) {
            std::vector<GzipMember> members;
            const bool scan_ok = co_await enumerate_gzip_member_candidates(
                fd, static_cast<std::uint64_t>(st.st_size), members);
            const std::uint64_t sz = static_cast<std::uint64_t>(st.st_size);
            if (scan_ok && members.size() >= 2) {
                co_return co_await process_chunks_parallel(
                    scope, fd, sz, sz, std::move(members), ckpt_size,
                    /*checkpoint_idx_base=*/0,
                    /*strip_slice_leading_partial=*/false, total_lines,
                    total_uc_size, tail_line_count, checkpoints, visitors);
            }
        }
    }

    co_return co_await process_chunks_serial(fd, ckpt_size, total_lines,
                                             total_uc_size, tail_line_count,
                                             checkpoints, visitors);
}

}  // namespace

dftracer::utils::coro::CoroTask<std::optional<GzipBuildArtifacts>>
build_gzip_index_artifacts(const std::string& gz_path, std::uint64_t ckpt_size,
                           const Indexer::VisitorList& visitors,
                           CoroScope* scope, const GzipMemberSlice* slice) {
    int fd = ::open(gz_path.c_str(), O_RDONLY);
    if (fd < 0) {
        co_return std::nullopt;
    }

    if (!visitors.empty()) {
        const std::uint64_t compressed_bytes = file_size_bytes(gz_path);
        const std::size_t estimated = static_cast<std::size_t>(
            compressed_bytes / (ckpt_size > 0 ? ckpt_size : 1));
        for (auto& visitor : visitors) {
            visitor.get().begin(estimated);
        }
    }

    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;
    std::uint64_t tail_line_count = 0;
    std::vector<IndexerCheckpoint> checkpoints;

    const bool success = co_await process_chunks(
        scope, fd, ckpt_size, slice, total_lines, total_uc_size,
        tail_line_count, checkpoints, visitors);
    ::close(fd);

    if (!success) {
        co_return std::nullopt;
    }

    finalize_checkpoints(checkpoints, total_uc_size, total_lines,
                         tail_line_count);

    GzipBuildArtifacts artifacts;
    artifacts.checkpoint_size = ckpt_size;
    artifacts.total_lines = total_lines;
    artifacts.total_uc_size = total_uc_size;
    artifacts.checkpoints = std::move(checkpoints);
    co_return artifacts;
}

void persist_gzip_index_artifacts(IndexDatabaseWriterContext& db, int file_id,
                                  const GzipBuildArtifacts& artifacts) {
    for (const auto& checkpoint : artifacts.checkpoints) {
        db.insert_checkpoint(file_id, checkpoint);
    }
    db.insert_file_metadata(file_id, artifacts.checkpoint_size,
                            artifacts.total_lines, artifacts.total_uc_size);
}

GzipIndexer::GzipIndexer(const std::string& gz_path_,
                         const std::string& idx_path_, std::uint64_t ckpt_size_,
                         bool force_rebuild_)
    : gz_path(gz_path_),
      gz_path_logical_path(get_logical_path(gz_path_)),
      index_path(normalize_index_root(idx_path_)),
      ckpt_size(ckpt_size_),
      force_rebuild(force_rebuild_),
      cached_is_valid(false),
      cached_file_id(-1),
      cached_max_bytes(0),
      cached_max_bytes_ready(false),
      cached_num_lines(0),
      cached_num_lines_ready(false),
      cached_checkpoint_size(0),
      cached_checkpoint_size_ready(false) {
    if (gz_path.empty()) {
        throw IndexerError(IndexerError::Type::INVALID_ARGUMENT,
                           "gz_path must not be empty");
    }

    if (!fs::exists(gz_path)) {
        throw IndexerError(IndexerError::Type::FILE_ERROR,
                           "gz_path does not exist: " + gz_path);
    }

    if (ckpt_size == 0) {
        throw IndexerError(IndexerError::Type::INVALID_ARGUMENT,
                           "ckpt_size must be greater than 0");
    }

    open();
}

GzipIndexer::~GzipIndexer() {
    DFTRACER_UTILS_LOG_DEBUG("Destroying GZIP indexer for %s", gz_path.c_str());
    close();
}

GzipIndexer::GzipIndexer(GzipIndexer&& other) noexcept
    : gz_path(std::move(other.gz_path)),
      gz_path_logical_path(std::move(other.gz_path_logical_path)),
      index_path(std::move(other.index_path)),
      ckpt_size(other.ckpt_size),
      force_rebuild(other.force_rebuild),
      visitors_(std::move(other.visitors_)),
      cached_is_valid(other.cached_is_valid.load()),
      cached_file_id(other.cached_file_id.load()),
      cached_max_bytes(other.cached_max_bytes.load()),
      cached_max_bytes_ready(other.cached_max_bytes_ready.load()),
      cached_num_lines(other.cached_num_lines.load()),
      cached_num_lines_ready(other.cached_num_lines_ready.load()),
      cached_checkpoint_size(other.cached_checkpoint_size.load()),
      cached_checkpoint_size_ready(other.cached_checkpoint_size_ready.load()),
      cached_checkpoints(std::move(other.cached_checkpoints)) {}

GzipIndexer& GzipIndexer::operator=(GzipIndexer&& other) noexcept {
    if (this != &other) {
        gz_path = std::move(other.gz_path);
        gz_path_logical_path = std::move(other.gz_path_logical_path);
        index_path = std::move(other.index_path);
        ckpt_size = other.ckpt_size;
        force_rebuild = other.force_rebuild;
        visitors_ = std::move(other.visitors_);
        cached_is_valid.store(other.cached_is_valid.load());
        cached_file_id.store(other.cached_file_id.load());
        cached_max_bytes.store(other.cached_max_bytes.load());
        cached_max_bytes_ready.store(other.cached_max_bytes_ready.load());
        cached_num_lines.store(other.cached_num_lines.load());
        cached_num_lines_ready.store(other.cached_num_lines_ready.load());
        cached_checkpoint_size.store(other.cached_checkpoint_size.load());
        cached_checkpoint_size_ready.store(
            other.cached_checkpoint_size_ready.load());
        std::lock_guard<std::mutex> lock(cached_checkpoints_mutex);
        cached_checkpoints = std::move(other.cached_checkpoints);
    }
    return *this;
}

void GzipIndexer::open() {}

void GzipIndexer::close() {}

dftracer::utils::coro::CoroTask<void> GzipIndexer::build_async() const {
    if (!force_rebuild && !need_rebuild()) {
        co_return;
    }

    IndexDatabase db(index_path);
    const std::time_t mtime = get_file_modification_time(gz_path);
    const auto hash = calculate_file_hash(gz_path);
    const std::uint64_t bytes = file_size_bytes(gz_path);
    const std::uint64_t final_ckpt_size =
        determine_checkpoint_size(ckpt_size, gz_path);
    const std::string logical = gz_path_logical_path;
    auto writer = db.begin_write();
    const int file_id = writer->get_or_create_file_info(logical, hash);
    writer->commit();

    auto artifacts = co_await build_gzip_index_artifacts(
        gz_path, final_ckpt_size, visitors_, nullptr);
    if (!artifacts) {
        throw IndexerError(IndexerError::Type::BUILD_ERROR,
                           "Failed to build index for " + gz_path);
    }

    {
        auto w = db.begin_write();
        persist_gzip_index_artifacts(*w, file_id, *artifacts);
        w->commit();
    }

    (void)mtime;
    (void)bytes;

    cached_is_valid = true;
    cached_file_id = file_id;
    cached_checkpoint_size = final_ckpt_size;
    cached_checkpoint_size_ready = true;
    cached_num_lines = db.get_num_lines(file_id);
    cached_num_lines_ready = true;
    cached_max_bytes = db.get_max_bytes(file_id);
    cached_max_bytes_ready = true;
    std::lock_guard<std::mutex> lock(cached_checkpoints_mutex);
    cached_checkpoints = db.query_checkpoints(file_id);
    co_return;
}

bool GzipIndexer::is_valid() const { return cached_is_valid; }

bool GzipIndexer::exists() const {
    return fs::exists(index_path) && fs::is_directory(index_path);
}

bool GzipIndexer::need_rebuild() const {
    if (is_valid()) {
        return false;
    }
    if (!exists()) {
        return true;
    }

    try {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        const auto stored_hash = db.get_file_hash(gz_path_logical_path);
        const int file_id = db.get_file_info_id(gz_path_logical_path);
        if (!stored_hash || file_id < 0) {
            return true;
        }

        const auto current_hash = calculate_file_hash(gz_path);
        const auto current_ckpt_size = db.get_checkpoint_size(file_id);
        return current_hash != *stored_hash || current_ckpt_size == 0;
    } catch (...) {
        return true;
    }
}

const std::string& GzipIndexer::get_index_path() const { return index_path; }

const std::string& GzipIndexer::get_archive_path() const { return gz_path; }

const std::string& GzipIndexer::get_gz_path() const { return gz_path; }

std::uint64_t GzipIndexer::get_max_bytes() const {
    if (!cached_max_bytes_ready.load(std::memory_order_acquire)) {
        const int file_id = get_file_id();
        if (file_id != -1) {
            IndexDatabase db(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
            auto val = db.get_max_bytes(file_id);
            cached_max_bytes.store(val, std::memory_order_relaxed);
            cached_max_bytes_ready.store(true, std::memory_order_release);
        }
    }
    return cached_max_bytes.load(std::memory_order_relaxed);
}

std::uint64_t GzipIndexer::get_checkpoint_size() const {
    if (!cached_checkpoint_size_ready.load(std::memory_order_acquire)) {
        const int file_id = get_file_id();
        if (file_id != -1) {
            IndexDatabase db(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
            auto val = db.get_checkpoint_size(file_id);
            cached_checkpoint_size.store(val, std::memory_order_relaxed);
            cached_checkpoint_size_ready.store(true, std::memory_order_release);
        }
    }
    return cached_checkpoint_size.load(std::memory_order_relaxed);
}

std::uint64_t GzipIndexer::get_num_lines() const {
    if (!cached_num_lines_ready.load(std::memory_order_acquire)) {
        const int file_id = get_file_id();
        if (file_id != -1) {
            IndexDatabase db(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
            auto val = db.get_num_lines(file_id);
            cached_num_lines.store(val, std::memory_order_relaxed);
            cached_num_lines_ready.store(true, std::memory_order_release);
        }
    }
    return cached_num_lines.load(std::memory_order_relaxed);
}

int GzipIndexer::get_file_id() const {
    auto val = cached_file_id.load(std::memory_order_relaxed);
    if (val == -1) {
        IndexDatabase db(
            index_path,
            dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        val = db.get_file_info_id(gz_path_logical_path);
        cached_file_id.store(val, std::memory_order_relaxed);
    }
    return val;
}

int GzipIndexer::find_file_id(const std::string& path) const {
    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    return db.get_file_info_id(get_logical_path(path));
}

bool GzipIndexer::find_checkpoint(std::size_t target_offset,
                                  IndexerCheckpoint& checkpoint) const {
    const int file_id = get_file_id();
    if (file_id == -1) {
        return false;
    }
    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    return db.find_checkpoint(file_id, target_offset, checkpoint);
}

std::vector<IndexerCheckpoint> GzipIndexer::get_checkpoints() const {
    std::lock_guard<std::mutex> lock(cached_checkpoints_mutex);
    if (cached_checkpoints.empty()) {
        const int file_id = get_file_id();
        if (file_id != -1) {
            IndexDatabase db(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
            cached_checkpoints = db.query_checkpoints(file_id);
        }
    }
    return cached_checkpoints;
}

std::vector<IndexerCheckpoint> GzipIndexer::get_checkpoints_for_line_range(
    std::uint64_t start_line, std::uint64_t end_line) const {
    const int file_id = get_file_id();
    if (file_id == -1) {
        return {};
    }
    IndexDatabase db(
        index_path,
        dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
    return db.query_checkpoints_for_line_range(file_id, start_line, end_line);
}

}  // namespace dftracer::utils::utilities::indexer::internal::gzip
