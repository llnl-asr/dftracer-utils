#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_STREAMING_LINE_READER_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_STREAMING_LINE_READER_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/io/lines/line_bytes_range.h>
#include <dftracer/utils/utilities/io/lines/line_range.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>

#include <memory>
#include <string>

namespace dftracer::utils::utilities::io::lines {

/**
 * @brief Configuration for StreamingLineReader with fluent API.
 *
 * Usage:
 * @code
 * auto config = StreamingLineReaderConfig()
 *     .with_file("file.gz")
 *     .with_index("file.gz.idx")
 *     .with_line_range(1, 100);
 *
 * auto range = StreamingLineReader::read(config);
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
 * @brief Composable utility for streaming line reading from various sources.
 *
 * This utility automatically detects the file format and creates the
 * appropriate line iterator. It supports:
 * - Indexed compressed files (.gz, .tar.gz) via Reader
 * - Plain text files
 * - Automatic index file detection
 *
 * Usage:
 * @code
 * // Auto-detect format
 * auto range = StreamingLineReader::read("data.gz");  // Uses index if
 * available while (range.has_next()) { Line line = range.next();
 *     // Process line...
 * }
 *
 * // Explicit line range
 * auto range2 = StreamingLineReader::read("data.gz", 100, 200);
 *
 * // Force plain file reading (no decompression)
 * auto range3 = StreamingLineReader::read_plain("data.txt");
 * @endcode
 */
class StreamingLineReader {
   public:
    /**
     * @brief Read lines from a file, auto-detecting format and index.
     *
     * This method automatically:
     * 1. Detects if an index file exists (.idx)
     * 2. Creates appropriate reader (indexed or plain)
     * 3. Returns a LineRange for streaming iteration
     *
     * @param config Configuration for the line reader
     * @return LineRange for streaming iteration
     */
    static LineRange read(const StreamingLineReaderConfig& config) {
        const std::string& file_path = config.file_path();
        std::size_t start_line = config.start_line();
        std::size_t end_line = config.end_line();
        const std::string& idx_path = config.index_path();
        // Check if index file exists
        std::string actual_idx_path =
            idx_path.empty() ? file_path + ".idx" : idx_path;
        bool has_index = fs::exists(actual_idx_path);

        DFTRACER_UTILS_LOG_DEBUG(
            "StreamingLineReader::read - file=%s, idx_path_param=%s, "
            "actual_idx=%s, has_index=%d",
            file_path.c_str(), idx_path.c_str(), actual_idx_path.c_str(),
            has_index);

        // Check file extension to determine if it's compressed
        bool is_compressed = is_compressed_format(file_path);

        if (is_compressed && has_index) {
            auto iter_config =
                sources::IndexedFileLineIteratorConfig().with_file(
                    file_path, actual_idx_path);
            if (start_line > 0 && end_line > 0) {
                iter_config.with_line_range(start_line, end_line);
            }
            return LineRange::from_indexed_file(iter_config);
        } else {
            // Use plain file reader
            if (start_line > 0 && end_line > 0) {
                return LineRange::from_plain_file(file_path, start_line,
                                                  end_line);
            } else {
                return LineRange::from_plain_file(file_path);
            }
        }
    }

    /**
     * @brief Read lines from a file using indexed reader.
     *
     * @param file_path Path to the compressed file
     * @param idx_path Path to the index file
     * @param start_line Starting line (1-based, inclusive), 0 means start
     * @param end_line Ending line (1-based, inclusive), 0 means end
     * @return LineRange for streaming iteration
     */
    static LineRange read_indexed(
        sources::IndexedFileLineIteratorConfig& config) {
        return LineRange::from_indexed_file(config);
    }

    /**
     * @brief Read lines from a plain text file (no decompression).
     *
     * @param file_path Path to the plain text file
     * @param start_line Starting line (1-based, inclusive), 0 means start
     * @param end_line Ending line (1-based, inclusive), 0 means end
     * @return LineRange for streaming iteration
     */
    static LineRange read_plain(const std::string& file_path,
                                std::size_t start_line = 0,
                                std::size_t end_line = 0) {
        if (start_line > 0 && end_line > 0) {
            return LineRange::from_plain_file(file_path, start_line, end_line);
        } else {
            return LineRange::from_plain_file(file_path);
        }
    }

   private:
    /**
     * @brief Check if file extension indicates compressed format.
     */
    static bool is_compressed_format(const std::string& file_path) {
        fs::path p(file_path);
        std::string ext = p.extension().string();

        // Check for .gz extension
        if (ext == ".gz") {
            return true;
        }

        // Check for .tar.gz or .tgz
        std::string stem = p.stem().string();
        if (!stem.empty()) {
            fs::path stem_path(stem);
            if (stem_path.extension().string() == ".tar") {
                return true;
            }
        }

        if (ext == ".tgz") {
            return true;
        }

        return false;
    }
};

}  // namespace dftracer::utils::utilities::io::lines

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_STREAMING_LINE_READER_H
