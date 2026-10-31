#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_STREAMING_LINE_READER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_STREAMING_LINE_READER_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_bytes_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_indexed_file_line_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>

#include <memory>
#include <string>

namespace dftracer::utils::utilities::fileio::lines {

/**
 * @brief Configuration for StreamingLineReader with fluent API.
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
    std::string file_path_;
    std::string index_path_;
    std::size_t start_line_ = 0;
    std::size_t end_line_ = 0;

   public:
    StreamingLineReaderConfig() = default;

    StreamingLineReaderConfig& with_file(const std::string& file_path) {
        file_path_ = file_path;
        return *this;
    }

    StreamingLineReaderConfig& with_index(const std::string& index_path) {
        index_path_ = index_path;
        return *this;
    }

    StreamingLineReaderConfig& with_line_range(std::size_t start_line,
                                               std::size_t end_line) {
        start_line_ = start_line;
        end_line_ = end_line;
        return *this;
    }

    const std::string& file_path() const { return file_path_; }
    const std::string& index_path() const { return index_path_; }
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
     * Returns an AsyncGenerator<Line> for non-blocking iteration:
     * @code
     * auto gen = StreamingLineReader::read_async(config);
     * for (;;) {
     *     auto line = co_await gen.next();
     *     if (!line) break;
     *     co_await process(*line);
     * }
     * @endcode
     */
    static coro::AsyncGenerator<Line> read_async(
        const StreamingLineReaderConfig& config) {
        const std::string& file_path = config.file_path();
        const std::string& index_path = config.index_path();
        bool is_compressed = is_compressed_format(file_path);

        // Only use the indexed path when an index was explicitly
        // provided. Auto-discovering `.dftindex` would silently
        // override callers that intentionally omit the index to
        // get single-pass streaming decompression.
        bool has_index = false;
        std::string actual_index_path;
        if (!index_path.empty()) {
            actual_index_path = index_path;
            has_index = fs::exists(actual_index_path);
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
        return sources::async_streaming_gz_lines(file_path, config.start_line(),
                                                 config.end_line());
    }

   private:
    /**
     * @brief Check if file extension indicates compressed format.
     */
    static bool is_compressed_format(const std::string& file_path) {
        fs::path p(file_path);
        std::string ext = p.extension().string();

        return ext == ".gz";
    }
};

}  // namespace dftracer::utils::utilities::fileio::lines

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LINES_STREAMING_LINE_READER_H
