#ifndef DFTRACER_UTILS_READER_TAR_READER_H
#define DFTRACER_UTILS_READER_TAR_READER_H

#include <dftracer/utils/indexer/tar_indexer.h>
#include <dftracer/utils/reader/reader.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils {

constexpr std::size_t DEFAULT_TAR_READER_BUFFER_SIZE = 1024 * 1024;

/**
 * TAR.GZ specialized reader that provides both:
 * 1. Archive-level access: Concatenated view of all file contents (without TAR
 * headers)
 * 2. File-level access: Individual file reading within the archive
 */
class TarReader : public Reader {
   public:
    /**
     * TAR file information structure
     */
    struct TarFileInfo {
        std::string file_name;
        std::uint64_t file_size;
        std::uint64_t file_mtime;
        char typeflag;
        std::uint64_t
            logical_start_offset;  // Offset in concatenated logical view
        std::uint64_t
            logical_end_offset;    // End offset in concatenated logical view
        std::uint64_t
            logical_start_line;    // Line number in concatenated logical view
        std::uint64_t
            logical_end_line;  // End line number in concatenated logical view
        std::uint64_t
            estimated_lines;   // Estimated number of lines in this file
    };

    TarReader(
        const std::string &tar_gz_path, const std::string &idx_path,
        std::size_t index_ckpt_size = TarIndexer::DEFAULT_CHECKPOINT_SIZE);
    explicit TarReader(std::shared_ptr<TarIndexer> indexer);
    ~TarReader();

    // Disable copy constructor and copy assignment
    TarReader(const TarReader &) = delete;
    TarReader &operator=(const TarReader &) = delete;
    TarReader(TarReader &&other) noexcept;
    TarReader &operator=(TarReader &&other) noexcept;

    // BaseReader interface - operates on concatenated logical view (all file
    // contents)
    std::size_t get_max_bytes() const override;
    std::size_t get_num_lines() const override;
    const std::string &get_archive_path() const override;
    const std::string &get_idx_path() const override;
    void set_buffer_size(std::size_t size) override;

    std::size_t read(std::size_t start_bytes, std::size_t end_bytes,
                     char *buffer, std::size_t buffer_size) override;
    std::size_t read_line_bytes(std::size_t start_bytes, std::size_t end_bytes,
                                char *buffer, std::size_t buffer_size) override;

    std::string read_lines(std::size_t start_line,
                           std::size_t end_line) override;
    void read_lines_with_processor(std::size_t start_line, std::size_t end_line,
                                   LineProcessor &processor) override;
    void read_line_bytes_with_processor(std::size_t start_bytes,
                                        std::size_t end_bytes,
                                        LineProcessor &processor) override;

    std::unique_ptr<ReaderStream> stream(StreamType stream_type,
                                         RangeType range_type,
                                         std::size_t start,
                                         std::size_t end) override;

    void reset() override;
    bool is_valid() const override;
    std::string get_format_name() const override;

    // TAR-specific functionality
    std::vector<TarFileInfo> list_files() const;
    std::size_t get_num_files() const;

    // File-specific reading (by filename)
    std::string read_file(const std::string &filename,
                          std::size_t start_line = 1,
                          std::size_t end_line = SIZE_MAX);
    std::size_t read_file_bytes(const std::string &filename,
                                std::size_t start_bytes, std::size_t end_bytes,
                                char *buffer, std::size_t buffer_size);
    void read_file_with_processor(const std::string &filename,
                                  std::size_t start_line, std::size_t end_line,
                                  LineProcessor &processor);

    // Get file information
    bool find_file(const std::string &filename, TarFileInfo &file_info) const;

   private:
    std::string tar_gz_path;
    std::string idx_path;
    bool is_open;
    std::size_t default_buffer_size;
    std::shared_ptr<TarIndexer> indexer;

    // Cached logical view mapping
    mutable bool logical_mapping_cached;
    mutable std::vector<TarFileInfo> cached_file_mapping;
    mutable std::uint64_t cached_total_logical_bytes;
    mutable std::uint64_t cached_total_logical_lines;

    // Internal helper methods
    void build_logical_mapping() const;
    std::size_t read_logical(std::size_t start_bytes, std::size_t end_bytes,
                             char *buffer, std::size_t buffer_size) const;

    // Helper methods for content extraction
    std::string read_file_content(const TarFileInfo &file_info,
                                  std::size_t offset, std::size_t size) const;
    std::string extract_lines_from_content(const std::string &content,
                                           std::size_t start_line,
                                           std::size_t end_line) const;
    void process_content_lines(const std::string &content,
                               LineProcessor &processor, std::size_t base_line,
                               std::size_t start_line,
                               std::size_t end_line) const;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_READER_TAR_READER_H