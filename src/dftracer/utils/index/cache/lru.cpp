#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/index/cache/lru.h>
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/utilities/common/serialization/binary_codec.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace dftracer::utils::index::cache {

namespace rdb = index::store;
namespace codec = utilities::common::serialization;

namespace {

constexpr char USAGE_TAG = '\x02';

std::string usage_value(std::uint64_t bytes, std::uint64_t last_used) {
    std::string v;
    codec::put_be64(v, bytes);
    codec::put_be64(v, last_used);
    return v;
}

std::uint64_t now_micros() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

void check_status(const ::rocksdb::Status& st, const char* what) {
    if (!st.ok())
        throw DFTUtilsException(ErrorCode::IO,
                                std::string(what) + ": " + st.ToString());
}

std::string usage_key(std::uint64_t sig) {
    std::string key;
    key.push_back(USAGE_TAG);
    codec::put_be64(key, sig);
    return key;
}

void put_usage(rdb::RocksDatabase& db, rdb::RocksDatabase::Batch& batch,
               std::string_view cf, std::uint64_t sig, std::uint64_t bytes) {
    db.put(batch, cf, usage_key(sig), usage_value(bytes, now_micros()));
}

void touch_usage(rdb::RocksDatabase& db, std::string_view cf,
                 std::uint64_t sig) {
    if (db.is_read_only()) return;
    std::string usage;
    if (!db.get(usage_key(sig), &usage, cf).ok() || usage.size() != USAGE_BYTES)
        return;
    codec::BinaryReader br(usage);
    check_status(
        db.put(usage_key(sig), usage_value(br.be64(), now_micros()), cf),
        "cache usage");
}

void evict_lru(rdb::RocksDatabase& db, std::string_view cf, std::uint64_t keep,
               const std::function<void(rdb::RocksDatabase::Batch&,
                                        std::uint64_t)>& drop) {
    struct Entry {
        std::uint64_t sig, bytes, last_used;
    };
    std::vector<Entry> entries;
    std::uint64_t total = 0;
    auto it = db.new_iterator(cf);
    for (it->Seek(std::string_view(&USAGE_TAG, 1)); it->Valid(); it->Next()) {
        const std::string_view k(it->key().data(), it->key().size());
        if (k.empty() || k[0] != USAGE_TAG) break;
        const std::string_view v(it->value().data(), it->value().size());
        if (k.size() != 9 || v.size() != USAGE_BYTES) continue;
        codec::BinaryReader kr(k.substr(1));
        codec::BinaryReader vr(v);
        const std::uint64_t sig = kr.be64();
        const std::uint64_t bytes = vr.be64();
        entries.push_back({sig, bytes, vr.be64()});
        total += bytes;
    }
    check_status(it->status(), "cache usage scan");
    const std::uint64_t budget = cache_max_bytes();
    if (total <= budget) return;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return a.last_used < b.last_used;
              });
    auto batch = db.begin_batch();
    auto remove = [&](const Entry& e) {
        drop(batch, e.sig);
        db.del(batch, cf, usage_key(e.sig));
        total -= e.bytes;
    };
    for (const auto& e : entries) {
        if (total <= budget) break;
        if (e.sig != keep) remove(e);
    }
    if (total > budget)
        for (const auto& e : entries)
            if (e.sig == keep) remove(e);
    check_status(db.commit_batch(batch), "cache eviction");
}

}  // namespace dftracer::utils::index::cache
