#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_READER_STREAM_CACHE_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_READER_STREAM_CACHE_H

#include <dftracer/utils/utilities/reader/internal/stream.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>

#include <memory>
#include <string>

namespace dftracer::utils::utilities::reader::internal {

/**
 * @brief Cache for reader streams to enable reuse across multiple read calls.
 *
 * Maintains a single stream along with metadata to determine if it can be
 * reused for subsequent read operations. This enables efficient POSIX-style
 * reading where multiple sequential read() calls are made.
 */
class ReaderStreamCache {
   private:
    std::unique_ptr<ReaderStream> stream_;
    StreamType type_;
    std::string path_;
    std::size_t start_bytes_;
    std::size_t end_bytes_;
    bool initialized_;

   public:
    ReaderStreamCache()
        : type_(StreamType::BYTES),
          start_bytes_(0),
          end_bytes_(0),
          initialized_(false) {}

    /**
     * @brief Check if the cached stream can continue for the requested
     * parameters.
     *
     * @param type Stream type requested
     * @param path File path requested
     * @param start Start byte offset
     * @param end End byte offset
     * @return true if cached stream can be reused, false otherwise
     */
    bool can_continue(StreamType type, const std::string& path,
                      std::size_t start, std::size_t end) const {
        if (!initialized_ || !stream_) return false;
        if (stream_->done()) return false;
        if (type_ != type) return false;
        if (path_ != path) return false;
        if (start_bytes_ != start) return false;
        if (end_bytes_ != end) return false;
        return true;
    }

    /**
     * @brief Update the cache with a new stream.
     *
     * @param new_stream New stream to cache
     * @param type Stream type
     * @param path File path
     * @param start Start byte offset
     * @param end End byte offset
     */
    void update(std::unique_ptr<ReaderStream> new_stream, StreamType type,
                const std::string& path, std::size_t start, std::size_t end) {
        stream_ = std::move(new_stream);
        type_ = type;
        path_ = path;
        start_bytes_ = start;
        end_bytes_ = end;
        initialized_ = true;
    }

    /**
     * @brief Update the position after a read operation.
     *
     * This should be called after each read() to update the cached start
     * position so that subsequent reads can continue from where the stream left
     * off.
     *
     * @param new_start New start position (typically old start + bytes read)
     */
    void update_position(std::size_t new_start) { start_bytes_ = new_start; }

    /**
     * @brief Get the cached stream.
     *
     * @return Pointer to cached stream, or nullptr if not initialized
     */
    ReaderStream* get() { return stream_.get(); }

    /**
     * @brief Clear the cache.
     */
    void clear() {
        stream_.reset();
        initialized_ = false;
    }

    /**
     * @brief Check if cache has a valid stream.
     *
     * @return true if initialized with a stream, false otherwise
     */
    bool has_stream() const { return initialized_ && stream_ != nullptr; }
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_READER_STREAM_CACHE_H
