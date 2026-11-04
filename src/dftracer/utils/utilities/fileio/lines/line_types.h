// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_LINE_TYPES_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_LINE_TYPES_H

#include <dftracer/utils/dstd/hasher.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace dftracer::utils::utilities::fileio::lines {

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
    dstd::StringView content;
    std::size_t line_number;  ///< 1-based line number

    Line() : line_number(0) {}
    Line(dstd::StringView text, std::size_t number)
        : content(text), line_number(number) {}

    bool empty() const { return content.empty(); }
    std::size_t size() const { return content.size(); }
};

/**
 * @brief Input for reading a range of lines from an indexed file.
 *
 * This structure encapsulates all information needed to read a specific
 * range of lines from an indexed gzip file.
 * Used for lazy evaluation and caching strategies.
 *
 * Usage:
 * @code
 * auto input = LineReadInput::from_file("data.txt")
 *                  .with_index("/data/.dftindex")
 *                  .with_range(10, 100);
 * @endcode
 */
struct LineReadInput {
    dstd::String file_path;   ///< Path to the archive file
    dstd::String index_path;  ///< Path to the `.dftindex` store
                              ///< (empty for plain files)
    std::size_t start_line;   ///< Starting line (1-based, inclusive), 0 = start
    std::size_t end_line;     ///< Ending line (1-based, inclusive), 0 = end

    LineReadInput() : start_line(0), end_line(0) {}

    LineReadInput(dstd::StringView path, dstd::StringView idx_path,
                  std::size_t first_line, std::size_t last_line)
        : file_path(path),
          index_path(idx_path),
          start_line(first_line),
          end_line(last_line) {}

    static LineReadInput from_file(dstd::StringView path) {
        LineReadInput input;
        input.file_path = path;
        return input;
    }

    LineReadInput& with_index(dstd::StringView idx) {
        index_path = idx;
        return *this;
    }

    bool operator==(const LineReadInput& other) const {
        return file_path == other.file_path && index_path == other.index_path &&
               start_line == other.start_line && end_line == other.end_line;
    }

    bool operator!=(const LineReadInput& other) const {
        return !(*this == other);
    }

    std::size_t line_count() const {
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
    dstd::Vector<Line> lines;
    dstd::Vector<dstd::String>
        storage;  ///< Owns string data for zero-copy views

    Lines() = default;

    explicit Lines(dstd::Vector<Line> ls) : lines(std::move(ls)) {}

    explicit Lines(const dstd::Vector<dstd::String>& strings) {
        storage = strings;
        lines.reserve(storage.size());
        std::size_t line_num = 1;
        for (const auto& str : storage) {
            lines.emplace_back(str, line_num++);
        }
    }

    explicit Lines(dstd::Vector<dstd::String>&& strings) {
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

}  // namespace dftracer::utils::utilities::fileio::lines

// Hash specializations for using Line and LineReadInput in hash-based
// containers
namespace dftracer::utils::dstd {

template <>
struct Hash<dftracer::utils::utilities::fileio::lines::Line> {
    using is_avalanching = void;

    std::size_t operator()(
        const dftracer::utils::utilities::fileio::lines::Line& line) const {
        ::dftracer::utils::utilities::hash::HasherUtility hasher;
        hasher.update(line.content);
        hasher.update(line.line_number);
        return static_cast<std::size_t>(hash::splitmix64(hasher.hash().value));
    }
};

template <>
struct Hash<dftracer::utils::utilities::fileio::lines::LineReadInput> {
    using is_avalanching = void;

    std::size_t operator()(
        const dftracer::utils::utilities::fileio::lines::LineReadInput& req)
        const {
        ::dftracer::utils::utilities::hash::HasherUtility hasher;
        hasher.update(req.file_path);
        hasher.update(req.index_path);
        hasher.update(req.start_line);
        hasher.update(req.end_line);
        return static_cast<std::size_t>(hash::splitmix64(hasher.hash().value));
    }
};

template <>
struct Hash<dftracer::utils::utilities::fileio::lines::Lines> {
    using is_avalanching = void;

    std::size_t operator()(
        const dftracer::utils::utilities::fileio::lines::Lines& lines)
        const noexcept {
        ::dftracer::utils::utilities::hash::HasherUtility hasher;
        for (const auto& line : lines.lines) {
            hasher.update(
                Hash<::dftracer::utils::utilities::fileio::lines::Line>{}(
                    line));
        }
        return static_cast<std::size_t>(hash::splitmix64(hasher.hash().value));
    }
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LINES_LINE_TYPES_H
