// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/versions.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/syntax/tree.h>
#include <dftracer/utils/index/cache/lookup_store.h>
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/trace/views/view_plan.h>

#include <algorithm>
#include <map>
#include <system_error>
#include <vector>

namespace dftracer::utils::index::cache {

namespace rdb = index::store;

namespace {

constexpr dstd::StringView VALUE_KEY = "\x01";
// The size of an entry counts the signature that names it.
constexpr std::uint64_t SIG_BYTES = 8;

}  // namespace

std::string lookup_cache_path(const trace::views::detail::ViewPlan& plan) {
    const std::string dir = cache_dir(plan);
    return dir.empty() ? dir : index::lookups_dir(dir);
}

dstd::Optional<std::uint64_t> lookup_signature(
    const trace::views::detail::ViewPlan& plan, dstd::StringView side_text) {
    if (!plan.record_schema)
        throw common::DFTUtilsException(
            common::ErrorCode::InvalidArgument,
            "lookup signature: the plan's record schema "
            "is not resolved");
    std::string sig;
    auto add = [&](dstd::StringView s) {
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
    std::map<std::string, dstd::Optional<index::Snapshot>, std::less<>> snaps;
    for (const auto* f : files) {
        if (f->index_path.empty()) return dstd::nullopt;
        auto [it, fresh] = snaps.try_emplace(f->index_path);
        if (fresh) {
            try {
                it->second.emplace(
                    common::unwrap(index::plan::open_snapshot(f->index_path)));
            } catch (const common::DFTUtilsException&) {
                return dstd::nullopt;
            }
        }
        const index::Snapshot& snapshot = *it->second;
        const std::string logical =
            rdb::internal::get_logical_path(f->file_path);
        common::InputFileStat now;
        dftracer::utils::common::stat_input(f->file_path, now);
        const SourceStat now_stat{static_cast<std::uint64_t>(now.mtime),
                                  now.size};
        if (snapshot.freshness(logical, now_stat) != index::Freshness::Current)
            return dstd::nullopt;
        const auto file = snapshot.file(logical);
        if (!file) return dstd::nullopt;
        const auto hash = index::file_hash(snapshot, *file);
        if (!hash) return dstd::nullopt;
        add(logical);
        add(std::to_string(*hash));
        add(std::to_string(file->stat().mtime));
        add(std::to_string(file->stat().size));
    }
    return dftracer::utils::dstd::hash::fnv1a_hash(sig);
}

dstd::SharedPtr<CacheStore> open_lookup_db(const std::string& path,
                                           CacheStore::Mode mode) {
    return CacheStore::open(path, mode);
}

dstd::Optional<std::string> read_lookup(CacheStore& db, std::uint64_t sig) {
    auto value = db.get(sig, VALUE_KEY);
    if (value) db.touch(sig);
    return value;
}

bool lookup_exists(const CacheStore& db, std::uint64_t sig) {
    return db.get(sig, VALUE_KEY).has_value();
}

void persist_lookup(CacheStore& db, std::uint64_t sig, dstd::StringView value) {
    std::vector<dstd::Pair<std::string, std::string>> entries;
    entries.emplace_back(VALUE_KEY, value);
    db.put(sig, entries, 2 * (1 + SIG_BYTES) + value.size() + USAGE_BYTES);
}

}  // namespace dftracer::utils::index::cache
