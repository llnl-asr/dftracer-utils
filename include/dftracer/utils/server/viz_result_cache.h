#ifndef DFTRACER_UTILS_SERVER_VIZ_RESULT_CACHE_H
#define DFTRACER_UTILS_SERVER_VIZ_RESULT_CACHE_H

#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace dftracer::utils::server {

// Thread-safe, byte-bounded LRU cache of serialized viz responses. Trace data
// is immutable for a server's lifetime, so a hit is always valid; entries are
// evicted only to respect the byte budget.
class VizResultCache {
   public:
    explicit VizResultCache(std::size_t max_bytes) : max_bytes_(max_bytes) {}

    std::optional<std::string> get(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        order_.splice(order_.begin(), order_, it->second.pos);
        return it->second.body;
    }

    void put(const std::string& key, std::string body) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            bytes_ -= it->second.body.size();
            it->second.body = std::move(body);
            bytes_ += it->second.body.size();
            order_.splice(order_.begin(), order_, it->second.pos);
            evict();
            return;
        }
        if (body.size() > max_bytes_) return;  // would never fit
        order_.push_front(key);
        Entry e;
        e.body = std::move(body);
        e.pos = order_.begin();
        bytes_ += e.body.size();
        map_.emplace(key, std::move(e));
        evict();
    }

   private:
    struct Entry {
        std::string body;
        std::list<std::string>::iterator pos;
    };

    void evict() {  // front = MRU, back = LRU
        while (bytes_ > max_bytes_ && !order_.empty()) {
            auto it = map_.find(order_.back());
            if (it != map_.end()) {
                bytes_ -= it->second.body.size();
                map_.erase(it);
            }
            order_.pop_back();
        }
    }

    std::size_t max_bytes_;
    std::size_t bytes_ = 0;
    std::list<std::string> order_;
    std::unordered_map<std::string, Entry> map_;
    std::mutex mu_;
};

}  // namespace dftracer::utils::server

#endif  // DFTRACER_UTILS_SERVER_VIZ_RESULT_CACHE_H
