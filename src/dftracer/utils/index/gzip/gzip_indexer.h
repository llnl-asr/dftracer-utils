// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_GZIP_GZIP_INDEXER_H
#define DFTRACER_UTILS_INDEX_GZIP_GZIP_INDEXER_H

#include <dftracer/utils/common/archive_format.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/index_visitor.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/gzip/gzip_member_scanner.h>
#include <dftracer/utils/index/index.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::index {
class FileBuild;
}  // namespace dftracer::utils::index

namespace dftracer::utils::index::gzip {

struct GzipBuildArtifacts {
    std::uint64_t checkpoint_size = 0;
    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;
    dstd::Vector<GzipMemberRecord> members;
    /// The 32 KiB inflate window of each RESTART piece, keyed by member_idx.
    dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>> restart_windows;
    /// The last member was cut short; `recovered_bytes` of it were kept.
    bool truncated = false;
    std::uint64_t recovered_bytes = 0;
    /// The last member ended inside a line, so the next bytes finish it.
    bool has_open_line = false;
    /// A chunk indexed before these artifacts starts inside a line.
    bool has_mid_line_chunk = false;
};

/// Optional slice of a multi-member gzip file. When set, the indexer
/// processes only members `[member_begin, member_end)` of the file
/// (byte range `[members[member_begin].c_offset, members[member_end-1]
/// .c_offset + members[member_end-1].c_size)`). Used for cross-rank
/// splitting of large files; uc_offsets/line numbers in emitted
/// members are slice-local and member indices start at `member_begin`
/// so multiple ranks writing the same file_id produce disjoint keys.
struct GzipMemberSlice {
    const dstd::Vector<index::gzip::GzipMember> *members = nullptr;
    std::size_t member_begin = 0;
    std::size_t member_end = 0;  // exclusive
    /// Added to each member index, for members that follow ones already
    /// indexed.
    std::uint64_t idx_base = 0;
    /// When set, the end of the last member. Nothing past it is read, and the
    /// range is treated as whole so nothing past it is recovered.
    std::uint64_t c_end = 0;
};

/// Build gzip index artifacts (member table, dispatched visitor events).
///
/// The inflate pass runs on `scope`'s executor in up to `ways` workers (0:
/// the available parallelism, at most 16), each on a run of members of at
/// least `ckpt_size / 4` uncompressed bytes. At most `ways + 2` runs are
/// inflated ahead of the visitors, so a file holds about `2 * ways + 2` runs
/// of plaintext. A caller that indexes several files at once divides its
/// cores between them with `ways`.
///
/// When `slice` is non-null, only the specified member range is
/// processed. The caller is responsible for ensuring `slice->members`
/// outlives this coroutine.

/// An InvalidArgument error when the file is not gzip, a Build error when the
/// pass fails, or the error a visitor returned.
coro::CoroTask<dstd::Result<GzipBuildArtifacts>> build_gzip_index_artifacts(
    dstd::CStringView gz_path, std::uint64_t ckpt_size,
    const CheckpointIndexer::VisitorList &visitors,
    coro::Scope *scope = nullptr, const GzipMemberSlice *slice = nullptr,
    std::size_t ways = 0);

/// The same over an open file. `fd` stays open and owned by the caller.
/// `gz_path` only names the file in messages.
coro::CoroTask<dstd::Result<GzipBuildArtifacts>> build_gzip_index_artifacts(
    int fd, dstd::CStringView gz_path, std::uint64_t ckpt_size,
    const CheckpointIndexer::VisitorList &visitors,
    coro::Scope *scope = nullptr, const GzipMemberSlice *slice = nullptr,
    std::size_t ways = 0);

/// Write the file's members, restart windows and metadata into `build` and
/// finish its Members extension.
void persist_gzip_index_artifacts(index::FileBuild &build,
                                  const GzipBuildArtifacts &artifacts);

class GzipIndexer : public CheckpointIndexer {
   public:
    static constexpr std::uint64_t DEFAULT_CHECKPOINT_SIZE =
        common::constants::indexer::DEFAULT_CHECKPOINT_SIZE;

