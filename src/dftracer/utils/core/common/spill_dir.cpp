#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/filesystem_info.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/scratch.h>
#include <dftracer/utils/core/common/spill_dir.h>
#include <dftracer/utils/core/env.h>
#include <unistd.h>

#include <system_error>

namespace dftracer::utils {

Result<std::string> spill_dir() {
    const auto env = Env::get(constants::SPILL_DIR_ENV);
    if (!env || env->empty()) {
        const std::string& local = local_spill_root();
        if (!local.empty()) return local;
    }
    std::error_code ec;
    fs::path dir = env && !env->empty() ? fs::path(std::string(*env))
                                        : fs::temp_directory_path(ec);
    if ((!env || env->empty()) && !ec &&
        is_memory_filesystem(filesystem_kind(dir.string()))) {
        static const bool warned = [&] {
            DFTRACER_UTILS_LOG_WARN(
                "spill directory %s is RAM-backed, so spilled data counts "
                "against the memory limit; set %s to a directory on a local "
                "disk",
                dir.c_str(), constants::SPILL_DIR_ENV);
            return true;
        }();
        (void)warned;
    }
    if (!ec) fs::create_directories(dir, ec);
    if (!ec && ::access(dir.c_str(), W_OK | X_OK) != 0)
        ec = std::make_error_code(std::errc::permission_denied);
    if (ec)
        return make_error(
            ErrorCode::IO,
            str_cat("spill: cannot use spill directory '", dir.string(), "' (",
                    ec.message(), "); set ", constants::SPILL_DIR_ENV,
                    " to a writable directory"));
    return dir.string();
}

}  // namespace dftracer::utils
