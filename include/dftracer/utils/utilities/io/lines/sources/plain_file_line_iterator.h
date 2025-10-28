#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_LINE_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_LINE_ITERATOR_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/io/lines/iterator.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>

#include <fstream>
#include <stdexcept>
#include <string>

namespace dftracer::utils::utilities::io::lines::sources {

/**
 * @brief Lazy, single‑buffer line‑by‑line iterator over plain text files.
 *
 * Reads directly from std::ifstream without loading the whole file.
 * Uses a single string buffer; the string_view returned by next()
 * remains valid until the next call to next().
 *
 * Usage:
 * @code
 * PlainFileLineIterator it("data.txt");
 * while (it.has_next()) {
 *     Line line = it.next();
 *     process(line.content);
 * }
 * @endcode
 */
class PlainFileLineIterator {
   private:
    std::string file_path_;
    mutable std::ifstream stream_;
    std::size_t end_line_ = 0;         // 0 = no limit
    std::size_t current_line_ = 0;     // last returned line number
    mutable std::string line_buffer_;
    mutable bool exhausted_ = false;
    mutable bool prefetched_ = false;  // true if we already read a line ahead
    mutable bool has_line_ = false;    // valid line in buffer

   public:
    explicit PlainFileLineIterator(std::string file_path)
        : file_path_(std::move(file_path)), stream_(file_path_) {
        validate_file();
    }

    PlainFileLineIterator(std::string file_path, std::size_t start_line,
                          std::size_t end_line)
        : file_path_(std::move(file_path)),
          stream_(file_path_),
          end_line_(end_line) {
        validate_file();

        if (start_line < 1 || end_line < start_line)
            throw std::invalid_argument("Invalid line range");

        // Skip to start_line (inclusive)
        std::string dummy;
        for (std::size_t i = 1; i < start_line && std::getline(stream_, dummy);
             ++i) {
            current_line_ = i;
        }
    }

    bool has_next() const {
        if (exhausted_) return false;
        if (!prefetched_) prefetch();
        return has_line_;
    }

    Line next() {
        if (!has_next()) throw std::runtime_error("No more lines available");

        current_line_++;
        prefetched_ = false;  // next call will read the next line
        return Line(std::string_view(line_buffer_), current_line_);
    }

    std::size_t current_position() const { return current_line_; }

    const std::string& get_file_path() const { return file_path_; }

    using Iterator = LineIterator<PlainFileLineIterator>;
    Iterator begin() { return Iterator(this, false); }
    Iterator end() { return Iterator(nullptr, true); }

   private:
    void validate_file() {
        if (!stream_.is_open())
            throw std::runtime_error("Cannot open file: " + file_path_);
        if (!fs::exists(file_path_))
            throw std::runtime_error("File does not exist: " + file_path_);
    }

    void prefetch() const {
        if (end_line_ > 0 && current_line_ >= end_line_) {
            has_line_ = false;
            exhausted_ = true;
            return;
        }

        if (std::getline(stream_, line_buffer_)) {
            has_line_ = true;
            prefetched_ = true;
        } else {
            has_line_ = false;
            exhausted_ = true;
        }
    }
};

}  // namespace dftracer::utils::utilities::io::lines::sources

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_LINE_ITERATOR_H
