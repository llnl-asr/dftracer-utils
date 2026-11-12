// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/internal/utils.h>

namespace dftracer::utils::trace::internal {

namespace {

bool is_input_file_name(dstd::StringView path) {
    const dstd::StringView name = dstd::PathView(path).filename();
    for (const char* pattern :
         dftracer::utils::common::constants::indexer::INPUT_FILE_PATTERNS) {
        if (dstd::fs::match_name(pattern, name)) return true;
    }
    return false;
}

bool is_directory_input(dstd::CStringView path) {
    dftracer::utils::common::InputFileStat st;
    return dftracer::utils::common::stat_input(path, st) && st.is_directory;
}

}  // namespace

dstd::String determine_index_path(dstd::CStringView cpath,
                                  dstd::StringView index_dir) {
    const dstd::StringView path = cpath.view();
    // A trace file name needs no stat to tell it from a directory.
    dstd::StringView root = index_dir;
    if (index_dir.empty()) {
        if (!is_input_file_name(path) && is_directory_input(cpath)) {
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
    return dftracer::utils::trace::index::normalize_index_root(root);
}

dstd::StringView to_lower_ascii(dstd::StringView s, dstd::String& storage) {
    bool has_upper = false;
    for (char c : s) {
        if (dstd::is_ascii_upper(c)) {
            has_upper = true;
            break;
        }
    }
    if (!has_upper) return s;
    storage.assign(s);
    ::dftracer::utils::common::detail::lower_ascii(storage);
    return storage;
}

bool is_data_transfer_op(dstd::StringView cat, dstd::StringView name) {
    if (!dstd::ascii_iequals(cat, "posix") &&
        !dstd::ascii_iequals(cat, "stdio")) {
        return false;
    }
    auto eq = [](dstd::StringView a, dstd::StringView b) { return a == b; };
    for (auto op : posix_ops::READ)
        if (eq(op, name)) return true;
    for (auto op : posix_ops::WRITE)
        if (eq(op, name)) return true;
    return false;
}

}  // namespace dftracer::utils::trace::internal
