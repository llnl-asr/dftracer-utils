#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_PATTERN_DIRECTORY_SCANNER_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_PATTERN_DIRECTORY_SCANNER_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>

#include <algorithm>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::filesystem {

/**
 * @brief Input for pattern-based directory scanning.
 */
struct PatternDirectoryScannerUtilityInput {
    std::string path;
    bool recursive = false;
    std::vector<std::string> patterns;  // e.g., {".pfw", ".pfw.gz", "*.txt"}

    PatternDirectoryScannerUtilityInput() = default;

    PatternDirectoryScannerUtilityInput(std::string p,
                                        std::vector<std::string> pats,
                                        bool rec = false)
        : path(std::move(p)), recursive(rec), patterns(std::move(pats)) {}

    static PatternDirectoryScannerUtilityInput from_path(std::string p) {
        PatternDirectoryScannerUtilityInput input;
        input.path = std::move(p);
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
 * PatternDirectoryScanner scanner;
 * auto files = scanner.process(
 *     PatternDirectory{"/path/to/dir", {".pfw", ".pfw.gz"}}
 * );
 * @endcode
 */
class PatternDirectoryScannerUtility
    : public utilities::Utility<PatternDirectoryScannerUtilityInput,
                                std::vector<FileEntry>,
                                utilities::tags::Parallelizable> {
   private:
    DirectoryScannerUtility base_scanner_;

   public:
    PatternDirectoryScannerUtility() = default;

    /**
     * @brief Scan directory and filter by patterns.
     *
     * @param input Directory path, patterns, and recursive flag
     * @return Vector of file entries matching patterns
     */
    std::vector<FileEntry> process(
        const PatternDirectoryScannerUtilityInput& input) override {
        // Step 1: Use base DirectoryScanner
        DirectoryScannerUtilityInput dir_input{input.path, input.recursive};
        std::vector<FileEntry> all_entries = base_scanner_.process(dir_input);

        // Step 2: Filter by patterns
        std::vector<FileEntry> matched_entries;

        for (const auto& entry : all_entries) {
            // Only consider regular files
            if (!entry.is_regular_file) continue;

            std::string path = entry.path.string();

            // Check if file matches any pattern
            if (matches_any_pattern(path, input.patterns)) {
                matched_entries.push_back(entry);
            }
        }

        return matched_entries;
    }

   private:
    /**
     * @brief Check if a path matches any of the given patterns.
     */
    bool matches_any_pattern(const std::string& path,
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
    bool matches_pattern(const std::string& path,
                         const std::string& pattern) const {
        if (pattern.empty()) {
            return true;
        }

        // Simple glob: "*.ext" -> matches any file ending with ".ext"
        if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
            std::string extension = pattern.substr(1);  // Remove '*'
            return ends_with(path, extension);
        }

        // Direct extension/suffix match
        if (pattern[0] == '.') {
            return ends_with(path, pattern);
        }

        // Exact filename match
        fs::path p(path);
        return p.filename().string() == pattern;
    }

    /**
     * @brief Check if string ends with suffix.
     */
    bool ends_with(const std::string& str, const std::string& suffix) const {
        if (suffix.size() > str.size()) {
            return false;
        }
        return str.compare(str.size() - suffix.size(), suffix.size(), suffix) ==
               0;
    }
};

}  // namespace dftracer::utils::utilities::filesystem

#endif  // DFTRACER_UTILS_UTILITIES_FILESYSTEM_PATTERN_DIRECTORY_SCANNER_H
