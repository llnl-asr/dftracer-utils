#ifndef DFTRACER_UTILS_INDEXER_TAR_INDEXER_H
#define DFTRACER_UTILS_INDEXER_TAR_INDEXER_H

#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/indexer/checkpoint.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/indexer/sqlite/database.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::tar_indexer {

class TarIndexer : public dftracer::utils::Indexer {
   public:
    static constexpr std::uint64_t DEFAULT_CHECKPOINT_SIZE =
        constants::indexer::DEFAULT_CHECKPOINT_SIZE;

    TarIndexer(const std::string &tar_gz_path, const std::string &idx_path,
               std::uint64_t checkpoint_size = DEFAULT_CHECKPOINT_SIZE,
               bool force = false);
    ~TarIndexer();
    TarIndexer(const TarIndexer &) = delete;
    TarIndexer &operator=(const TarIndexer &) = delete;
    TarIndexer(TarIndexer &&other) noexcept;
    TarIndexer &operator=(TarIndexer &&other) noexcept;

    void build() const override;
    bool need_rebuild() const override;
    bool exists() const override;

    const std::string &get_idx_path() const override;
    const std::string &get_archive_path() const override;
    const std::string &get_tar_gz_path() const;
    std::uint64_t get_checkpoint_size() const override;
    std::uint64_t get_max_bytes() const override;
    std::uint64_t get_num_lines() const override;
    int get_archive_id() const;

    // TAR-specific metadata
    std::uint64_t get_num_files() const;
    std::string get_archive_name() const;

    // Lookup
    int find_archive_id(const std::string &tar_gz_path) const;
    bool find_checkpoint(std::size_t target_offset,
                         IndexerCheckpoint &checkpoint) const override;
    std::vector<IndexerCheckpoint> get_checkpoints() const override;
    std::vector<IndexerCheckpoint> get_checkpoints_for_line_range(
        std::uint64_t start_line, std::uint64_t end_line) const override;

    // TAR-specific file lookup
    struct TarFileInfo {
        std::string file_name;
        std::uint64_t file_size;
        std::uint64_t file_mtime;
        char typeflag;
        std::uint64_t data_offset;
        std::uint64_t uncompressed_offset;
    };

    std::vector<TarFileInfo> list_files() const;
    bool find_file(const std::string &file_name, TarFileInfo &file_info) const;
    std::vector<TarFileInfo> find_files_in_range(
        std::uint64_t start_offset, std::uint64_t end_offset) const;

    inline ArchiveFormat get_format_type() const override {
        return ArchiveFormat::TAR_GZ;
    }
    const char *get_format_name() const override {
        return ::get_format_name(get_format_type());
    }

   private:
    std::string tar_gz_path;
    std::string tar_gz_path_logical_path;
    std::string idx_path;
    std::uint64_t ckpt_size;
    bool force_rebuild;
    SqliteDatabase db;

    // Cached values
    mutable bool cached_is_valid;
    mutable int cached_archive_id;
    mutable std::uint64_t cached_max_bytes;
    mutable std::uint64_t cached_num_lines;
    mutable std::uint64_t cached_num_files;
    mutable std::uint64_t cached_checkpoint_size;
    mutable std::string cached_archive_name;
    mutable std::vector<IndexerCheckpoint> cached_checkpoints;

    // Internal methods
    void open();
    void close();
    bool is_valid() const;
};

}  // namespace dftracer::utils::tar_indexer

#endif  // DFTRACER_UTILS_INDEXER_TAR_INDEXER_H
