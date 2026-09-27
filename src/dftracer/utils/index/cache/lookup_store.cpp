#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/duql/syntax/tree.h>
#include <dftracer/utils/index/cache/lookup_store.h>
#include <dftracer/utils/index/cache/lru.h>
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/utilities/common/serialization/binary_codec.h>

#include <algorithm>
#include <map>
#include <system_error>
#include <vector>

namespace dftracer::utils::index::cache {

namespace rdb = index::store;
namespace codec = utilities::common::serialization;

namespace {

std::string value_key(std::uint64_t sig) {
    std::string key;
    key.push_back('\x01');
    codec::put_be64(key, sig);
    return key;
}

}  // namespace

std::string lookup_cache_path(const trace::views::detail::ViewPlan& plan) {
    const std::string dir = cache_dir(plan);
    return dir.empty() ? dir : (fs::path(dir) / "lookups").string();
}

std::optional<std::uint64_t> lookup_signature(
    const trace::views::detail::ViewPlan& plan, std::string_view side_text) {
    if (!plan.record_schema)
        throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                "lookup signature: the plan's record schema "
                                "is not resolved");
    std::string sig;
    auto add = [&](std::string_view s) {
        sig.append(s);
        sig.push_back('\0');
    };
    add(side_text);
    add(std::to_string(duql::syntax::DUQL_VERSION));
    add(std::to_string(plan.record_schema->params_hash()));
    std::vector<const trace::views::ViewFile*> files;
    files.reserve(plan.files.size());
    for (const auto& f : plan.files) files.push_back(&f);
    std::sort(files.begin(), files.end(),
              [](auto* a, auto* b) { return a->file_path < b->file_path; });
    std::map<std::string, std::optional<rdb::IndexDatabase>, std::less<>> dbs;
    for (const auto* f : files) {
        if (f->index_path.empty()) return std::nullopt;
        auto [it, fresh] = dbs.try_emplace(f->index_path);
        if (fresh) {
            try {
                it->second.emplace(f->index_path, rdb::IndexOpenMode::ReadOnly);
            } catch (const DFTUtilsException&) {
                return std::nullopt;
            }
        }
        const rdb::IndexDatabase& idx = *it->second;
        if (idx.check_freshness(f->file_path) !=
            rdb::IndexDatabase::Freshness::Fresh)
            return std::nullopt;
        const std::string logical =
            rdb::internal::get_logical_path(f->file_path);
        const auto hash = idx.get_file_hash(logical);
        const auto stat = idx.get_file_stat(logical);
        if (!hash || !stat) return std::nullopt;
        add(logical);
        add(std::to_string(*hash));
        add(std::to_string(stat->mtime));
        add(std::to_string(stat->size));
    }
    return dftracer::utils::hash::fnv1a_hash(sig);
}

std::shared_ptr<rdb::RocksDatabase> open_lookup_db(
    const std::string& path, rdb::RocksDatabase::OpenMode mode) {
    try {
        if (mode == rdb::RocksDatabase::OpenMode::ReadWrite)
            fs::create_directories(path);
        return rdb::RocksDBManager::instance().get_or_open(path, mode);
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::optional<std::string> read_lookup(rdb::RocksDatabase& db,
                                       std::uint64_t sig) {
    std::string value;
    const auto st = db.get(value_key(sig), &value, rdb::cf::ROLLUP);
    if (st.IsNotFound()) return std::nullopt;
    check_status(st, "lookup read");
    touch_usage(db, rdb::cf::ROLLUP, sig);
    return value;
}

bool lookup_exists(const rdb::RocksDatabase& db, std::uint64_t sig) {
    std::string value;
    return db.get(value_key(sig), &value, rdb::cf::ROLLUP).ok();
}

void persist_lookup(rdb::RocksDatabase& db, std::uint64_t sig,
                    std::string_view value) {
    const std::string key = value_key(sig);
    auto batch = db.begin_batch();
    db.put(batch, rdb::cf::ROLLUP, key, value);
    put_usage(db, batch, rdb::cf::ROLLUP, sig,
              key.size() + value.size() + 9 + USAGE_BYTES);
    check_status(db.commit_batch(batch), "lookup persist");
    evict_lru(db, rdb::cf::ROLLUP, sig,
              [&db](rdb::RocksDatabase::Batch& b, std::uint64_t s) {
                  db.del(b, rdb::cf::ROLLUP, value_key(s));
              });
}

}  // namespace dftracer::utils::index::cache
