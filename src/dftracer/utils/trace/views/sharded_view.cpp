#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_serialization.h>
#include <dftracer/utils/index/store/column_families.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/scan_prefix.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/sharded_view.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::views {

namespace {

// Each shard's aggregate_partial, run concurrently in waves as wide as the
// spill budget allows, the budget split evenly across a wave.
coro::CoroTask<std::vector<std::string>> collect_partials(
    std::vector<View> shards) {
    std::vector<std::string> partials;
    if (shards.empty()) co_return partials;
    const std::uint64_t configured = shards.front().memory_budget_bytes();
    const std::size_t ways = concurrent_spill_ways(configured, shards.size());
    const std::uint64_t share = share_spill_budget(configured, ways);
    partials.reserve(shards.size());
    for (std::size_t at = 0; at < shards.size(); at += ways) {
        const std::size_t end = std::min(shards.size(), at + ways);
        std::vector<dataframe::LazyResult<std::string>> plans;
        plans.reserve(end - at);
        for (std::size_t i = at; i < end; ++i)
            plans.push_back(shards[i].memory_budget(share).aggregate_partial());
        std::vector<coro::CoroTask<std::string>> runs;
        runs.reserve(plans.size());
        for (const auto& p : plans) runs.push_back(p.collect());
        std::vector<std::string> done =
            co_await coro::when_all(std::move(runs));
        for (std::string& d : done) partials.push_back(std::move(d));
    }
    co_return partials;
}

}  // namespace

ShardedView ShardedView::from_shard_dirs(std::vector<std::string> shard_dirs) {
    return ShardedView(std::move(shard_dirs));
}

