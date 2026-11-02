// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>

namespace dftracer::utils::dstd {

namespace {

void append_component(String& out, StringView component, PathStyle style) {
    if (!out.empty() && !path_detail::is_separator(out.back(), style)) {
        out.push_back(path_detail::preferred_separator(style));
    }
    out.append(component);
}

StringView tail_component(StringView path, std::size_t root, PathStyle style) {
    std::size_t end = path.size();
    while (end > root && path_detail::is_separator(path[end - 1], style)) --end;
    std::size_t begin = end;
    while (begin > root && !path_detail::is_separator(path[begin - 1], style))
        --begin;
    return path.substr(begin, end - begin);
}

Vector<StringView> components(StringView path, std::size_t root,
                              PathStyle style) {
    Vector<StringView> out;
    std::size_t at = root;
    while (at < path.size()) {
        while (at < path.size() && path_detail::is_separator(path[at], style)) {
            ++at;
        }
        const std::size_t begin = at;
        while (at < path.size() &&
               !path_detail::is_separator(path[at], style)) {
            ++at;
        }
        if (at > begin) out.push_back(path.substr(begin, at - begin));
    }
    return out;
}

}  // namespace

Path& Path::operator/=(StringView rhs) {
    if (rhs.empty()) return *this;
    if (path_detail::is_absolute(rhs, HOST_PATH_STYLE) || path_.empty()) {
        path_.assign(rhs);
        return *this;
    }
    if (!path_detail::is_separator(path_.back(), HOST_PATH_STYLE)) {
        path_.push_back(PATH_SEPARATOR);
    }
    path_.append(rhs);
    return *this;
}

Path& Path::replace_extension(StringView replacement) {
    const path_detail::ComponentRange range =
        path_detail::filename_range(path_, HOST_PATH_STYLE);
    const std::size_t keep =
        range.end - path_detail::extension(path_, HOST_PATH_STYLE).size();
    path_.resize(keep);
    if (replacement.empty()) return *this;
    if (replacement[0] != '.') path_.push_back('.');
    path_.append(replacement);
    return *this;
}

Path normalize(PathView path, PathStyle style) {
    const StringView bytes = path.str();
    const std::size_t root = path_detail::root_length(bytes, style);
    String out(bytes.substr(0, root));
    for (const StringView component : components(bytes, root, style)) {
        if (component == ".") continue;
        if (component == "..") {
            const StringView tail = tail_component(out, root, style);
            if (!tail.empty() && tail != "..") {
                out.resize(out.size() - tail.size());
                while (out.size() > root &&
                       path_detail::is_separator(out.back(), style)) {
                    out.pop_back();
                }
            } else if (root == 0) {
                append_component(out, component, style);
            }
            continue;
        }
        append_component(out, component, style);
    }
    if (out.empty() && !bytes.empty()) out.push_back('.');
    return Path(out);
}

Path relative(PathView path, PathView base, PathStyle style) {
    const StringView bytes = path.str();
    const StringView base_bytes = base.str();
    const StringView root = path_detail::root(bytes, style);
    if (root != path_detail::root(base_bytes, style)) return Path();
    const Vector<StringView> leaves = components(bytes, root.size(), style);
    const Vector<StringView> bases = components(base_bytes, root.size(), style);
    std::size_t common = 0;
    while (common < leaves.size() && common < bases.size() &&
           leaves[common] == bases[common]) {
        ++common;
    }
    String out;
    for (std::size_t at = common; at < bases.size(); ++at) {
        append_component(out, StringView(".."), style);
    }
    for (std::size_t at = common; at < leaves.size(); ++at) {
        append_component(out, leaves[at], style);
    }
    if (out.empty()) out.push_back('.');
    return Path(out);
}

}  // namespace dftracer::utils::dstd
