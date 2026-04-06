#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_GZIP_GZIP_INDEXER_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_GZIP_GZIP_INDEXER_H

#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_visitor.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::indexer::internal::gzip {

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