ShardedView ShardedView::from_manifest(const std::string& root) {
    auto manifest = dftracer::utils::index::store::read_shard_manifest(root);
    if (!manifest) {
        throw DFTUtilsException(
            ErrorCode::Io,
            "no shard manifest under " + root + " (" +
                std::string(
                    dftracer::utils::index::store::SHARD_MANIFEST_FILENAME) +
                ")");
    }
    if (manifest->format_version !=
        std::optional<std::uint32_t>(
            dftracer::utils::index::store::IndexDatabase::FORMAT_VERSION)) {
        throw DFTUtilsException(
            ErrorCode::InvalidArgument,
            "shard manifest format version " +
                (manifest->format_version
                     ? std::to_string(*manifest->format_version)
                     : std::string("(missing)")) +
                " does not match this build's " +
                std::to_string(dftracer::utils::index::store::IndexDatabase::
                                   FORMAT_VERSION));
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
    dftracer::utils::index::store::IndexDatabase db(
        shard_dir, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
    std::vector<ViewFile> files;
    for (const auto& [path, file_id] : db.query_all_file_info_ids()) {
        ViewFile vf;
        vf.file_path = path;
        vf.index_path = shard_dir;
        files.push_back(std::move(vf));
    }
    return files;
}

std::vector<View> ShardedView::shards(const Configure& configure) const {
    std::vector<View> out;
    out.reserve(shard_dirs_.size());
    for (const auto& dir : shard_dirs_) {
        std::vector<ViewFile> files = shard_view_files(dir);
        if (files.empty()) continue;
        out.push_back(configure(View::from_files(std::move(files))));
    }
    return out;
}

coro::CoroTask<dftracer::utils::dataframe::DataFrame> ShardedView::aggregate(
    Configure configure) const {
    std::vector<View> views = shards(configure);
    std::vector<std::string> partials =
        co_await collect_partials(std::move(views));

    std::vector<std::string_view> pv(partials.begin(), partials.end());
    co_return configure(View::from_files({})).merge_partials(pv);
}

coro::CoroTask<ExportStats> ShardedView::aggregate_counters(
    Configure configure, ExportSink& sink) const {
    std::vector<View> views = shards(configure);
    std::vector<std::string> partials =
        co_await collect_partials(std::move(views));

    std::vector<std::string_view> pv(partials.begin(), partials.end());
    co_return configure(View::from_files({})).merge_counter_partials(pv, sink);
}

void write_shard_set(const std::string& root,
                     const std::vector<std::string>& shard_dirs) {
    dftracer::utils::index::store::IndexShardManifest manifest;
    manifest.format_version =
        dftracer::utils::index::store::IndexDatabase::FORMAT_VERSION;
    manifest.shards.reserve(shard_dirs.size());
    for (const auto& dir : shard_dirs) {
        dftracer::utils::index::store::IndexDatabase db(
            dir, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        auto registry = db.query_all_file_registry();

        dftracer::utils::index::store::IndexShardEntry entry;
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
    dftracer::utils::index::store::write_shard_manifest(root, manifest);
}

coro::CoroTask<std::size_t> consolidate_shard_set(CoroScope* scope,
                                                  const std::string& root,
                                                  const std::string& out_root) {
    auto manifest = dftracer::utils::index::store::read_shard_manifest(root);
    if (!manifest) {
        throw DFTUtilsException(
            ErrorCode::Io,
            "no shard manifest under " + root + " (" +
                std::string(
                    dftracer::utils::index::store::SHARD_MANIFEST_FILENAME) +
                ")");
    }

    std::vector<std::string> files;
    for (const auto& shard : manifest->shards) {
        if (shard.empty()) continue;
        const std::string idx = (fs::path(root) / shard.path).string();
        dftracer::utils::index::store::IndexDatabase db(
            idx, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
        for (const auto& [path, file_id] : db.query_all_file_info_ids())
            files.push_back(path);
    }
    if (files.empty()) co_return 0;

    dftracer::utils::index::build::ResolveAndBuildInput in;
    in.files = files;
    in.index_dir = out_root;
    in.require_checkpoints = true;
    in.require_aggregation = true;
    in.aggregation_config =
        dftracer::utils::index::schemas::dft::agg::AggregationConfig{};
    co_await dftracer::utils::index::build::resolve_and_build_index(
        scope, std::move(in));

    const std::string unified =
        internal::determine_index_path(files.front(), out_root);
    write_shard_set(out_root, {unified});
    co_return files.size();
}

namespace {

namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace rcf = dftracer::utils::index::store::cf;

}  // namespace

std::size_t merge_shard_set(const std::string& root,
                            const std::string& out_root) {
    auto manifest = dftracer::utils::index::store::read_shard_manifest(root);
    if (!manifest) {
        throw DFTUtilsException(
            ErrorCode::Io,
            "no shard manifest under " + root + " (" +
                std::string(
                    dftracer::utils::index::store::SHARD_MANIFEST_FILENAME) +
                ")");
    }

    const std::string out_idx = internal::determine_index_path("x", out_root);

    agg::AggInternTable out_intern;
    std::size_t files = 0;
    int next_file_id = 0;
    std::optional<agg::tier::Config> config;

    {
        dftracer::utils::index::store::IndexDatabase out_db(
            out_idx, dftracer::utils::index::store::IndexOpenMode::ReadWrite);
        out_db.init_schema();
        auto out = out_db.db();

        for (const auto& shard : manifest->shards) {
            if (shard.empty()) continue;
            const std::string idx = (fs::path(root) / shard.path).string();
            dftracer::utils::index::store::IndexDatabase sdb(
                idx, dftracer::utils::index::store::IndexOpenMode::ReadOnly);
            auto s = sdb.db();

            agg::AggInternTable shard_intern;
            agg::tier::load_dictionary(*s, shard_intern);

            if (auto shard_config = agg::tier::read_config(*s)) {
                if (!config) {
                    config = shard_config;
                    agg::tier::write_config(*out, *config);
                } else if (config->params_hash != shard_config->params_hash) {
                    throw DFTUtilsException(
                        ErrorCode::InvalidArgument,
                        "shard " + shard.path +
                            " was aggregated with another config than the "
                            "first shard");
                }
            }

            // System metrics keys carry raw hhash/name, so their rows copy
            // verbatim and the merge operator combines the ones that recur
            // across shards.
            {
                agg::tier::Writer w(*out);
                agg::tier::for_each_system_row(
                    *s, 0, agg::AGG_KEY_NUM_SHARDS,
                    [&w](std::string_view key, std::string_view value) {
                        w.merge_system_row(key, value);
                        return true;
                    });
                w.finish();
            }

            // Each shard numbers its files from the start, so a file takes
            // the next id of the consolidated index; its registry record and
            // its partials carry that id.
            dstd::HashMap<int, int> remap;
            {
                namespace layout = dftracer::utils::index::store::layout;
                const auto prefix = layout::kind_prefix(
                    layout::Ext::Host, layout::host::FILE_BY_PATH);
                auto write = out_db.begin_write();
                dftracer::utils::index::store::internal::scan_prefix_iterator(
                    "shard file registry scan", prefix,
                    [&] { return s->new_iterator(rcf::DEFAULT); },
                    [&](::rocksdb::Iterator& it) {
                        const std::string_view key(it.key().data(),
                                                   it.key().size());
                        auto record =
                            layout::decode_file_record(std::string_view(
                                it.value().data(), it.value().size()));
                        if (!record)
                            throw DFTUtilsException(
                                ErrorCode::Parse,
                                "shard " + shard.path +
                                    " holds a malformed file record");
                        const int old_id = static_cast<int>(record->file_id);
                        record->file_id =
                            static_cast<std::uint32_t>(next_file_id);
                        remap[old_id] = next_file_id++;
                        dftracer::utils::index::store::records::put_file_record(
                            *write, key.substr(prefix.size()), *record);
                        ++files;
                    });
                write->commit();
            }

            // The dftracer.agg entry of a file follows its new id.
            if (config) {
                std::vector<int> aggregated;
                std::vector<int> aggregated_old;
                for (const auto& [path, id] : sdb.query_all_file_info_ids()) {
                    auto state = sdb.extension_state(
                        id, dftracer::utils::index::store::IndexExtension::Agg);
                    if (state && state->ready &&
                        state->params_hash == config->params_hash) {
                        aggregated.push_back(remap[id]);
                        aggregated_old.push_back(id);
                    }
                }
                agg::tier::put_files(*out, aggregated, config->params_hash);

                // The partials carry dictionary ids in their cat, name, host,
                // extra and arg keys; pid, tid and file hashes inside are raw.
                // Each quantile store holds its own accuracy, so the accuracy
                // argument of the parse does not reach the bytes written.
                auto write = out_db.begin_write();
                dstd::Vector<std::uint8_t> bytes;
                for (const int old_id : aggregated_old) {
                    const int new_id = remap[old_id];
                    agg::tier::for_each_partial(
                        *s, old_id,
                        [&](std::uint32_t chunk, std::uint32_t cat,
                            std::uint32_t name, std::uint32_t host,
                            std::uint32_t extra, agg::PartialKind kind,
                            std::span<const std::uint8_t> body) {
                            const std::uint32_t new_cat =
                                out_intern.intern.get_or_insert(
                                    shard_intern.intern.resolve(cat));
                            const std::uint32_t new_name =
                                out_intern.intern.get_or_insert(
                                    shard_intern.intern.resolve(name));
                            const std::uint32_t new_extra =
                                out_intern.intern.get_or_insert(
                                    shard_intern.intern.resolve(extra));
                            const std::uint32_t new_host =
                                out_intern.intern.get_or_insert(
                                    shard_intern.intern.resolve(host));
                            std::string_view payload(
                                reinterpret_cast<const char*>(body.data()),
                                body.size());
                            auto parsed = agg::AggPartial::deserialize(
                                dstd::Span<const std::uint8_t>(body.data(),
                                                               body.size()));
                            if (!parsed)
                                throw DFTUtilsException(
                                    ErrorCode::Parse,
                                    "aggregation partial: malformed record");
                            if (!parsed->args.empty()) {
                                for (auto& a : parsed->args)
                                    a.id = out_intern.intern.get_or_insert(
                                        shard_intern.intern.resolve(a.id));
                                std::sort(parsed->args.begin(),
                                          parsed->args.end(),
                                          [](const agg::ArgStat& x,
                                             const agg::ArgStat& y) {
                                              return x.id < y.id;
                                          });
                                bytes.clear();
                                parsed->serialize_into(bytes);
                                payload = std::string_view(
                                    reinterpret_cast<const char*>(bytes.data()),
                                    bytes.size());
                            }
                            dftracer::utils::index::store::records::
                                put_agg_partial(*write, new_id, chunk, new_cat,
                                                new_name, new_host, new_extra,
                                                static_cast<std::uint8_t>(kind),
                                                payload);
                            return true;
                        });
                }
                write->commit();
            }
        }

        agg::tier::flush_dictionary(*out, out_intern);
        agg::tier::compact(*out);
    }

    write_shard_set(out_root, {out_idx});
    return files;
}

}  // namespace dftracer::utils::trace::views
