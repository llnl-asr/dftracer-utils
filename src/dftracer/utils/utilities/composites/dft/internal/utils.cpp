#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>

#include <functional>
#include <sstream>
#include <unordered_set>

namespace dftracer::utils::utilities::composites::dft::internal {

std::string determine_index_path(const std::string& file_path,
                                 const std::string& index_dir) {
    fs::path data_path(file_path);
    std::string base_name =
        data_path.filename().string() + constants::indexer::EXTENSION;

    if (!index_dir.empty()) {
        return (fs::path(index_dir) / base_name).string();
    }

    return (data_path.parent_path() / base_name).string();
}

std::string determine_provenance_index_path(const std::string& data_path,
                                            const std::string& index_dir) {
    fs::path path(data_path);
    std::string base_name = path.filename().string() + ".pidx";

    if (!index_dir.empty()) {
        return (fs::path(index_dir) / base_name).string();
    }

    return (path.parent_path() / base_name).string();
}

bool is_data_transfer_op(std::string_view cat, std::string_view name) {
    if (cat != "POSIX" && cat != "STDIO") return false;
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
