#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_LINE_BYTES_RANGE_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_LINE_BYTES_RANGE_H

#include <dftracer/utils/utilities/io/lines/iterator.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>
#include <dftracer/utils/utilities/io/lines/sources/indexed_file_bytes_iterator.h>
#include <dftracer/utils/utilities/io/lines/sources/plain_file_bytes_iterator.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace dftracer::utils::utilities::io::lines {

/**
 * @brief Type-erased range of lines from byte boundaries.
 *
 * This class provides a unified interface for iterating over lines within
 * byte ranges from different sources (indexed files, plain files) using
 * std::variant for type erasure. It mirrors the design of LineRange but
 * operates on byte offsets instead of line numbers.
 *
 * Line boundaries are automatically aligned to ensure complete lines.
 *
 * Usage:
 * @code
 * // From indexed file with byte range
 * auto reader = ReaderFactory::create("file.gz", "file.gz.idx");
 * LineBytesRange range1 = LineBytesRange::from_indexed_file(reader, 1000,
 * 5000);
 *
 * // From plain file with byte range
 * LineBytesRange range2 = LineBytesRange::from_plain_file("data.txt", 1000,
 * 5000);
 *
 * // Iterate uniformly
 * while (range1.has_next()) {
 *     Line line = range1.next();
 *     // Process line...
 * }
 * @endcode
 */
class LineBytesRange {
   private:
    using IteratorVariant = std::variant<sources::IndexedFileBytesIterator,
                                         sources::PlainFileBytesIterator>;

    std::optional<IteratorVariant> iterator_;

   public:
    using Iterator = LineIterator<LineBytesRange>;

    /**
     * @brief Default constructor creates empty range.
     */
    LineBytesRange() = default;

    /**
     * @brief Create LineBytesRange from indexed file (gzip, tar.gz, etc.).
     *
     * Reads lines that fall within the specified byte range.
     * Line boundaries are automatically aligned.
     *
     * @param reader Shared pointer to Reader
     * @param start_byte Starting byte offset (0-based, inclusive)
     * @param end_byte Ending byte offset (0-based, exclusive)
     */
    static LineBytesRange from_indexed_file(
        std::shared_ptr<reader::internal::Reader> reader,
        std::size_t start_byte, std::size_t end_byte) {
        return LineBytesRange(
            sources::IndexedFileBytesIterator(reader, start_byte, end_byte));
    }

    /**
     * @brief Create LineBytesRange from plain text file.
     *
     * Reads lines that fall within the specified byte range.
     * Line boundaries are automatically aligned.
     *
     * @param file_path Path to plain text file
     * @param start_byte Starting byte offset (0-based, inclusive)
     * @param end_byte Ending byte offset (0-based, exclusive)
     */
    static LineBytesRange from_plain_file(const std::string& file_path,
                                          std::size_t start_byte,
                                          std::size_t end_byte) {
        return LineBytesRange(
            sources::PlainFileBytesIterator(file_path, start_byte, end_byte));
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
            throw std::runtime_error("LineBytesRange is empty");
        }
        return std::visit([](auto& iter) { return iter.next(); }, *iterator_);
    }

    /**
     * @brief Get the current line position.
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

    /**
     * @brief Get an iterator to the beginning.
     */
    Iterator begin() { return Iterator(this, false); }

    /**
     * @brief Get an iterator to the end.
     */
    Iterator end() { return Iterator(nullptr, true); }

   private:
    explicit LineBytesRange(IteratorVariant iterator)
        : iterator_(std::move(iterator)) {}
};

}  // namespace dftracer::utils::utilities::io::lines

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_LINE_BYTES_RANGE_H
