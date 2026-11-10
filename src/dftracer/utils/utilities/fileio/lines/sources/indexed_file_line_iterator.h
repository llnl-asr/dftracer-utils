// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_INDEXED_FILE_LINE_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_INDEXED_FILE_LINE_ITERATOR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>

#include <cstddef>

namespace dftracer::utils::utilities::fileio::lines::sources {

/**
 * @brief Configuration for an indexed file line source.
 *
 * The source automatically selects the appropriate stream type:
 * - LINE_RANGE: Uses StreamType::LINE
 * - BYTE_RANGE: Uses StreamType::LINE_BYTES
 *
 * Usage:
 * @code
 * // Option 1: From file path
 * auto config = IndexedFileLineIteratorConfig()
 *     .with_file("file.gz", "file.gz.idx")
 *     .with_line_range(1, 100)
 *     .with_buffer_size(128 * 1024);
 *
 * // Option 2: From existing reader
 * auto config = IndexedFileLineIteratorConfig()
 *     .with_reader(reader)
 *     .with_line_range(1, 100);
 * @endcode
 */
class IndexedFileLineIteratorConfig {
   private:
    dstd::SharedPtr<dftracer::utils::utilities::reader::internal::Reader>
        reader_;
    dftracer::utils::utilities::reader::internal::RangeType range_type_ =
        dftracer::utils::utilities::reader::internal::RangeType::LineRange;
    std::size_t start_ = 1;
    std::size_t end_ = 0;
    std::size_t buffer_size_ = 1024 * 1024;  ///< 1MB default buffer
    dstd::Optional<dstd::Error> error_;

   public:
    IndexedFileLineIteratorConfig() = default;

    /// Opens a reader over @p file_path. When that fails, the config holds the
    /// error, which @ref error gives, and no reader.
    IndexedFileLineIteratorConfig& with_file(dstd::CStringView file_path,
                                             dstd::StringView index_path = "") {
        auto created =
            dftracer::utils::utilities::reader::internal::ReaderFactory::create(
                file_path, index_path);
        if (created.has_value()) {
            reader_ = std::move(*created);
            error_.reset();
        } else {
            reader_ = nullptr;
            error_ = created.error();
        }
        return *this;
    }

    IndexedFileLineIteratorConfig& with_reader(
        dstd::SharedPtr<dftracer::utils::utilities::reader::internal::Reader>
            reader) {
        reader_ = reader;
        return *this;
    }

    IndexedFileLineIteratorConfig& with_line_range(std::size_t start_line,
                                                   std::size_t end_line) {
        range_type_ =
            dftracer::utils::utilities::reader::internal::RangeType::LineRange;
        start_ = start_line;
        end_ = end_line;
        return *this;
    }

    IndexedFileLineIteratorConfig& with_byte_range(std::size_t start_bytes,
                                                   std::size_t end_bytes) {
        range_type_ =
            dftracer::utils::utilities::reader::internal::RangeType::ByteRange;
        start_ = start_bytes;
        end_ = end_bytes;
        return *this;
    }

    IndexedFileLineIteratorConfig& with_buffer_size(std::size_t size) {
        buffer_size_ = size;
        return *this;
    }

    dstd::SharedPtr<dftracer::utils::utilities::reader::internal::Reader>
    reader() const {
        return reader_;
    }
    dftracer::utils::utilities::reader::internal::RangeType range_type() const {
        return range_type_;
    }
    std::size_t start() const { return start_; }
    std::size_t end() const { return end_; }
    std::size_t buffer_size() const { return buffer_size_; }
    /// The error of a failed @ref with_file, if any.
    const dstd::Optional<dstd::Error>& error() const { return error_; }
};

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_INDEXED_FILE_LINE_ITERATOR_H
