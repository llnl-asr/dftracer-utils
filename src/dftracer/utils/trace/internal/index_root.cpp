#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/trace/internal/index_root.h>

#include <mutex>

namespace dftracer::utils::trace::internal {

std::shared_ptr<IndexRoot> index_root(const std::string& index_path) {
    static std::mutex guard;
    static StringViewMap<std::shared_ptr<IndexRoot>> roots;
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(index_path, ec);
    const std::lock_guard<std::mutex> lock(guard);
    auto& r = roots[ec ? index_path : canonical.string()];
    if (!r) r = std::make_shared<IndexRoot>();
    return r;
}

}  // namespace dftracer::utils::trace::internal