    /// Checks what the constructor requires: a non-empty existing `gz_path` and
    /// a non-zero `ckpt_size`. The constructor ends the process on arguments
    /// that fail it.
    static dstd::Result<void> check_arguments(const dstd::String &gz_path,
                                              std::uint64_t ckpt_size);

    GzipIndexer(const dstd::String &gz_path, const dstd::String &index_path,
                std::uint64_t checkpoint_size = DEFAULT_CHECKPOINT_SIZE,
                bool force = false);
    ~GzipIndexer();
    GzipIndexer(const GzipIndexer &) = delete;
    GzipIndexer &operator=(const GzipIndexer &) = delete;
    GzipIndexer(GzipIndexer &&other) noexcept;
    GzipIndexer &operator=(GzipIndexer &&other) noexcept;

    dftracer::utils::coro::CoroTask<dstd::Result<void>> try_build_async()
        const override;
    dstd::Result<void> load() const override;
    bool need_rebuild() const override;
    bool exists() const override;

    void set_visitors(VisitorList visitors) override {
        visitors_ = std::move(visitors);
    }

    // Metadata - BaseIndexer interface implementation
    const dstd::String &index_path() const override;
    const dstd::String &archive_path() const override;
    const dstd::String &gz_path() const;
    std::uint64_t checkpoint_size() const override;
    std::uint64_t max_bytes() const override;
    std::uint64_t line_count() const override;

    // Lookup
    bool find_member(std::size_t target_offset,
                     GzipMemberRecord &member) const override;
    dstd::Vector<GzipMemberRecord> members() const override;
    dstd::Span<const std::uint64_t> piece_heads() const override;
    std::uint64_t compressed_end() const override;
    dstd::String restart_window(std::uint64_t member_idx) const override;

    inline common::ArchiveFormat format_type() const override {
        return common::ArchiveFormat::Gzip;
    }
    inline const char *format_name() const override {
        return dftracer::utils::common::get_format_name(format_type());
    }

   private:
    dstd::String gz_path_;
    dstd::String gz_path_logical_path_;
    dstd::String index_path_;
    std::uint64_t ckpt_size_;
    bool force_rebuild_;
    VisitorList visitors_;

    // Cached values (atomic for thread-safe lazy initialization)
    mutable std::atomic<bool> cached_is_valid_{false};
    mutable std::atomic<std::uint64_t> cached_max_bytes_{0};
    mutable std::atomic<bool> cached_max_bytes_ready_{false};
    mutable std::atomic<std::uint64_t> cached_line_count_{0};
    mutable std::atomic<bool> cached_line_count_ready_{false};
    mutable std::atomic<std::uint64_t> cached_checkpoint_size_{0};
    mutable std::atomic<bool> cached_checkpoint_size_ready_{false};
    mutable dstd::Vector<GzipMemberRecord> cached_members_;
    mutable dstd::Vector<std::uint64_t> cached_heads_;
    // One snapshot per loaded indexer, so a restart read opens nothing.
    mutable dstd::Optional<index::Snapshot> snapshot_;
    mutable dstd::Optional<index::FileRef> file_ref_;
    mutable std::mutex cached_members_mutex_;
    mutable std::atomic<bool> cached_loaded_{false};

    // Internal methods
    void open();
    void close();
    bool is_valid() const;
    // Open the index once and populate every read-path cache (file id, line
    // count, byte count, checkpoint size, members). The per-getter opens it
    // replaces were the dominant cost of streaming a file.
    // load(), ending the process when it fails; see CheckpointIndexer::load.
    void ensure_loaded() const;
    void cache_heads() const;
};

}  // namespace dftracer::utils::index::gzip

#endif  // DFTRACER_UTILS_INDEX_GZIP_GZIP_INDEXER_H
