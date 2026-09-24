#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/build/index_write_lock.h>

#include <memory>

namespace dftracer::utils::index::build {

std::mutex& index_write_mutex(const std::string& index_path) {
    static std::mutex registry_mtx;
    static StringViewMap<std::unique_ptr<std::mutex>> mutexes;
    std::lock_guard<std::mutex> lk(registry_mtx);
    auto& slot = mutexes[index_path];
    if (!slot) slot = std::make_unique<std::mutex>();
    return *slot;
}

}  // namespace dftracer::utils::index::build
