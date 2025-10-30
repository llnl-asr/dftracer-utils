#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_STREAM_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_STREAM_H

#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/reader/internal/stream.h>  // Public ReaderStream interface

namespace dftracer::utils::utilities::reader::internal {

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
    virtual void initialize(
        const std::string &gz_path, std::size_t start_bytes,
        std::size_t end_bytes,
        dftracer::utils::utilities::indexer::internal::Indexer &indexer) = 0;
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_STREAM_H
