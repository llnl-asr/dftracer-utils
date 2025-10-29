#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_LINE_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_LINE_ITERATOR_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/io/lines/iterator.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace dftracer::utils::utilities::io::lines::sources {

/**
 * @brief Lazy line-by-line iterator over plain text files.
 *
 * This iterator reads uncompressed text files directly using std::ifstream.
 * It provides streaming access without loading the entire file into memory.
 * Suitable for large text files, CSV files, log files, etc.
 *
 * Usage:
 * @code
 * PlainFileLineIterator it("data.txt");
 * while (it.has_next()) {
 *     Line line = it.next();
 *     // Process line...
 * }
 * @endcode
 */
class PlainFileLineIterator {
   private:
    std::string file_path_;
    std::shared_ptr<std::ifstream> stream_;
    std::size_t current_line_;
    std::string next_line_;
    bool has_next_line_;
    bool exhausted_;

   public:
    /**
     * @brief Construct iterator from file path.
     *
     * @param file_path Path to the plain text file
     * @throws std::runtime_error if file cannot be opened
     */
    explicit PlainFileLineIterator(std::string file_path)
        : file_path_(std::move(file_path)),
          stream_(std::make_shared<std::ifstream>(file_path_)),
          current_line_(0),
          has_next_line_(false),
          exhausted_(false) {
        if (!stream_->is_open()) {
            throw std::runtime_error("Cannot open file: " + file_path_);
        }
        if (!fs::exists(file_path_)) {
            throw std::runtime_error("File does not exist: " + file_path_);
        }

        // Pre-read the first line
        advance();
    }

    /**
     * @brief Construct iterator from file path with line range.
     *
     * @param file_path Path to the plain text file
     * @param start_line Starting line number (1-based, inclusive)
     * @param end_line Ending line number (1-based, inclusive)
     * @throws std::runtime_error if file cannot be opened
     */
    PlainFileLineIterator(std::string file_path, std::size_t start_line,
                          std::size_t end_line)
        : file_path_(std::move(file_path)),
          stream_(std::make_shared<std::ifstream>(file_path_)),
          current_line_(0),
          has_next_line_(false),
          exhausted_(false) {
        if (!stream_->is_open()) {
            throw std::runtime_error("Cannot open file: " + file_path_);
        }
        if (!fs::exists(file_path_)) {
            throw std::runtime_error("File does not exist: " + file_path_);
        }
        if (start_line < 1 || end_line < start_line) {
            throw std::invalid_argument("Invalid line range");
        }

        // Skip to start_line
        std::string dummy;
        for (std::size_t i = 1; i < start_line && std::getline(*stream_, dummy);
             ++i) {
            current_line_++;
        }

        // Pre-read the first line in range
        advance();

        // Store end_line for range checking
        end_line_ = end_line;
    }

    /**
     * @brief Check if more lines are available.
     */
    bool has_next() const { return has_next_line_ && !exhausted_; }

    /**
     * @brief Get the next line.
     *
     * @return Line object with content and line number
     * @throws std::runtime_error if no more lines available
     */
    Line next() {
        if (!has_next()) {
            throw std::runtime_error("No more lines available");
        }

        Line result(next_line_, current_line_);
        advance();
        return result;
    }

    /**
     * @brief Get the current line position (1-based).
     */
    std::size_t current_position() const { return current_line_; }

    /**
     * @brief Get the file path.
     */
    const std::string& get_file_path() const { return file_path_; }

    using Iterator = LineIterator<PlainFileLineIterator>;

    /**
     * @brief Get an iterator to the beginning.
     */
    Iterator begin() { return Iterator(this, false); }

    /**
     * @brief Get an iterator to the end.
     */
    Iterator end() { return Iterator(nullptr, true); }

   private:
    std::size_t end_line_ = 0;  // 0 means no limit

    /**
     * @brief Advance to the next line.
     */
    void advance() {
        if (exhausted_) {
            has_next_line_ = false;
            return;
        }

        // Check if we've reached the end_line limit
        if (end_line_ > 0 && current_line_ >= end_line_) {
            has_next_line_ = false;
            exhausted_ = true;
            return;
        }

        if (std::getline(*stream_, next_line_)) {
            current_line_++;
            has_next_line_ = true;
        } else {
            has_next_line_ = false;
            exhausted_ = true;
        }
    }
};

}  // namespace dftracer::utils::utilities::io::lines::sources

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_LINE_ITERATOR_H
