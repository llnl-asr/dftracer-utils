// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_PATH_H
#define DFTRACER_UTILS_DSTD_PATH_H

#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::dstd {

/// Separator, root and component rules of one platform family. The host style
/// is HOST_PATH_STYLE. dstd::fs is POSIX only, so the Windows rules are
/// reachable through path_detail on any platform.
enum class PathStyle : std::uint8_t { Posix, Windows };

#if defined(_WIN32)
inline constexpr PathStyle HOST_PATH_STYLE = PathStyle::Windows;
#else
inline constexpr PathStyle HOST_PATH_STYLE = PathStyle::Posix;
#endif

inline constexpr char PATH_SEPARATOR =
    HOST_PATH_STYLE == PathStyle::Windows ? '\\' : '/';

/// The pure path rules. None of them allocates and none touches the file
/// system. Every query matches std::filesystem on POSIX except that a trailing
/// separator is dropped before the last component and a doubled leading
/// separator counts as one root.
namespace path_detail {

constexpr bool is_separator(char c, PathStyle style) noexcept {
    if (c == '/') return true;
    return style == PathStyle::Windows && c == '\\';
}

constexpr char preferred_separator(PathStyle style) noexcept {
    return style == PathStyle::Windows ? '\\' : '/';
}

constexpr bool is_drive_letter(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/// The length of the root, such as `/` on POSIX, or `C:`, `C:\` or
/// `\\server\share` on Windows. Zero for a relative path.
constexpr std::size_t root_length(StringView path, PathStyle style) noexcept {
    const std::size_t size = path.size();
    if (size == 0) return 0;
    if (style == PathStyle::Posix) return path[0] == '/' ? 1 : 0;
    if (is_separator(path[0], style)) {
        if (size < 2 || !is_separator(path[1], style)) return 1;
        std::size_t at = 2;
        while (at < size && !is_separator(path[at], style)) ++at;
        if (at == 2) return 2;
        while (at < size && is_separator(path[at], style)) ++at;
        if (at == size) return at;
        while (at < size && !is_separator(path[at], style)) ++at;
        return at;
    }
    if (size >= 2 && is_drive_letter(path[0]) && path[1] == ':') {
        return size >= 3 && is_separator(path[2], style) ? 3 : 2;
    }
    return 0;
}

constexpr bool is_absolute(StringView path, PathStyle style) noexcept {
    const std::size_t root = root_length(path, style);
    if (root == 0) return false;
    if (style == PathStyle::Posix) return true;
    if (is_separator(path[0], style)) return true;
    return root == 3;
}

constexpr StringView root(StringView path, PathStyle style) noexcept {
    return path.substr(0, root_length(path, style));
}

struct ComponentRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

constexpr ComponentRange filename_range(StringView path,
                                        PathStyle style) noexcept {
    const std::size_t root = root_length(path, style);
    std::size_t end = path.size();
    while (end > root && is_separator(path[end - 1], style)) --end;
    std::size_t begin = end;
    while (begin > root && !is_separator(path[begin - 1], style)) --begin;
    return {begin, end};
}

constexpr StringView filename(StringView path, PathStyle style) noexcept {
    const ComponentRange range = filename_range(path, style);
    return path.substr(range.begin, range.end - range.begin);
}

/// Everything before the last component, with the separators that followed it
/// dropped. A path with no component keeps its root, so parent("/") is "/".
constexpr StringView parent(StringView path, PathStyle style) noexcept {
    const std::size_t root = root_length(path, style);
    std::size_t end = filename_range(path, style).begin;
    while (end > root && is_separator(path[end - 1], style)) --end;
    return path.substr(0, end);
}

/// The substring from the last '.' of the filename, or empty. A filename of
/// "." or ".." has no extension, and neither has a dotfile such as ".bashrc".
constexpr StringView extension(StringView path, PathStyle style) noexcept {
    const ComponentRange range = filename_range(path, style);
    const std::size_t size = range.end - range.begin;
    if (size == 0) return {};
    if (size <= 2) {
        bool only_dots = true;
        for (std::size_t at = range.begin; at < range.end; ++at) {
            if (path[at] != '.') {
                only_dots = false;
                break;
            }
        }
        if (only_dots) return {};
    }
    for (std::size_t at = range.end; at > range.begin; --at) {
        if (path[at - 1] != '.') continue;
        if (at - 1 == range.begin) return {};
        return path.substr(at - 1, range.end - at + 1);
    }
    return {};
}

constexpr StringView stem(StringView path, PathStyle style) noexcept {
    const ComponentRange range = filename_range(path, style);
    const StringView ext = extension(path, style);
    return path.substr(range.begin, range.end - range.begin - ext.size());
}

}  // namespace path_detail

/// A borrowed path. Every query returns a view of the bytes it was built from.
class PathView {
   public:
    constexpr PathView() noexcept = default;
    constexpr PathView(StringView path) noexcept : path_(path) {}

