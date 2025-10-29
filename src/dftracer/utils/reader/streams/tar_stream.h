#ifndef DFTRACER_UTILS_READER_STREAMS_TAR_STREAM_H
#define DFTRACER_UTILS_READER_STREAMS_TAR_STREAM_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/indexer/tar_indexer.h>
#include <dftracer/utils/reader/streams/gzip_stream.h>
#include <dftracer/utils/reader/tar_reader.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * TAR-aware base stream class that provides logical content mapping
 * for TAR.GZ files, presenting concatenated file contents without TAR headers.
 */
class TarStream : public GzipStream {
   protected:
    // TAR-specific state
    std::unique_ptr<TarIndexer> tar_indexer_;
    std::vector<TarReader::TarFileInfo> file_mapping_;
    std::size_t current_file_index_;
    std::size_t current_file_offset_;
    bool logical_mapping_initialized_;

    // Current file being read
    const TarReader::TarFileInfo* current_file_;
    std::unique_ptr<dftracer::utils::ReaderStream> underlying_stream_;

   public:
    TarStream()
        : GzipStream(),
          current_file_index_(0),
          current_file_offset_(0),
          logical_mapping_initialized_(false),
          current_file_(nullptr) {}

    virtual ~TarStream() = default;

    void initialize(const std::string& tar_gz_path, std::size_t start_bytes,
                    std::size_t end_bytes,
                    dftracer::utils::Indexer& indexer) override {
        DFTRACER_UTILS_LOG_DEBUG(
            "TarStream::initialize - start_bytes=%zu, end_bytes=%zu",
            start_bytes, end_bytes);

        // Cast to TarIndexer for TAR-specific operations
        auto* tar_idx = dynamic_cast<TarIndexer*>(&indexer);
        if (!tar_idx) {
            throw ReaderError(ReaderError::INITIALIZATION_ERROR,
                              "TarStream requires a TarIndexer");
        }

        GzipStream::initialize(tar_gz_path, start_bytes, end_bytes, indexer);

        // Build logical file mapping
        build_logical_mapping(*tar_idx);

        // Position at the start of the logical range
        seek_to_logical_position(start_bytes);

        DFTRACER_UTILS_LOG_DEBUG(
            "TarStream::initialize - completed, files mapped: %zu",
            file_mapping_.size());
    }

    void reset() override {
        GzipStream::reset();
        file_mapping_.clear();
        current_file_index_ = 0;
        current_file_offset_ = 0;
        logical_mapping_initialized_ = false;
        current_file_ = nullptr;
        underlying_stream_.reset();
    }

   protected:
    void build_logical_mapping(TarIndexer& tar_indexer) {
        if (logical_mapping_initialized_) {
            return;
        }

        DFTRACER_UTILS_LOG_DEBUG("%s", "Building TAR logical mapping");

        auto tar_files = tar_indexer.list_files();
        file_mapping_.clear();
        file_mapping_.reserve(tar_files.size());

        std::uint64_t logical_offset = 0;
        for (const auto& tar_file : tar_files) {
            TarReader::TarFileInfo mapped_file;
            mapped_file.file_name = tar_file.file_name;
            mapped_file.file_size = tar_file.file_size;
            mapped_file.file_mtime = tar_file.file_mtime;
            mapped_file.typeflag = tar_file.typeflag;
            mapped_file.logical_start_offset = logical_offset;
            mapped_file.logical_end_offset =
                logical_offset + tar_file.file_size;

            file_mapping_.push_back(mapped_file);
            logical_offset += tar_file.file_size;

            DFTRACER_UTILS_LOG_DEBUG(
                "TAR file mapped: %s [%lu-%lu] size=%lu",
                mapped_file.file_name.c_str(), mapped_file.logical_start_offset,
                mapped_file.logical_end_offset, mapped_file.file_size);
        }

        logical_mapping_initialized_ = true;
        DFTRACER_UTILS_LOG_DEBUG("TAR logical mapping complete: %zu files",
                                 file_mapping_.size());
    }

    void seek_to_logical_position(std::size_t logical_pos) {
        // Find the file that contains this logical position
        current_file_ = nullptr;
        current_file_index_ = 0;
        current_file_offset_ = 0;

        for (std::size_t i = 0; i < file_mapping_.size(); ++i) {
            const auto& file = file_mapping_[i];
            if (logical_pos >= file.logical_start_offset &&
                logical_pos < file.logical_end_offset) {
                current_file_ = &file;
                current_file_index_ = i;
                current_file_offset_ = logical_pos - file.logical_start_offset;

                DFTRACER_UTILS_LOG_DEBUG(
                    "Positioned in file %s at offset %zu (logical pos %zu)",
                    file.file_name.c_str(), current_file_offset_, logical_pos);
                break;
            }
        }

        if (!current_file_) {
            // Position is beyond all files
            current_file_index_ = file_mapping_.size();
            current_file_offset_ = 0;
            DFTRACER_UTILS_LOG_DEBUG(
                "Positioned beyond all files (logical pos %zu)", logical_pos);
        }
    }

    bool advance_to_next_file() {
        if (current_file_index_ + 1 >= file_mapping_.size()) {
            current_file_ = nullptr;
            return false;
        }

        current_file_index_++;
        current_file_ = &file_mapping_[current_file_index_];
        current_file_offset_ = 0;

        DFTRACER_UTILS_LOG_DEBUG("Advanced to file %s",
                                 current_file_->file_name.c_str());
        return true;
    }

    // Get the actual file data offset for the current TAR file
    std::uint64_t get_current_file_data_offset() const {
        if (!current_file_) {
            return 0;
        }

        // We need to get the actual data_offset from the TAR indexer
        // For now, we'll use the TarIndexer to look it up
        // In a more optimized implementation, we could cache these offsets
        return 0;  // This will be filled by subclasses that have access to the
                   // indexer
    }
};

#endif  // DFTRACER_UTILS_READER_STREAMS_TAR_STREAM_H
