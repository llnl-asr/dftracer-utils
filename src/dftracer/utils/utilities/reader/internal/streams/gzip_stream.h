#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_STREAM_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_STREAM_H

#include <dftracer/utils/core/common/checkpointer.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/reader/internal/error.h>
#include <dftracer/utils/utilities/reader/internal/inflater.h>
#include <dftracer/utils/utilities/reader/internal/streams/stream.h>
#include <fcntl.h>
#include <unistd.h>

namespace dftracer::utils::utilities::reader::internal {

class GzipStream : public StreamBase {
   protected:
    int fd_ = -1;
    mutable off_t file_offset_ = 0;
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
    dftracer::utils::utilities::indexer::internal::IndexerCheckpoint
        checkpoint_;

   public:
    GzipStream()
        : StreamBase(),
          fd_(-1),
          file_offset_(0),
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

    coro::CoroTask<std::span<const char>> read_async() override = 0;
    coro::CoroTask<std::size_t> read_async(
        char *buffer, std::size_t buffer_size) override = 0;

    void reset() override {
        current_gz_path_.clear();
        start_bytes_ = 0;
        current_position_ = 0;
        target_end_bytes_ = 0;
        max_file_bytes_ = 0;
        is_active_ = false;
        is_finished_ = false;
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        file_offset_ = 0;
        inflater_.reset();
        checkpoint_ =
            dftracer::utils::utilities::indexer::internal::IndexerCheckpoint();
        decompression_initialized_ = false;
    }

   protected:
    int open_file(const std::string &path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw ReaderError(ReaderError::FILE_IO_ERROR,
                              "Failed to open file: " + path);
        }
#ifdef __linux__
        posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
        return fd;
    }

    void initialize(const std::string &gz_path, std::size_t start_bytes,
                    std::size_t end_bytes,
                    dftracer::utils::utilities::indexer::internal::Indexer
                        &indexer) override {
        if (is_active_) {
            reset();
        }
        current_gz_path_ = gz_path;
        start_bytes_ = start_bytes;
        target_end_bytes_ = end_bytes;
        max_file_bytes_ = indexer.get_max_bytes();
        is_active_ = true;
        is_finished_ = false;

        fd_ = open_file(gz_path);
        file_offset_ = 0;

        use_checkpoint_ = try_initialize_with_checkpoint(start_bytes, indexer);

        if (!use_checkpoint_) {
            checkpoint_ = dftracer::utils::utilities::indexer::internal::
                IndexerCheckpoint();
            if (!inflater_
                     .initialize(fd_, file_offset_, 0,
                                 constants::indexer::ZLIB_GZIP_WINDOW_BITS)
                     .get()) {
                throw ReaderError(ReaderError::COMPRESSION_ERROR,
                                  "Failed to initialize inflater");
            }
        }

        decompression_initialized_ = true;
    }

    bool try_initialize_with_checkpoint(
        std::size_t start_bytes,
        dftracer::utils::utilities::indexer::internal::Indexer &indexer) {
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
            inflater_
                .skip_bytes(fd_, file_offset_, target_position - current_pos)
                .get();
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
            if (!inflater_
                     .initialize(fd_, file_offset_, 0,
                                 constants::indexer::ZLIB_GZIP_WINDOW_BITS)
                     .get()) {
                throw ReaderError(ReaderError::COMPRESSION_ERROR,
                                  "Failed to initialize inflater");
            }
        }
    }

   private:
    bool inflate_init_from_checkpoint() const {
        return inflater_.restore_from_checkpoint(fd_, file_offset_, checkpoint_)
            .get();
    }
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_STREAM_H
