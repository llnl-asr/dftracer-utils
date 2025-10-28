#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_LINE_TYPES_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_LINE_TYPES_H

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace dftracer::utils::utilities::io::lines {

/**
 * @brief Represents a single line of text with metadata.
 *
 * This structure holds a line's content along with its line number,
 * enabling easy tracking and processing of lines from various sources.
 *
 * NOTE: Uses string_view for zero-copy performance. The view is only valid
 * until the next call to next() on the iterator.
 */
struct Line {
    std::string_view content;
    std::size_t line_number;  // 1-based line number

    Line() : line_number(0) {}
    Line(std::string_view content_, std::size_t line_number_)
        : content(content_), line_number(line_number_) {}

    bool empty() const { return content.empty(); }
    std::size_t size() const { return content.size(); }
};

/**
 * @brief Input for reading a range of lines from an indexed file.
 *
 * This structure encapsulates all information needed to read a specific
 * range of lines from an indexed archive (gzip, tar.gz, etc.).
 * Used for lazy evaluation and caching strategies.
 *
 * Usage:
 * @code
 * auto input = LineReadInput::from_file("data.txt")
 *                  .with_index("data.txt.idx")
 *                  .with_range(10, 100);
 * @endcode
 */
struct LineReadInput {
    std::string file_path;   // Path to the archive file
    std::string idx_path;    // Path to the index file (empty for plain files)
    std::size_t start_line;  // Starting line (1-based, inclusive), 0 = start
    std::size_t end_line;    // Ending line (1-based, inclusive), 0 = end

    LineReadInput() : start_line(0), end_line(0) {}

    LineReadInput(std::string file_path_, std::string idx_path_,
                  std::size_t start_line_, std::size_t end_line_)
        : file_path(std::move(file_path_)),
          idx_path(std::move(idx_path_)),
          start_line(start_line_),
          end_line(end_line_) {}

    static LineReadInput from_file(std::string path) {
        LineReadInput input;
        input.file_path = std::move(path);
        return input;
    }

    LineReadInput& with_index(std::string idx) {
        idx_path = std::move(idx);
        return *this;
    }

    LineReadInput& with_range(std::size_t start, std::size_t end) {
        start_line = start;
        end_line = end;
        return *this;
    }

    bool operator==(const LineReadInput& other) const {
        return file_path == other.file_path && idx_path == other.idx_path &&
               start_line == other.start_line && end_line == other.end_line;
    }

    bool operator!=(const LineReadInput& other) const {
        return !(*this == other);
    }

    std::size_t num_lines() const {
        return (end_line >= start_line) ? (end_line - start_line + 1) : 0;
    }
};

/**
 * @brief Multiple lines of text.
 *
 * A collection of lines with convenient constructors for building
 * from vectors of strings or Line objects.
 */
struct Lines {
    std::vector<Line> lines;
    std::vector<std::string> storage;  // Owns string data for zero-copy views

    Lines() = default;

    explicit Lines(std::vector<Line> ls) : lines(std::move(ls)) {}

    explicit Lines(const std::vector<std::string>& strings) {
        storage = strings;
        lines.reserve(storage.size());
        std::size_t line_num = 1;
        for (const auto& str : storage) {
            lines.emplace_back(str, line_num++);
        }
    }

    explicit Lines(std::vector<std::string>&& strings) {
        storage = std::move(strings);
        lines.reserve(storage.size());
        std::size_t line_num = 1;
        for (const auto& str : storage) {
            lines.emplace_back(str, line_num++);
        }
    }

    std::size_t size() const { return lines.size(); }

    bool empty() const { return lines.empty(); }
};

}  // namespace dftracer::utils::utilities::io::lines

// Hash specializations for using Line and LineReadInput in hash-based
// containers
namespace std {

template <>
struct hash<dftracer::utils::utilities::io::lines::Line> {
    std::size_t operator()(
        const dftracer::utils::utilities::io::lines::Line& line) const {
        // Combine hash of content and line number
        std::size_t h1 = std::hash<std::string_view>{}(line.content);
        std::size_t h2 = std::hash<std::size_t>{}(line.line_number);
        return h1 ^ (h2 << 1);
    }
};

template <>
struct hash<dftracer::utils::utilities::io::lines::LineReadInput> {
    std::size_t operator()(
        const dftracer::utils::utilities::io::lines::LineReadInput& req) const {
        // Combine hashes of all fields
        std::size_t h1 = std::hash<std::string>{}(req.file_path);
        std::size_t h2 = std::hash<std::string>{}(req.idx_path);
        std::size_t h3 = std::hash<std::size_t>{}(req.start_line);
        std::size_t h4 = std::hash<std::size_t>{}(req.end_line);
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
    }
};

template <>
struct hash<dftracer::utils::utilities::io::lines::Lines> {
    std::size_t operator()(const dftracer::utils::utilities::io::lines::Lines&
                               lines) const noexcept {
        std::size_t h = 0;
        for (const auto& line : lines.lines) {
            h ^=
                std::hash<dftracer::utils::utilities::io::lines::Line>{}(line) +
                0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};

}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_LINE_TYPES_H
