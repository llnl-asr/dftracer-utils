#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>

#include <cctype>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace dftracer::utils::utilities::composites::dft::internal {

std::string determine_index_path(const std::string& path,
                                 const std::string& index_dir) {
    fs::path data_path(path);
    fs::path root = index_dir.empty() ? (fs::is_directory(data_path)
                                             ? data_path
                                             : data_path.parent_path())
                                      : fs::path(index_dir);
    return indexer::internal::normalize_index_root(root.string());
}

std::string determine_provenance_index_path(const std::string& data_path,
                                            const std::string& index_dir) {
    return determine_index_path(data_path, index_dir);
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
    for (auto& c : storage) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return storage;
}

bool ascii_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool is_data_transfer_op(std::string_view cat, std::string_view name) {
    if (!ascii_iequals(cat, "posix") && !ascii_iequals(cat, "stdio")) {
        return false;
    }
    static const std::unordered_set<std::string_view> OPS = {
        "read",     "write",    "pread",           "pwrite",  "pread64",
        "pwrite64", "readv",    "writev",          "preadv",  "pwritev",
        "preadv2",  "pwritev2", "fread",           "fwrite",  "recv",
        "send",     "recvfrom", "sendto",          "recvmsg", "sendmsg",
        "splice",   "sendfile", "copy_file_range",
    };
    return OPS.count(name) > 0;
}

}  // namespace dftracer::utils::utilities::composites::dft::internal
