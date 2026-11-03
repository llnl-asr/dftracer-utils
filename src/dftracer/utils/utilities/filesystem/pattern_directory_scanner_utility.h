// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_PATTERN_DIRECTORY_SCANNER_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_PATTERN_DIRECTORY_SCANNER_H

#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>

#include <algorithm>
#include <iterator>

namespace dftracer::utils::utilities::filesystem {

/// The suffixes directory discovery lists as traces: JSON records, one per
/// line, in .pfw, .jsonl or .ndjson files, plain or gzip. A .json file is
/// not listed, since directories hold other JSON such as summaries.
inline constexpr dstd::StringView TRACE_FILE_SUFFIXES[] = {
    ".pfw", ".pfw.gz", ".jsonl", ".jsonl.gz", ".ndjson", ".ndjson.gz"};

inline dstd::Vector<dstd::String> trace_file_patterns() {
    return dstd::Vector<dstd::String>(std::begin(TRACE_FILE_SUFFIXES),
                                      std::end(TRACE_FILE_SUFFIXES));
}

/// Whether `path` ends in one of trace_file_patterns().
inline bool is_trace_file(dstd::StringView path) {
    return std::any_of(std::begin(TRACE_FILE_SUFFIXES),
                       std::end(TRACE_FILE_SUFFIXES),
                       [&](dstd::StringView p) { return path.ends_with(p); });
}

/**
 * @brief Input for pattern-based directory scanning.
 */
struct PatternDirectoryScannerUtilityInput {
    dstd::Path path;
    bool recursive = false;
    bool populate_size = true;
    dstd::Vector<dstd::String>
        patterns;  ///< e.g., {".pfw", ".pfw.gz", "*.txt"}

    PatternDirectoryScannerUtilityInput() = default;

    PatternDirectoryScannerUtilityInput(dstd::StringView p,
                                        dstd::Vector<dstd::String> pats,
                                        bool rec = false, bool with_size = true)
        : path(p),
          recursive(rec),
          populate_size(with_size),
          patterns(std::move(pats)) {}

    static PatternDirectoryScannerUtilityInput from_path(dstd::StringView p) {
        PatternDirectoryScannerUtilityInput input;
        input.path = dstd::Path(p);
        return input;
    }

    PatternDirectoryScannerUtilityInput& with_patterns(
        dstd::Vector<dstd::String> pats) {
        patterns = std::move(pats);
        return *this;
    }

    PatternDirectoryScannerUtilityInput& with_recursive(bool rec) {
        recursive = rec;
        return *this;
    }
};

/**
 * @brief Directory scanner that filters files by patterns/extensions.
 *
 * This component extends DirectoryScanner by adding pattern matching
 * for file extensions or patterns. Supports:
 * - Extension matching: ".pfw", ".gz"
 * - Suffix matching: ".pfw.gz"
 * - Simple glob patterns: "*.txt" (future enhancement)
 *
 * Usage:
 * @code
 * PatternDirectoryScannerUtility scanner;
 * auto files = co_await scanner(
 *     PatternDirectoryScannerUtilityInput{"/path/to/dir", {".pfw",
 * ".pfw.gz"}});
 * @endcode
 */
class PatternDirectoryScannerUtility {
   public:
    /**
     * @brief Scan directory and filter by patterns.
     *
     * @param ctx Coroutine scope for async execution
     * @param input Directory path, patterns, and recursive flag
     * @return Vector of file entries matching patterns
     */
    coro::CoroTask<dstd::Vector<FileEntry>> operator()(
        coro::Scope& ctx,
        const PatternDirectoryScannerUtilityInput& input) const {
        DirectoryScannerUtilityInput dir_input{
            input.path.str(), input.recursive, input.populate_size};
        dstd::Vector<FileEntry> all_entries =
            co_await base_scanner_(ctx, dir_input);

        // Step 2: Filter by patterns
        dstd::Vector<FileEntry> matched_entries;

        for (const auto& entry : all_entries) {
            // Only consider regular files
            if (!entry.is_regular_file) continue;

            // Check if file matches any pattern
            if (matches_any_pattern(entry.path.str(), input.patterns)) {
                matched_entries.push_back(entry);
            }
        }

        co_return matched_entries;
    }

    /// Scope-less overload: opens its own scope on the current executor.
    coro::CoroTask<dstd::Vector<FileEntry>> operator()(
        const PatternDirectoryScannerUtilityInput& input) const {
        return coro::with_scope(
            [this, input](coro::Scope& s) { return (*this)(s, input); });
    }

   private:
    DirectoryScannerUtility base_scanner_;

    /**
     * @brief Check if a path matches any of the given patterns.
     */
    bool matches_any_pattern(dstd::StringView path,
                             const dstd::Vector<dstd::String>& patterns) const {
        if (patterns.empty()) {
            return true;  // No patterns = match everything
        }

        for (const auto& pattern : patterns) {
            if (matches_pattern(path, pattern)) {
                return true;
            }
        }

        return false;
    }

    /**
     * @brief Check if a path matches a single pattern.
     *
     * Currently supports:
     * - Extension matching: ".txt" matches "file.txt"
     * - Suffix matching: ".pfw.gz" matches "file.pfw.gz"
     * - Prefix glob: "*.txt" matches any file ending with ".txt"
     */
    bool matches_pattern(dstd::StringView path,
                         const dstd::String& pattern) const {
        if (pattern.empty()) {
            return true;
        }

        // Simple glob: "*.ext" -> matches any file ending with ".ext"
        if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
            return ends_with(path, dstd::StringView(pattern).substr(1));
        }

        // Direct extension/suffix match
        if (pattern[0] == '.') {
            return ends_with(path, pattern);
        }

        // Exact filename match
        return dstd::PathView(path).filename() == pattern;
    }

    /**
     * @brief Check if string ends with suffix.
     */
    bool ends_with(dstd::StringView str, dstd::StringView suffix) const {
        if (suffix.size() > str.size()) {
            return false;
        }
        return str.compare(str.size() - suffix.size(), suffix.size(), suffix) ==
               0;
    }
};

}  // namespace dftracer::utils::utilities::filesystem

#endif  // DFTRACER_UTILS_UTILITIES_FILESYSTEM_PATTERN_DIRECTORY_SCANNER_H
