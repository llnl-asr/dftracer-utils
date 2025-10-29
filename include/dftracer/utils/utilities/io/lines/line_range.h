#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_LINE_RANGE_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_LINE_RANGE_H

#include <dftracer/utils/utilities/io/lines/iterator.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>
#include <dftracer/utils/utilities/io/lines/sources/indexed_file_line_iterator.h>
#include <dftracer/utils/utilities/io/lines/sources/plain_file_line_iterator.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace dftracer::utils::utilities::io::lines {

/**
 * @brief Type-erased range of lines from various sources.
 *
 * This class provides a unified interface for iterating over lines from
 * different sources (indexed files, plain files, memory, streams) using
 * std::variant for type erasure. It enables composition and interchangeable
 * use of different line sources.
 *
 * Usage:
 * @code
 * // From indexed file
 * auto reader = ReaderFactory::create("file.gz", "file.gz.idx");
 * LineRange range1 = LineRange::from_indexed_file(reader, 1, 100);
 *
 * // From plain file
 * LineRange range2 = LineRange::from_plain_file("data.txt");
 *
 * // Iterate uniformly
 * while (range1.has_next()) {
 *     Line line = range1.next();
 *     // Process line...
 * }
 * @endcode
 */
class LineRange {
   private:
    using IteratorVariant = std::variant<sources::IndexedFileLineIterator,
                                         sources::PlainFileLineIterator>;

    std::optional<IteratorVariant> iterator_;

   public:
    /**
     * @brief Default constructor creates empty range.
     */
    LineRange() = default;
    /**
     * @brief Create LineRange from indexed file (gzip, tar.gz, etc.).
     *
     * @param config Configuration for the indexed file line iterator
     */
    static LineRange from_indexed_file(
        const sources::IndexedFileLineIteratorConfig& config) {
        return LineRange(sources::IndexedFileLineIterator(config));
    }

    /**
     * @brief Create LineRange from plain text file.
     *
     * @param file_path Path to plain text file
     */
    static LineRange from_plain_file(const std::string& file_path) {
        return LineRange(sources::PlainFileLineIterator(file_path));
    }

    /**
     * @brief Create LineRange from plain text file with line range.
     *
     * @param file_path Path to plain text file
     * @param start_line Starting line number (1-based, inclusive)
     * @param end_line Ending line number (1-based, inclusive)
     */
    static LineRange from_plain_file(const std::string& file_path,
                                     std::size_t start_line,
                                     std::size_t end_line) {
        return LineRange(
            sources::PlainFileLineIterator(file_path, start_line, end_line));
    }

    /**
     * @brief Check if more lines are available.
     */
    bool has_next() const {
        if (!iterator_.has_value()) return false;
        return std::visit([](const auto& iter) { return iter.has_next(); },
                          *iterator_);
    }

    /**
     * @brief Get the next line.
     *
     * @return Line object with content and line number
     * @throws std::runtime_error if no more lines available
     */
    Line next() {
        if (!iterator_.has_value()) {
            throw std::runtime_error("LineRange is empty");
        }
        return std::visit([](auto& iter) { return iter.next(); }, *iterator_);
    }

    /**
     * @brief Get the current line position (1-based).
     */
    std::size_t current_position() const {
        if (!iterator_.has_value()) return 0;
        return std::visit(
            [](const auto& iter) { return iter.current_position(); },
            *iterator_);
    }

    /**
     * @brief Collect all remaining lines into a vector.
     *
     * @return Vector of all lines
     */
    std::vector<Line> collect() {
        std::vector<Line> lines;
        while (has_next()) {
            lines.push_back(next());
        }
        return lines;
    }

    /**
     * @brief Collect up to N lines into a vector.
     *
     * @param n Maximum number of lines to collect
     * @return Vector of up to N lines
     */
    std::vector<Line> take(std::size_t n) {
        std::vector<Line> lines;
        lines.reserve(n);
        for (std::size_t i = 0; i < n && has_next(); ++i) {
            lines.push_back(next());
        }
        return lines;
    }

    /**
     * @brief Apply a function to each line.
     *
     * @param func Function to apply to each line
     */
    template <typename Func>
    void for_each(Func&& func) {
        while (has_next()) {
            func(next());
        }
    }

    /**
     * @brief Filter lines based on a predicate.
     *
     * @param predicate Function that returns true for lines to keep
     * @return Vector of lines that match the predicate
     */
    template <typename Predicate>
    std::vector<Line> filter(Predicate&& predicate) {
        std::vector<Line> filtered;
        while (has_next()) {
            Line line = next();
            if (predicate(line)) {
                filtered.push_back(std::move(line));
            }
        }
        return filtered;
    }

    // Use generic iterator template to avoid code duplication
    using Iterator = LineIterator<LineRange>;

    /**
     * @brief Get an iterator to the beginning.
     */
    Iterator begin() { return Iterator(this, false); }

    /**
     * @brief Get an iterator to the end.
     */
    Iterator end() { return Iterator(nullptr, true); }

   private:
    // Private constructor - use factory methods
    explicit LineRange(IteratorVariant iterator)
        : iterator_(std::move(iterator)) {}
};

}  // namespace dftracer::utils::utilities::io::lines

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_LINE_RANGE_H
