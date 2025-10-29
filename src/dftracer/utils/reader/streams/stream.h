#ifndef DFTRACER_UTILS_READER_STREAMS_STREAM_H
#define DFTRACER_UTILS_READER_STREAMS_STREAM_H

#include <dftracer/utils/indexer/indexer.h>
#include <dftracer/utils/reader/stream.h>  // Public ReaderStream interface

namespace dftracer::utils {

/**
 * @brief Base class for internal stream implementations.
 *
 * Extends the public ReaderStream interface and adds internal initialization
 * methods.
 */
class StreamBase : public ReaderStream {
   public:
    virtual ~StreamBase() = default;

   protected:
    /**
     * @brief Initialize stream with file path and byte range.
     *
     * Internal method used by stream factories to set up the stream.
     */
    virtual void initialize(const std::string &gz_path, std::size_t start_bytes,
                            std::size_t end_bytes, Indexer &indexer) = 0;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_READER_STREAMS_STREAM_H
