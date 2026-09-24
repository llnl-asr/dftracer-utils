#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/trace/aggregators/aggregation_config.h>
#include <dftracer/utils/trace/aggregators/aggregation_intern.h>
#include <dftracer/utils/trace/aggregators/aggregation_serialization.h>
#include <dftracer/utils/trace/indexing/resolve_and_build.h>
#include <dftracer/utils/trace/indexing/shard_manifest.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/sharded_view.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::views {

ShardedView ShardedView::from_shard_dirs(std::vector<std::string> shard_dirs) {
    return ShardedView(std::move(shard_dirs));
}

ShardedView ShardedView::from_manifest(const std::string& root) {
    auto manifest = read_shard_manifest(root);
    if (!manifest) {
        throw DFTUtilsException(ErrorCode::IO,
                                "no shard manifest under " + root + " (" +
                                    std::string(SHARD_MANIFEST_FILENAME) + ")");
    }
    if (manifest->schema_version !=
        utilities::indexer::IndexDatabase::SCHEMA_VERSION) {
        throw DFTUtilsException(
            ErrorCode::INVALID_ARGUMENT,
            "shard manifest schema version " +
                std::to_string(manifest->schema_version) +
                " does not match this build's " +
                std::to_string(
                    utilities::indexer::IndexDatabase::SCHEMA_VERSION));
    }

    std::vector<std::string> dirs;
    dirs.reserve(manifest->shards.size());
    for (const auto& shard : manifest->shards) {
        if (shard.empty()) continue;
        dirs.push_back((fs::path(root) / shard.path).string());
    }
    return ShardedView(std::move(dirs));
}

std::vector<ViewFile> ShardedView::shard_view_files(
    const std::string& shard_dir) const {
    // The registry stores each file's full canonical path, so it locates the
    // trace directly - no reconstruction, and the index may live apart from it.
    utilities::indexer::IndexDatabase db(
        shard_dir, utilities::indexer::IndexOpenMode::ReadOnly);
    std::vector<ViewFile> files;
    for (const auto& [path, file_id] : db.query_all_file_info_ids()) {
        ViewFile vf;
        vf.file_path = path;
        vf.index_path = shard_dir;
        files.push_back(std::move(vf));
    }
    return files;
}

coro::CoroTask<dftracer::utils::dataframe::DataFrame> ShardedView::aggregate(
    Configure configure) const {
    std::vector<std::string> partials;
    partials.reserve(shard_dirs_.size());
    for (const auto& dir : shard_dirs_) {
        std::vector<ViewFile> files = shard_view_files(dir);
        if (files.empty()) continue;
        View shard = configure(View::from_files(std::move(files)));
        partials.push_back(co_await shard.aggregate_partial().collect());
    }

    std::vector<std::string_view> pv(partials.begin(), partials.end());
    co_return configure(View::from_files({})).merge_partials(pv);
}

coro::CoroTask<ExportStats> ShardedView::aggregate_counters(
    Configure configure, ExportSink& sink) const {
    std::vector<std::string> partials;
    partials.reserve(shard_dirs_.size());
    for (const auto& dir : shard_dirs_) {
        std::vector<ViewFile> files = shard_view_files(dir);
        if (files.empty()) continue;
        View shard = configure(View::from_files(std::move(files)));
        partials.push_back(co_await shard.aggregate_partial().collect());
    }

    std::vector<std::string_view> pv(partials.begin(), partials.end());
    co_return configure(View::from_files({})).merge_counter_partials(pv, sink);
}

void write_shard_set(const std::string& root,
                     const std::vector<std::string>& shard_dirs) {
    IndexShardManifest manifest;
    manifest.schema_version = utilities::indexer::IndexDatabase::SCHEMA_VERSION;
    manifest.shards.reserve(shard_dirs.size());
    for (const auto& dir : shard_dirs) {
        utilities::indexer::IndexDatabase db(
            dir, utilities::indexer::IndexOpenMode::ReadOnly);
        auto registry = db.query_all_file_registry();

        IndexShardEntry entry;
        entry.num_files = registry.size();
        std::int64_t lo = -1;
        std::int64_t hi = -1;
        for (const auto& [name, file] : registry) {
            if (lo < 0 || file.file_id < lo) lo = file.file_id;
            if (file.file_id > hi) hi = file.file_id;
        }
        entry.file_id_min = entry.num_files == 0 ? 0 : lo;
        entry.file_id_max = entry.num_files == 0 ? -1 : hi;

        const std::string rel = fs::path(dir).lexically_relative(root).string();
        entry.path = (rel.empty() || rel.rfind("..", 0) == 0) ? dir : rel;
        manifest.shards.push_back(std::move(entry));
    }
    write_shard_manifest(root, manifest);
}

coro::CoroTask<std::size_t> consolidate_shard_set(CoroScope* scope,
                                                  const std::string& root,
                                                  const std::string& out_root) {
    auto manifest = read_shard_manifest(root);
    if (!manifest) {
        throw DFTUtilsException(ErrorCode::IO,
                                "no shard manifest under " + root + " (" +
                                    std::string(SHARD_MANIFEST_FILENAME) + ")");
    }

    std::vector<std::string> files;
    for (const auto& shard : manifest->shards) {
        if (shard.empty()) continue;
        const std::string idx = (fs::path(root) / shard.path).string();
        utilities::indexer::IndexDatabase db(
            idx, utilities::indexer::IndexOpenMode::ReadOnly);
        for (const auto& [path, file_id] : db.query_all_file_info_ids())
            files.push_back(path);
    }
    if (files.empty()) co_return 0;

    indexing::ResolveAndBuildInput in;
    in.files = files;
    in.index_dir = out_root;
    in.require_checkpoints = true;
    in.require_aggregation = true;
    in.aggregation_config = aggregators::AggregationConfig{};
    co_await indexing::resolve_and_build_index(scope, std::move(in));

    const std::string unified =
        internal::determine_index_path(files.front(), out_root);
    write_shard_set(out_root, {unified});
    co_return files.size();
}

