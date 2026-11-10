// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_STREAMING_LINE_READER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_STREAMING_LINE_READER_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_bytes_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_line_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>

namespace dftracer::utils::utilities::fileio::lines {

namespace detail {

/// @p source as items of a Result. The source reports a failure as an
/// exception; that failure is yielded as an error item, after which the
/// generator ends.
inline coro::AsyncGenerator<dstd::Result<Line>> lines_as_results(
    coro::AsyncGenerator<Line> source) {
    for (;;) {
        auto settled = co_await source.settled_next();
        if (settled.stopped) co_return;
        if (settled.exception) {
            co_yield dstd::Result<Line>(
                common::error_of_exception(settled.exception));
            co_return;
        }
        auto& next = *settled.value;
        if (!next) co_return;
        co_yield Line(*next);
    }
}

/// A generator that yields @p error and ends.
inline coro::AsyncGenerator<dstd::Result<Line>> failed_lines(
    dstd::Error error) {
    co_yield dstd::Result<Line>(std::move(error));
}

}  // namespace detail

/**
 * @brief Configuration for @ref StreamingLineReader with fluent API.
 *
 * Usage:
 * @code
 * auto config = StreamingLineReaderConfig()
 *     .with_file("file.gz")
 *     .with_index("trace-root/.dftindex")
 *     .with_line_range(1, 100);
 *
 * @endcode
 */
class StreamingLineReaderConfig {
   private:
    dstd::String file_path_;
    dstd::String index_path_;
    std::size_t start_line_ = 0;
    std::size_t end_line_ = 0;

   public:
    StreamingLineReaderConfig() = default;

    StreamingLineReaderConfig& with_file(dstd::StringView file_path) {
        file_path_ = file_path;
        return *this;
    }

    StreamingLineReaderConfig& with_index(dstd::StringView index_path) {
        index_path_ = index_path;
        return *this;
    }

    StreamingLineReaderConfig& with_line_range(std::size_t start_line,
                                               std::size_t end_line) {
        start_line_ = start_line;
        end_line_ = end_line;
        return *this;
    }

    dstd::CStringView file_path() const { return file_path_; }
    dstd::CStringView index_path() const { return index_path_; }
    std::size_t start_line() const { return start_line_; }
    std::size_t end_line() const { return end_line_; }
};

/**
 * @brief Async streaming line reader for gzip traces.
 *
 * Opens an indexed compressed trace via the Reader when a `.dftindex`
 * exists, else streams the gzip file directly.
 */
class StreamingLineReader {
   public:
    /**
     * @brief Async read lines from a file, auto-detecting format.
     *
     * Returns an AsyncGenerator of Result<Line> for non-blocking iteration. A
     * failure, such as an index path that cannot be checked, a reader that
     * cannot be opened or data that cannot be read, is yielded as an error
     * item, after which the generator ends:
     * @code
     * auto gen = StreamingLineReader::read_async(config);
     * for (;;) {
     *     auto line = co_await gen.next();
     *     if (!line) break;
     *     if (!*line) return line->error();
     *     co_await process(**line);
     * }
     * @endcode
     */
    static coro::AsyncGenerator<dstd::Result<Line>> read_async(
        const StreamingLineReaderConfig& config) {
        dstd::CStringView file_path = config.file_path();
        dstd::CStringView index_path = config.index_path();
        bool is_compressed = is_compressed_format(file_path);

        // Only use the indexed path when an index was explicitly
        // provided. Auto-discovering `.dftindex` would silently
        // override callers that intentionally omit the index to
        // get single-pass streaming decompression.
        bool has_index = false;
        dstd::String actual_index_path;
        if (!index_path.empty()) {
            actual_index_path = index_path;
            const auto found =
                dstd::fs::exists(dstd::PathView(actual_index_path));
            if (!found.has_value()) return detail::failed_lines(found.error());
            has_index = *found;
        }

        if (is_compressed && has_index) {
            auto iter_config =
                sources::IndexedFileLineIteratorConfig().with_file(
                    file_path, actual_index_path);
            if (config.start_line() > 0 || config.end_line() > 0) {
                iter_config.with_line_range(config.start_line(),
                                            config.end_line());
            }
            return sources::async_indexed_file_lines(iter_config);
        }
        return detail::lines_as_results(sources::async_streaming_gz_lines(
            dstd::String(file_path.view()), config.start_line(),
            config.end_line()));
    }

   private:
    /**
     * @brief Check if file extension indicates compressed format.
     */
    static bool is_compressed_format(dstd::StringView file_path) {
        return dstd::PathView(file_path).extension() == ".gz";
    }
};

}  // namespace dftracer::utils::utilities::fileio::lines

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LINES_STREAMING_LINE_READER_H
