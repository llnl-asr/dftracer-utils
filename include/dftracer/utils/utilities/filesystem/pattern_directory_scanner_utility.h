// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_PATTERN_DIRECTORY_SCANNER_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_PATTERN_DIRECTORY_SCANNER_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::filesystem {

/// The suffixes directory discovery lists as traces: JSON records, one per
/// line, in .pfw, .jsonl or .ndjson files, plain or gzip. A .json file is
/// not listed, since directories hold other JSON such as summaries.
inline const std::vector<std::string>& trace_file_patterns() {
    static const std::vector<std::string> PATTERNS = {
        ".pfw", ".pfw.gz", ".jsonl", ".jsonl.gz", ".ndjson", ".ndjson.gz"};
    return PATTERNS;
}

/// Whether `path` ends in one of trace_file_patterns().
inline bool is_trace_file(std::string_view path) {
    const auto& patterns = trace_file_patterns();
    return std::any_of(patterns.begin(), patterns.end(),
                       [&](const std::string& p) { return path.ends_with(p); });
}

/**
 * @brief Input for pattern-based directory scanning.
 */
struct PatternDirectoryScannerUtilityInput {
    dstd::Path path;
    bool recursive = false;
    bool populate_size = true;
    std::vector<std::string> patterns;  ///< e.g., {".pfw", ".pfw.gz", "*.txt"}

    PatternDirectoryScannerUtilityInput() = default;

    PatternDirectoryScannerUtilityInput(dstd::StringView p,
                                        std::vector<std::string> pats,
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
        std::vector<std::string> pats) {
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
    coro::CoroTask<std::vector<FileEntry>> operator()(
        CoroScope& ctx,
        const PatternDirectoryScannerUtilityInput& input) const {
        DirectoryScannerUtilityInput dir_input{
            input.path.str(), input.recursive, input.populate_size};
        std::vector<FileEntry> all_entries =
            co_await base_scanner_(ctx, dir_input);

        // Step 2: Filter by patterns
        std::vector<FileEntry> matched_entries;

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

    /// Scope-less overload: opens its own CoroScope on the current executor.
    coro::CoroTask<std::vector<FileEntry>> operator()(
        const PatternDirectoryScannerUtilityInput& input) const {
        return with_scope(*this, input);
    }

   private:
    DirectoryScannerUtility base_scanner_;

    /**
     * @brief Check if a path matches any of the given patterns.
     */
    bool matches_any_pattern(dstd::StringView path,
                             const std::vector<std::string>& patterns) const {
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
                         const std::string& pattern) const {
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
