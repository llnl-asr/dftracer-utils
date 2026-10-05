#ifndef DFTRACER_UTILS_INDEX_GZIP_GZIP_INDEXER_H
#define DFTRACER_UTILS_INDEX_GZIP_GZIP_INDEXER_H

#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/index_visitor.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/gzip/gzip_member_scanner.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::index::gzip {

struct GzipBuildArtifacts {
    std::uint64_t checkpoint_size = 0;
    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;
    std::vector<GzipMemberRecord> members;
    /// The 32 KiB inflate window of each RESTART piece, keyed by member_idx.
    std::vector<std::pair<std::uint64_t, std::string>> restart_windows;
    /// The last member was cut short; `recovered_bytes` of it were kept.
    bool truncated = false;
    std::uint64_t recovered_bytes = 0;
};

/// Optional slice of a multi-member gzip file. When set, the indexer
/// processes only members `[member_begin, member_end)` of the file
/// (byte range `[members[member_begin].c_offset, members[member_end-1]
/// .c_offset + members[member_end-1].c_size)`). Used for cross-rank
/// splitting of large files; uc_offsets/line numbers in emitted
/// members are slice-local and member indices start at `member_begin`
/// so multiple ranks writing the same file_id produce disjoint keys.
struct GzipMemberSlice {
    const std::vector<index::gzip::GzipMember> *members = nullptr;
    std::size_t member_begin = 0;
    std::size_t member_end = 0;  // exclusive
};

/// Build gzip index artifacts (member table, dispatched visitor events).
///
/// The inflate pass is parallelised across `scope`'s executor, one task per
/// gzip member (single-member input runs as a single task).
///
/// When `slice` is non-null, only the specified member range is
/// processed. The caller is responsible for ensuring `slice->members`
/// outlives this coroutine.
coro::CoroTask<std::optional<GzipBuildArtifacts>> build_gzip_index_artifacts(
    const std::string &gz_path, std::uint64_t ckpt_size,
    const CheckpointIndexer::VisitorList &visitors, CoroScope *scope = nullptr,
    const GzipMemberSlice *slice = nullptr);

/// Replace the file's `core.members` data (members and metadata) and write
/// its manifest entry into `w`.
void persist_gzip_index_artifacts(index::store::IndexWrite &w, int file_id,
                                  const GzipBuildArtifacts &artifacts);

class GzipIndexer : public CheckpointIndexer {
   public:
    static constexpr std::uint64_t DEFAULT_CHECKPOINT_SIZE =
        constants::indexer::DEFAULT_CHECKPOINT_SIZE;

    GzipIndexer(const std::string &gz_path, const std::string &index_path,
                std::uint64_t checkpoint_size = DEFAULT_CHECKPOINT_SIZE,
                bool force = false);
    ~GzipIndexer();
    GzipIndexer(const GzipIndexer &) = delete;
    GzipIndexer &operator=(const GzipIndexer &) = delete;
    GzipIndexer(GzipIndexer &&other) noexcept;
    GzipIndexer &operator=(GzipIndexer &&other) noexcept;

    dftracer::utils::coro::CoroTask<void> build_async() const override;
    bool need_rebuild() const override;
    bool exists() const override;

    void set_visitors(VisitorList visitors) override {
        visitors_ = std::move(visitors);
    }

    // Metadata - BaseIndexer interface implementation
    const std::string &get_index_path() const override;
    const std::string &get_archive_path() const override;
    const std::string &get_gz_path() const;
    std::uint64_t get_checkpoint_size() const override;
    std::uint64_t get_max_bytes() const override;
    std::uint64_t get_num_lines() const override;
    int get_file_id() const;

    // Lookup
    int find_file_id(const std::string &gz_path) const;
    bool find_member(std::size_t target_offset,
                     GzipMemberRecord &member) const override;
    std::vector<GzipMemberRecord> get_members() const override;
    std::string restart_window(std::uint64_t member_idx) const override;

    inline ArchiveFormat get_format_type() const override {
        return ArchiveFormat::GZIP;
    }
    inline const char *get_format_name() const override {
        return dftracer::utils::get_format_name(get_format_type());
    }

   private:
    std::string gz_path;
    std::string gz_path_logical_path;
    std::string index_path;
    std::uint64_t ckpt_size;
    bool force_rebuild;
    VisitorList visitors_;

    // Cached values (atomic for thread-safe lazy initialization)
    mutable std::atomic<bool> cached_is_valid{false};
    mutable std::atomic<int> cached_file_id{-1};
    mutable std::atomic<std::uint64_t> cached_max_bytes{0};
    mutable std::atomic<bool> cached_max_bytes_ready{false};
    mutable std::atomic<std::uint64_t> cached_num_lines{0};
    mutable std::atomic<bool> cached_num_lines_ready{false};
    mutable std::atomic<std::uint64_t> cached_checkpoint_size{0};
    mutable std::atomic<bool> cached_checkpoint_size_ready{false};
    mutable std::vector<GzipMemberRecord> cached_members;
    mutable std::mutex cached_members_mutex;
    mutable std::atomic<bool> cached_loaded{false};

    // Internal methods
    void open();
    void close();
    bool is_valid() const;
    // Open the index once and populate every read-path cache (file id, line
    // count, byte count, checkpoint size, members). The per-getter opens it
    // replaces were the dominant cost of streaming a file.
    void ensure_loaded() const;
};

}  // namespace dftracer::utils::index::gzip

#endif  // DFTRACER_UTILS_INDEX_GZIP_GZIP_INDEXER_H
