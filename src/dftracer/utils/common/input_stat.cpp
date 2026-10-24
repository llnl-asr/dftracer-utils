#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/memory.h>

#include <mutex>
#include <string_view>

namespace dftracer::utils {

namespace {

// The published cache. Held by shared_ptr so a lookup that is under way when
// its owner ends still has a live cache; empty outside any scope.
std::mutex g_published_mutex;
dstd::SharedPtr<InputStatCache> g_published;

dstd::SharedPtr<InputStatCache> published() {
    std::lock_guard<std::mutex> lock(g_published_mutex);
    return g_published;
}

}  // namespace

bool InputStatCache::find(const std::string& path, InputFileStat& out) const {
    std::shared_lock lock(mutex_);
    if (!map_) return false;
    const auto it = map_->find(std::string_view(path));
    if (it == map_->end()) return false;
    out = it->second;
    return true;
}

void InputStatCache::insert(const std::string& path, const InputFileStat& st) {
    std::unique_lock lock(mutex_);
    if (!map_)
        map_ = dstd::make_unique<dstd::HashMap<dstd::String, InputFileStat>>();
    map_->emplace(dstd::String(path.data(), path.size()), st);
}

InputStatScope::InputStatScope() {
    std::lock_guard<std::mutex> lock(g_published_mutex);
    if (g_published) return;
    cache_ = dstd::make_shared<InputStatCache>();
    g_published = cache_;
}

InputStatScope::~InputStatScope() {
    if (!cache_) return;
    std::lock_guard<std::mutex> lock(g_published_mutex);
    if (g_published == cache_) g_published.reset();
}

bool stat_input(const std::string& path, InputFileStat& out) {
    const dstd::SharedPtr<InputStatCache> cache = published();
    if (!cache) return stat_path(path.c_str(), out);
    if (cache->find(path, out)) return true;
    if (!stat_path(path.c_str(), out)) return false;
    cache->insert(path, out);
    return true;
}

}  // namespace dftracer::utils
