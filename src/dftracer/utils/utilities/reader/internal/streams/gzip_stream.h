#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_STREAM_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_STREAM_H

#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/utilities/reader/error.h>
#include <dftracer/utils/utilities/reader/internal/inflater.h>
#include <dftracer/utils/utilities/reader/internal/streams/stream.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <vector>

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
    bool use_member_;

    // Less frequently accessed members
    std::string current_gz_path_;
    dftracer::utils::index::gzip::CheckpointIndexer *indexer_ = nullptr;
    std::size_t start_bytes_;
    dftracer::utils::index::gzip::GzipMemberRecord member_;

    // Backing buffer and copy-based read cursor, shared by the copy-drain
    // read_async(char*, size_t) below. Derived classes fill buffer_ via their
    // zero-copy read_async() override.
    static constexpr std::size_t DEFAULT_BUFFER_SIZE = 64 * 1024;  // 64KB
    std::vector<char> buffer_;
    std::size_t valid_bytes_;
    std::size_t buffer_pos_;  // Current position in buffer for copy-based reads

   public:
    explicit GzipStream(std::size_t buffer_size = DEFAULT_BUFFER_SIZE)
        : StreamBase(),
          fd_(-1),
          file_offset_(0),
          current_position_(0),
          target_end_bytes_(0),
          max_file_bytes_(0),
          is_active_(false),
          is_finished_(false),
          decompression_initialized_(false),
          use_member_(false),
          start_bytes_(0),
          buffer_(buffer_size, 0),
          valid_bytes_(0),
          buffer_pos_(0) {}

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

    // Copy-drain adapter over the zero-copy read_async(): serves the caller's
    // buffer from buffer_, refilling via the derived read_async() when empty.
    coro::CoroTask<std::size_t> read_async(char *buffer,
                                           std::size_t buffer_size) override {
#ifdef __GNUC__
        __builtin_prefetch(buffer, 1, 3);
#endif

        // Check if we have unconsumed data from previous read
        if (buffer_pos_ < valid_bytes_) {
            std::size_t remaining = valid_bytes_ - buffer_pos_;
            std::size_t copy_size = std::min(remaining, buffer_size);
            std::memcpy(buffer, buffer_.data() + buffer_pos_, copy_size);
            buffer_pos_ += copy_size;

            DFTRACER_UTILS_LOG_DEBUG(
                "Copied %zu bytes from existing buffer (pos %zu/%zu)",
                copy_size, buffer_pos_, valid_bytes_);

            co_return copy_size;
        }

        // Buffer exhausted, get new chunk via zero-copy read
        auto span = co_await read_async();
        if (span.empty()) {
            co_return 0;
        }

        // Update our tracking of the buffer state
        valid_bytes_ = span.size();
        buffer_pos_ = 0;

        std::size_t copy_size = std::min(valid_bytes_, buffer_size);
        std::memcpy(buffer, span.data(), copy_size);
        buffer_pos_ = copy_size;

        DFTRACER_UTILS_LOG_DEBUG(
            "Got new chunk via zero-copy, copied %zu bytes (total in buffer: "
            "%zu)",
            copy_size, valid_bytes_);

        co_return copy_size;
    }

    void reset() override {
        current_gz_path_.clear();
        indexer_ = nullptr;
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
        member_ = dftracer::utils::index::gzip::GzipMemberRecord();
        use_member_ = false;
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

    /// Records the request only. Opening and seeking happen on the first
    /// read, so a stream is built without blocking a caller who is going to
    /// await the data anyway.
    void initialize(
        const std::string &gz_path, std::size_t start_bytes,
        std::size_t end_bytes,
        dftracer::utils::index::gzip::CheckpointIndexer &indexer) override {
        if (is_active_) {
            reset();
        }
        current_gz_path_ = gz_path;
        start_bytes_ = start_bytes;
        target_end_bytes_ = end_bytes;
        indexer_ = &indexer;
        max_file_bytes_ = indexer.get_max_bytes();
        is_active_ = true;
        is_finished_ = false;
    }

    coro::CoroTask<void> ensure_initialized() {
        if (decompression_initialized_ || !is_active_) co_return;

        fd_ = open_file(current_gz_path_);
        file_offset_ = 0;

        // Share member decodes across concurrent readers when a process-level
        // cache is configured (server). Keyed by a stable per-path token so
        // different fds to the same file coalesce.
        if (auto *cache = global_member_decode_cache()) {
            inflater_.set_cache(cache,
                                std::hash<std::string>{}(current_gz_path_));
        }

        // A range starting past the end of the file yields nothing, so skip
        // the seek entirely rather than decoding up to EOF to discover that.
        if (max_file_bytes_ > 0 && start_bytes_ >= max_file_bytes_) {
            is_finished_ = true;
            decompression_initialized_ = true;
            co_return;
        }

        // Inlined rather than a helper coroutine: GCC 12's coroutine frontend
        // miscompiles this small frame's resume-index dispatch, so the co_await
        // stays in the caller's frame. See docs/concepts/coroutine-caveats.
        use_member_ = false;
        if (indexer_ && indexer_->find_member(start_bytes_, member_)) {
            use_member_ = co_await inflater_.seek_to_member(
                fd_, file_offset_, member_, expected_out_bytes());
            if (use_member_)
                DFTRACER_UTILS_LOG_DEBUG(
                    "Using member %" PRIu64 " at uncompressed offset %" PRIu64
                    " for target %zu",
                    member_.member_idx, member_.uc_offset, start_bytes_);
        }

        if (!use_member_) {
            if (!co_await inflater_.initialize(
                    fd_, file_offset_, 0,
                    constants::indexer::ZLIB_GZIP_WINDOW_BITS,
                    expected_out_bytes())) {
                throw ReaderError(ReaderError::COMPRESSION_ERROR,
                                  "Failed to initialize inflater");
            }
        }

        decompression_initialized_ = true;
        co_await on_initialized();
    }

    /// Hook for derived streams to seek to their own start once the base
    /// stream is positioned.
    virtual coro::CoroTask<void> on_initialized() { co_return; }

    /// Uncompressed offset the stream sits at right after a seek, and the
    /// base every skip measures from. Derived streams must ask here rather
    /// than reach for the member record directly.
    std::size_t seek_anchor_offset() const { return member_.uc_offset; }

    coro::CoroTask<void> skip(std::size_t target_position) {
        std::size_t current_pos = seek_anchor_offset();
        if (target_position > current_pos) {
            co_await inflater_.skip_bytes(fd_, file_offset_,
                                          target_position - current_pos);
        }
    }

    bool is_at_target_end() const {
        return current_position_ >= target_end_bytes_;
    }

    // Decode-buffer size hint (uncompressed span); 0 for an unbounded end.
    std::size_t expected_out_bytes() const {
        if (target_end_bytes_ <= start_bytes_) return 0;
        const std::size_t span = target_end_bytes_ - start_bytes_;
        if (max_file_bytes_ == 0 || span > max_file_bytes_) return 0;
        return span;
    }

    coro::CoroTask<void> restart_compression() {
        inflater_.reset();
        if (use_member_) {
            if (!co_await inflater_.seek_to_member(fd_, file_offset_, member_,
                                                   expected_out_bytes())) {
                throw ReaderError(ReaderError::COMPRESSION_ERROR,
                                  "Failed to reinitialize from member");
            }
        } else {
            if (!co_await inflater_.initialize(
                    fd_, file_offset_, 0,
                    constants::indexer::ZLIB_GZIP_WINDOW_BITS,
                    expected_out_bytes())) {
                throw ReaderError(ReaderError::COMPRESSION_ERROR,
                                  "Failed to initialize inflater");
            }
        }
    }
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_STREAM_H
