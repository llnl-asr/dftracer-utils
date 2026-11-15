// SPDX-License-Identifier: MIT
#include <dftracer/utils/domains/server/trace_index.h>
#include <dftracer/utils/domains/server/viz_result_cache.h>

#include <utility>

namespace dftracer::utils::domains::server {

VizResultCache::VizResultCache(std::size_t max_bytes, TraceIndex* index)
    : index_(index), max_bytes_(max_bytes) {}

dstd::Optional<dstd::String> VizResultCache::get(const dstd::String& key) {
    const dstd::String keys[] = {key};
    auto got = get_many(dstd::Span<const dstd::String>(keys, 1));
    return std::move(got.front());
}

dstd::Vector<dstd::Optional<dstd::String>> VizResultCache::get_many(
    dstd::Span<const dstd::String> keys) {
    dstd::Vector<dstd::Optional<dstd::String>> out(keys.size());
    dstd::Vector<dstd::String> missed;
    dstd::Vector<std::size_t> missed_at;
    missed.reserve(keys.size());
    missed_at.reserve(keys.size());
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            auto it = map_.find(keys[i]);
            if (it == map_.end()) {
                missed.emplace_back(keys[i]);
                missed_at.push_back(i);
                continue;
            }
            order_.splice(order_.begin(), order_, it->second.pos);
            out[i] = it->second.body;
        }
    }
    if (missed.empty() || index_ == nullptr) return out;
    const auto stored = index_->load_viz_results(missed);
    bool computed = false;
    for (std::size_t m = 0; m < stored.size(); ++m) {
        if (!stored[m]) {
            computed = true;
            continue;
        }
        const dstd::String& body = *stored[m];
        put_memory(missed[m], body);
        out[missed_at[m]] = body;
    }
    if (computed) index_->note_viz_result_compute();
    return out;
}

void VizResultCache::put(const dstd::String& key, dstd::String body) {
    put_memory(key, body);
    if (index_ == nullptr) return;
    index_->store_viz_result(dstd::String(key), std::move(body));
}

void VizResultCache::put_memory(const dstd::String& key, dstd::String body) {
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

void VizResultCache::evict() {
    while (bytes_ > max_bytes_ && !order_.empty()) {
        auto it = map_.find(order_.back());
        if (it != map_.end()) {
            bytes_ -= it->second.body.size();
            map_.erase(it);
        }
        order_.pop_back();
    }
}

}  // namespace dftracer::utils::domains::server
