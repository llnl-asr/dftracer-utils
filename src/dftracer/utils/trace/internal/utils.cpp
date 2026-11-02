// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

namespace dftracer::utils::trace::internal {

namespace {

bool is_directory_input(dstd::CStringView path) {
    dftracer::utils::common::InputFileStat st;
    return dftracer::utils::common::stat_input(path, st) && st.is_directory;
}

}  // namespace

std::string determine_index_path(dstd::CStringView cpath,
                                 dstd::StringView index_dir) {
    const dstd::StringView path = cpath.view();
    // A trace file name needs no stat to tell it from a directory.
    dstd::StringView root = index_dir;
    if (index_dir.empty()) {
        if (!utilities::filesystem::is_trace_file(path) &&
            is_directory_input(cpath)) {
            root = path;
        } else if (!path.empty() && path.back() == '/') {
            // The parent of "a/b/" is "a/b", where PathView would give "a".
            root = path;
            while (root.size() > 1 && root.back() == '/') {
                root.remove_suffix(1);
            }
        } else {
            root = dstd::PathView(path).parent().str();
        }
    }
    return dftracer::utils::index::store::internal::normalize_index_root(root);
}

std::string_view to_lower_ascii(std::string_view s, std::string& storage) {
    bool has_upper = false;
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') {
            has_upper = true;
            break;
        }
    }
    if (!has_upper) return s;
    storage.assign(s);
    ::dftracer::utils::common::detail::lower_ascii(storage);
    return storage;
}

bool is_data_transfer_op(std::string_view cat, std::string_view name) {
    if (!::dftracer::utils::common::detail::iequals(cat, "posix") &&
        !::dftracer::utils::common::detail::iequals(cat, "stdio")) {
        return false;
    }
    auto eq = [](std::string_view a, std::string_view b) { return a == b; };
    for (auto op : posix_ops::READ)
        if (eq(op, name)) return true;
    for (auto op : posix_ops::WRITE)
        if (eq(op, name)) return true;
    return false;
}

}  // namespace dftracer::utils::trace::internal
