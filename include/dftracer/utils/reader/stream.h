#ifndef DFTRACER_UTILS_READER_STREAM_H
#define DFTRACER_UTILS_READER_STREAM_H

#ifdef __cplusplus
#include <cstddef>

namespace dftracer::utils {

/**
 * @brief Abstract base class for streaming data from readers.
 *
 * ReaderStream provides incremental access to data (bytes, lines, etc.)
 * without loading everything into memory. Implementations handle
 * different formats (gzip, tar, etc.) transparently.
 *
 * Usage:
 * @code
 * auto stream = reader->create_stream(StreamType::MULTI_LINES,
 * RangeType::LINE_RANGE, 1, 100); char buffer[4096]; while (!stream->done()) {
 * size_t bytes = stream->read(buffer, sizeof(buffer));
 *     // Process buffer...
 * }
 * @endcode
 */
class ReaderStream {
   public:
    virtual ~ReaderStream() = default;

    /**
     * @brief Read next chunk of data into buffer.
     *
     * Reads incrementally from the stream. Each call returns the next
     * available chunk up to buffer_size bytes.
     *
     * @param buffer Output buffer
     * @param buffer_size Maximum bytes to read
     * @return Number of bytes actually read (0 if finished)
     */
    virtual std::size_t read(char* buffer, std::size_t buffer_size) = 0;

    /**
     * @brief Check if stream is done (no more data available).
     *
     * @return true if no more data to read, false otherwise
     */
    virtual bool done() const = 0;

    /**
     * @brief Reset stream to beginning.
     *
     * After reset, read() will start from the beginning of the range.
     */
    virtual void reset() = 0;

   protected:
    ReaderStream() = default;
    ReaderStream(const ReaderStream&) = delete;
    ReaderStream& operator=(const ReaderStream&) = delete;
};

}  // namespace dftracer::utils

extern "C" {
#endif

// C API

/**
 * @brief Opaque stream handle (C API).
 */
typedef void* dft_reader_stream_t;

/**
 * @brief Read next chunk from stream into buffer.
 *
 * @param stream Stream handle
 * @param buffer Output buffer
 * @param buffer_size Maximum bytes to read
 * @return Number of bytes actually read (0 if finished)
 */
size_t dft_reader_stream_read(dft_reader_stream_t stream, char* buffer,
                              size_t buffer_size);

/**
 * @brief Check if stream is done.
 *
 * @param stream Stream handle
 * @return 1 if done (no more data), 0 otherwise
 */
int dft_reader_stream_done(dft_reader_stream_t stream);

/**
 * @brief Reset stream to beginning.
 *
 * @param stream Stream handle
 */
void dft_reader_stream_reset(dft_reader_stream_t stream);

/**
 * @brief Destroy stream and free resources.
 *
 * @param stream Stream handle
 */
void dft_reader_stream_destroy(dft_reader_stream_t stream);

#ifdef __cplusplus
}
#endif

#endif  // DFTRACER_UTILS_READER_STREAM_H
