#ifndef DFTRACER_UTILS_READER_STREAMS_GZIP_STREAM_H
#define DFTRACER_UTILS_READER_STREAMS_GZIP_STREAM_H

#include <dftracer/utils/core/common/checkpointer.h>
#include <dftracer/utils/indexer/checkpoint.h>
#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/reader/error.h>
#include <dftracer/utils/reader/inflater.h>
#include <dftracer/utils/reader/streams/stream.h>

#ifdef __linux__
#include <fcntl.h>
#endif

using namespace dftracer::utils;

class GzipStream : public dftracer::utils::StreamBase {
   protected:
    FILE *file_handle_;
    mutable ReaderInflater inflater_;
    std::size_t current_position_;
    std::size_t target_end_bytes_;
    std::size_t max_file_bytes_;
    bool is_active_;
    bool is_finished_;
    bool decompression_initialized_;
    bool use_checkpoint_;

    // Less frequently accessed members
    std::string current_gz_path_;
    std::size_t start_bytes_;
    IndexerCheckpoint checkpoint_;

   public:
    GzipStream()
        : StreamBase(),
          file_handle_(nullptr),
          current_position_(0),
          target_end_bytes_(0),
          max_file_bytes_(0),
          is_active_(false),
          is_finished_(false),
          decompression_initialized_(false),
          use_checkpoint_(false),
          start_bytes_(0) {}

    virtual ~GzipStream() { reset(); }

    bool matches(const std::string &gz_path, std::size_t /*start_bytes*/,
                 std::size_t end_bytes) const {
        // Reuse the stream if same file and same end position.
        // For POSIX-style sequential reads, the stream continues from
        // current_position_ regardless of start_bytes (which is unused).
        return current_gz_path_ == gz_path && target_end_bytes_ == end_bytes;
    }

    bool done() const override { return is_finished_; }

    std::size_t read(char *buffer, std::size_t buffer_size) override = 0;

    void reset() override {
        current_gz_path_.clear();
        start_bytes_ = 0;
        current_position_ = 0;
        target_end_bytes_ = 0;
        max_file_bytes_ = 0;
        is_active_ = false;
        is_finished_ = false;
        if (file_handle_) {
            std::fclose(file_handle_);
            file_handle_ = nullptr;
        }
        inflater_.reset();
        checkpoint_ = IndexerCheckpoint();
        decompression_initialized_ = false;
    }

   protected:
    FILE *open_file(const std::string &path) {
        FILE *file = std::fopen(path.c_str(), "rb");
        if (!file) {
            throw ReaderError(ReaderError::FILE_IO_ERROR,
                              "Failed to open file: " + path);
        }

        // Optimize file I/O with larger buffer
        setvbuf(file, nullptr, _IOFBF, constants::reader::FILE_IO_BUFFER_SIZE);

#ifdef __linux__
        // Hint to kernel about sequential access
        int fd = fileno(file);
        posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

        return file;
    }

    void initialize(const std::string &gz_path, std::size_t start_bytes,
                    std::size_t end_bytes,
                    dftracer::utils::Indexer &indexer) override {
        if (is_active_) {
            reset();
        }
        current_gz_path_ = gz_path;
        start_bytes_ = start_bytes;
        target_end_bytes_ = end_bytes;
        max_file_bytes_ = indexer.get_max_bytes();
        is_active_ = true;
        is_finished_ = false;

        file_handle_ = open_file(gz_path);

        use_checkpoint_ = try_initialize_with_checkpoint(start_bytes, indexer);

        if (!use_checkpoint_) {
            checkpoint_ = IndexerCheckpoint();
            if (!inflater_.initialize(
                    file_handle_, 0,
                    constants::indexer::ZLIB_GZIP_WINDOW_BITS)) {
                throw ReaderError(ReaderError::COMPRESSION_ERROR,
                                  "Failed to initialize inflater");
            }
        }

        decompression_initialized_ = true;
    }

    bool try_initialize_with_checkpoint(std::size_t start_bytes,
                                        dftracer::utils::Indexer &indexer) {
        bool should_use_first_checkpoint =
            start_bytes < indexer.get_checkpoint_size();

        if (should_use_first_checkpoint) {
            if (indexer.find_checkpoint(0, checkpoint_)) {
                if (inflate_init_from_checkpoint()) {
                    DFTRACER_UTILS_LOG_DEBUG(
                        "Using first checkpoint at uncompressed offset %zu for "
                        "early "
                        "target %zu",
                        checkpoint_.uc_offset, start_bytes);
                    return true;
                }
            }
        } else {
            if (indexer.find_checkpoint(start_bytes, checkpoint_)) {
                if (inflate_init_from_checkpoint()) {
                    DFTRACER_UTILS_LOG_DEBUG(
                        "Using checkpoint at uncompressed offset %llu for "
                        "target %zu",
                        checkpoint_.uc_offset, start_bytes);
                    return true;
                }
            }
        }
        return false;
    }

    void skip(std::size_t target_position) {
        std::size_t current_pos = checkpoint_.uc_offset;
        if (target_position > current_pos) {
            inflater_.skip_bytes(file_handle_, target_position - current_pos);
        }
    }

    bool is_at_target_end() const {
        return current_position_ >= target_end_bytes_;
    }

    void restart_compression() {
        inflater_.reset();
        if (use_checkpoint_) {
            if (!inflate_init_from_checkpoint()) {
                throw ReaderError(ReaderError::COMPRESSION_ERROR,
                                  "Failed to reinitialize from checkpoint");
            }
        } else {
            if (!inflater_.initialize(
                    file_handle_, 0,
                    constants::indexer::ZLIB_GZIP_WINDOW_BITS)) {
                throw ReaderError(ReaderError::COMPRESSION_ERROR,
                                  "Failed to initialize inflater");
            }
        }
    }

   private:
    bool inflate_init_from_checkpoint() const {
        return inflater_.restore_from_checkpoint(file_handle_, checkpoint_);
    }
};

#endif  // DFTRACER_UTILS_READER_STREAMS_GZIP_STREAM_H
