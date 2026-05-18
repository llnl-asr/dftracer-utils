#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_PARALLEL_WRITER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_PARALLEL_WRITER_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/fileio/parallel/layout.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dftracer::utils {
class CoroScope;
}

namespace dftracer::utils::utilities::fileio::parallel {

/// Parallel file writer interface. Concrete impls (striped, sharded) hide the
/// on-disk layout; for gzip output the caller must feed standalone gzip
/// members so chunks stay valid at any offset.
class ParallelWriter {
   public:
    virtual ~ParallelWriter() = default;

    /// Create/truncate backing storage. `scope` may be null for layouts that
    /// don't spawn internal coroutines; padded-striped requires a non-null
    /// scope that outlives close().
    virtual coro::CoroTask<int> open(std::string path, std::size_t num_workers,
                                     bool gzip_extension, CoroScope* scope) = 0;

    /// Prologue, written before any worker chunk.
    virtual coro::CoroTask<int> write_header(ByteView data) = 0;

    /// Striped: placed at an atomic offset. Sharded: appended to shard N.
    virtual coro::CoroTask<int> write_chunk(std::size_t worker_idx,
                                            ByteView data) = 0;

    /// Epilogue, written after all workers drain.
    virtual coro::CoroTask<int> write_footer(ByteView data) = 0;

    virtual coro::CoroTask<int> close() = 0;

    /// One entry for striped; N entries (read order) for sharded.
    virtual std::vector<std::string> output_paths() const = 0;

    /// Per-write_chunk layout entry: byte offset + length of one independently
    /// decompressable gzip member (or raw chunk for non-gzip layouts).
    struct MemberSpan {
        std::uint64_t offset;
        std::uint64_t length;
    };

    /// Member offsets recorded by `write_chunk`, sorted by ascending offset.
    /// Returned span is owned by the writer; valid until destruction.
    /// Must be called after `close()` (no concurrent writes).
    /// Empty for layouts that don't expose member boundaries.
    virtual std::span<const MemberSpan> member_layout() const { return {}; }

    /// Span of the most recent `write_chunk(worker_idx, ...)` call on this
    /// worker. Caller must invoke immediately after `co_await write_chunk()`
    /// returns; subsequent calls overwrite. For sharded layouts the offset
    /// is shard-local; remap with `shard_base_offsets()` after close.
    virtual std::optional<MemberSpan> last_member(
        std::size_t /*worker_idx*/) const {
        return std::nullopt;
    }

    /// Per-worker base offset to add to a shard-local `MemberSpan.offset` to
    /// get the merged-file offset. Empty by default (no remap needed for
    /// single-stream layouts). Call after `close()`.
    virtual std::vector<std::uint64_t> shard_base_offsets() const { return {}; }
};

struct WriterConfig {
    FileLayout layout = FileLayout::STRIPED;
    std::size_t stripe_size = 0;  // PFS stripe; 0 disables padded layout
    bool gzip = false;
};

std::unique_ptr<ParallelWriter> make_writer(const WriterConfig& cfg);
std::unique_ptr<ParallelWriter> make_striped_writer();
std::unique_ptr<ParallelWriter> make_sharded_writer();
std::unique_ptr<ParallelWriter> make_padded_striped_writer(
    std::size_t stripe_size);

}  // namespace dftracer::utils::utilities::fileio::parallel

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_PARALLEL_WRITER_H
