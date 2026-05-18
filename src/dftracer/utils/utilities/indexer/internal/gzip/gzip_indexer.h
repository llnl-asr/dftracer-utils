#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_GZIP_GZIP_INDEXER_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_GZIP_GZIP_INDEXER_H

#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/index_visitor.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>
#include <dftracer/utils/utilities/indexer/internal/common/gzip_member_scanner.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::indexer::internal::gzip {

struct GzipBuildArtifacts {
    std::uint64_t checkpoint_size = 0;
    std::uint64_t total_lines = 0;
    std::uint64_t total_uc_size = 0;
    std::vector<IndexerCheckpoint> checkpoints;
};

/// Optional slice of a multi-member gzip file. When set, the indexer
/// processes only members `[member_begin, member_end)` of the file
/// (byte range `[members[member_begin].c_offset, members[member_end-1]
/// .c_offset + members[member_end-1].c_size)`). Used for cross-rank
/// splitting of large files; uc_offsets/line numbers in emitted
/// checkpoints are slice-local and `checkpoint_idx` is offset by
/// `checkpoint_idx_base` so multiple ranks writing the same file_id
/// produce disjoint keys.
struct GzipMemberSlice {
    const std::vector<internal::GzipMember> *members = nullptr;
    std::size_t member_begin = 0;
    std::size_t member_end = 0;  // exclusive
    std::uint64_t checkpoint_idx_base = 0;
};

/// Build gzip index artifacts (checkpoints, dispatched visitor events).
///
/// When `scope` is non-null and the input is multi-member gzip (the
/// dftracer runtime format), the inflate pass is parallelised across the
/// scope's executor. On single-member files or when `scope` is null,
/// falls back to the serial inflate loop with identical semantics.
///
/// When `slice` is non-null, only the specified member range is
/// processed. The caller is responsible for ensuring `slice->members`
/// outlives this coroutine.
coro::CoroTask<std::optional<GzipBuildArtifacts>> build_gzip_index_artifacts(
    const std::string &gz_path, std::uint64_t ckpt_size,
    const Indexer::VisitorList &visitors, CoroScope *scope = nullptr,
    const GzipMemberSlice *slice = nullptr);

void persist_gzip_index_artifacts(IndexDatabaseWriterContext &db, int file_id,
                                  const GzipBuildArtifacts &artifacts);

class GzipIndexer : public Indexer {
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
    bool find_checkpoint(std::size_t target_offset,
                         IndexerCheckpoint &checkpoint) const override;
    std::vector<IndexerCheckpoint> get_checkpoints() const override;
    std::vector<IndexerCheckpoint> get_checkpoints_for_line_range(
        std::uint64_t start_line, std::uint64_t end_line) const override;

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
    mutable std::vector<IndexerCheckpoint> cached_checkpoints;
    mutable std::mutex cached_checkpoints_mutex;

    // Internal methods
    void open();
    void close();
    bool is_valid() const;
};

}  // namespace dftracer::utils::utilities::indexer::internal::gzip

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_GZIP_GZIP_INDEXER_H
