#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/versions.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/duql/syntax/tree.h>
#include <dftracer/utils/index/cache/lookup_store.h>
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <algorithm>
#include <map>
#include <system_error>
#include <vector>

namespace dftracer::utils::index::cache {

namespace rdb = index::store;

namespace {

constexpr std::string_view VALUE_KEY = "\x01";
// The signature that prefixed the key when entries shared one key space; the
// size of an entry still counts it.
constexpr std::uint64_t SIG_BYTES = 8;

}  // namespace

std::string lookup_cache_path(const trace::views::detail::ViewPlan& plan) {
    const std::string dir = cache_dir(plan);
    return dir.empty() ? dir : (fs::path(dir) / "lookups").string();
}

std::optional<std::uint64_t> lookup_signature(
    const trace::views::detail::ViewPlan& plan, std::string_view side_text) {
    if (!plan.record_schema)
        throw DFTUtilsException(ErrorCode::InvalidArgument,
                                "lookup signature: the plan's record schema "
                                "is not resolved");
    std::string sig;
    auto add = [&](std::string_view s) {
        sig.append(s);
        sig.push_back('\0');
    };
    add(side_text);
    add(DFTRACER_UTILS_DUQL_VERSION_STRING);
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

std::shared_ptr<CacheStore> open_lookup_db(const std::string& path,
                                           CacheStore::Mode mode) {
    return CacheStore::open(path, mode);
}

std::optional<std::string> read_lookup(CacheStore& db, std::uint64_t sig) {
    auto value = db.get(sig, VALUE_KEY);
    if (value) db.touch(sig);
    return value;
}

bool lookup_exists(const CacheStore& db, std::uint64_t sig) {
    return db.get(sig, VALUE_KEY).has_value();
}

void persist_lookup(CacheStore& db, std::uint64_t sig, std::string_view value) {
    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back(VALUE_KEY, value);
    db.put(sig, entries, 2 * (1 + SIG_BYTES) + value.size() + USAGE_BYTES);
}

}  // namespace dftracer::utils::index::cache