    constexpr StringView str() const noexcept { return path_; }
    constexpr operator StringView() const noexcept { return path_; }
    constexpr bool empty() const noexcept { return path_.empty(); }

    constexpr StringView root(
        PathStyle style = HOST_PATH_STYLE) const noexcept {
        return path_detail::root(path_, style);
    }
    constexpr bool is_absolute(
        PathStyle style = HOST_PATH_STYLE) const noexcept {
        return path_detail::is_absolute(path_, style);
    }
    constexpr PathView parent(
        PathStyle style = HOST_PATH_STYLE) const noexcept {
        return path_detail::parent(path_, style);
    }
    constexpr StringView filename(
        PathStyle style = HOST_PATH_STYLE) const noexcept {
        return path_detail::filename(path_, style);
    }
    constexpr StringView stem(
        PathStyle style = HOST_PATH_STYLE) const noexcept {
        return path_detail::stem(path_, style);
    }
    constexpr StringView extension(
        PathStyle style = HOST_PATH_STYLE) const noexcept {
        return path_detail::extension(path_, style);
    }

   private:
    StringView path_;
};

/// An owned path. The bytes live in a dstd::String, so c_str() is valid until
/// the next change.
class Path {
   public:
    Path() = default;
    Path(StringView path) : path_(path) {}
    Path(const Path&) = default;
    Path(Path&&) noexcept = default;
    Path& operator=(const Path&) = default;
    Path& operator=(Path&&) noexcept = default;
    ~Path() = default;

    PathView view() const noexcept { return PathView(StringView(path_)); }
    StringView str() const noexcept { return path_; }
    const char* c_str() const noexcept { return path_.c_str(); }
    bool empty() const noexcept { return path_.empty(); }

    operator CStringView() const noexcept {
        return CStringView::unchecked(path_.c_str(), path_.size());
    }

    /// An absolute rhs replaces the path, an empty rhs adds nothing.
    Path& operator/=(StringView rhs);
    /// A replacement that does not start with '.' gets one prepended, and an
    /// empty one drops the extension.
    Path& replace_extension(StringView replacement);

   private:
    String path_;
};

/// Joins with the host separator. An absolute child replaces the base.
inline Path operator/(PathView base, StringView child) {
    Path joined(base.str());
    joined /= child;
    return joined;
}

/// Lexical only: "." is dropped, ".." cancels the component before it, a ".."
/// that has nothing before it is kept unless the path is absolute, and the root
/// is kept. No syscall, no symlink is resolved.
Path normalize(PathView path, PathStyle style = HOST_PATH_STYLE);

/// Lexical only: the components of path below the longest run it shares with
/// base, with one ".." per remaining base component. Empty when the roots
/// differ, "." when the paths are equal.
Path relative(PathView path, PathView base, PathStyle style = HOST_PATH_STYLE);

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_PATH_H