namespace {

namespace agg = aggregators;
namespace rcf = dftracer::utils::rocksdb::cf;

bool is_agg_sentinel(std::string_view key) {
    // Data keys begin with a 2-byte shard prefix < 0x1000 (so byte 0 <= 0x0F);
    // the intern-dict / config / file-marker sentinels begin with 0xFF, and the
    // tracker key begins with '_'. Skip everything that is not a data key.
    return key.empty() || static_cast<unsigned char>(key[0]) == 0xFF ||
           key[0] == '_';
}

}  // namespace

std::size_t merge_shard_set(const std::string& root,
                            const std::string& out_root) {
    auto manifest = read_shard_manifest(root);
    if (!manifest) {
        throw DFTUtilsException(ErrorCode::IO,
                                "no shard manifest under " + root + " (" +
                                    std::string(SHARD_MANIFEST_FILENAME) + ")");
    }

    const std::string out_idx = internal::determine_index_path("x", out_root);

    agg::AggInternTable out_intern;
    std::size_t files = 0;
    bool wrote_config = false;

    {
        utilities::indexer::IndexDatabase out_db(
            out_idx, utilities::indexer::IndexOpenMode::ReadWrite);
        out_db.init_schema();
        auto out = out_db.db();

        const auto agg_cfg_key = std::string_view(
            agg::AGG_GLOBAL_CONFIG_KEY, sizeof(agg::AGG_GLOBAL_CONFIG_KEY) - 1);

        for (const auto& shard : manifest->shards) {
            if (shard.empty()) continue;
            const std::string idx = (fs::path(root) / shard.path).string();
            utilities::indexer::IndexDatabase sdb(
                idx, utilities::indexer::IndexOpenMode::ReadOnly);
            auto s = sdb.db();

            agg::AggInternTable shard_intern;
            agg::load_intern_dictionary(*s, shard_intern);

            std::uint32_t config_hash = 0;
            {
                std::string cfg;
                if (s->get(agg_cfg_key, &cfg, rcf::AGGREGATION).ok()) {
                    config_hash =
                        agg::deserialize_agg_global_config(cfg).config_hash;
                    if (!wrote_config) {
                        auto b = out->begin_batch();
                        out->put(b, rcf::AGGREGATION, agg_cfg_key, cfg);
                        out->commit_batch(b);
                        wrote_config = true;
                    }
                }
            }

            // Re-key the aggregation tier into the unified intern; the value
            // (metrics) carries no interned strings, so it copies verbatim and
            // the merge operator combines groups that recur across shards.
            {
                auto b = out->begin_batch();
                std::size_t n = 0;
                auto it = s->new_iterator(rcf::AGGREGATION);
                std::string newkey;
                for (it->SeekToFirst(); it->Valid(); it->Next()) {
                    const std::string_view key(it->key().data(),
                                               it->key().size());
                    if (is_agg_sentinel(key)) continue;
                    const std::string_view value(it->value().data(),
                                                 it->value().size());
                    agg::AggKeyView kv;
                    if (!agg::parse_agg_key_view(key, shard_intern.intern, kv,
                                                 /*want_extra_keys=*/true))
                        continue;
                    newkey.clear();
                    agg::serialize_agg_key_into(
                        newkey, config_hash, kv.map_type, kv.cat, kv.name,
                        kv.pid, kv.tid, kv.hhash, kv.fhash_str, kv.time_bucket,
                        out_intern.intern, &kv.extra_keys);
                    out->merge(b, rcf::AGGREGATION, newkey, value);
                    if (++n % 4096 == 0) {
                        out->commit_batch(b);
                        b = out->begin_batch();
                    }
                }
                out->commit_batch(b);
            }

            // System metrics keys carry raw hhash/name (no intern), so merge
            // them verbatim.
            {
                auto b = out->begin_batch();
                std::size_t n = 0;
                auto it = s->new_iterator(rcf::SYSTEM_METRICS);
                for (it->SeekToFirst(); it->Valid(); it->Next()) {
                    const std::string_view key(it->key().data(),
                                               it->key().size());
                    const std::string_view value(it->value().data(),
                                                 it->value().size());
                    out->merge(b, rcf::SYSTEM_METRICS, key, value);
                    if (++n % 4096 == 0) {
                        out->commit_batch(b);
                        b = out->begin_batch();
                    }
                }
                out->commit_batch(b);
            }

            // Copy the file registry (the `f|` entries) so the consolidated
            // index knows its files; the tier read matches on path, and the
            // per-file ids the value carries are unused by a tier query.
            {
                auto b = out->begin_batch();
                auto it = s->new_iterator(rcf::DEFAULT);
                for (it->Seek("f|"); it->Valid(); it->Next()) {
                    const std::string_view key(it->key().data(),
                                               it->key().size());
                    if (key.rfind("f|", 0) != 0) break;
                    out->put(b, rcf::DEFAULT, key,
                             std::string_view(it->value().data(),
                                              it->value().size()));
                    ++files;
                }
                out->commit_batch(b);
            }
        }

        {
            auto b = out->begin_batch();
            agg::flush_intern_dictionary(*out, b, out_intern);
            out->commit_batch(b);
        }
        out->compact(rcf::AGGREGATION);
        out->compact(rcf::SYSTEM_METRICS);
    }

    write_shard_set(out_root, {out_idx});
    return files;
}

}  // namespace dftracer::utils::trace::views
