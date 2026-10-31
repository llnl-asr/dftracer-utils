#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/runtime.h>
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
    in.aggregation_config = dftracer::utils::index::schemas::dft::agg::
        standard_aggregation_config();
    co_await dftracer::utils::index::build::resolve_and_build_index(
        scope, std::move(in));

    const std::string unified =
        internal::determine_index_path(files.front(), out_root);
    write_shard_set(out_root, {unified});
    co_return files.size();
}

namespace {

namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace idx = dftracer::utils::index;

// Copies the partials and column blocks of one file, re-keying their
// dictionary ids into the consolidated table.
void copy_agg_data(const idx::store::IndexDatabase& source_db, int source_id,
                   idx::FileBuild& build, const agg::AggInternTable& source,
                   agg::AggInternTable& target) {
    std::vector<std::uint32_t> referenced;
    const auto remap = [&](std::uint32_t id) {
        const std::uint32_t mapped =
            target.intern.get_or_insert(source.intern.resolve(id));
        referenced.push_back(mapped);
        return mapped;
    };
    agg::tier::for_each_partial(
        source_db, source_id,
        [&](std::uint64_t chunk, std::uint32_t cat, std::uint32_t name,
            std::uint32_t host, std::uint32_t extra, agg::PartialKind kind,
            std::span<const std::uint8_t> body) {
            auto parsed = agg::AggPartial::deserialize(
                dstd::Span<const std::uint8_t>(body.data(), body.size()));
            if (!parsed)
                throw DFTUtilsException(
                    ErrorCode::Parse, "aggregation partial: malformed record");
            idx::put_agg_partial(
                build, chunk, remap(cat), remap(name), remap(host),
                remap(extra), static_cast<std::uint8_t>(kind),
                std::string_view(reinterpret_cast<const char*>(body.data()),
                                 body.size()));
            return true;
        });

    // A column block keeps a block-local table of dictionary ids, which is
    // rewritten before the copy.
    dstd::Vector<std::uint8_t> bytes;
    agg::tier::for_each_chunk_columns(
        source_db, source_id,
        [&](std::uint64_t chunk, const agg::ChunkColumns& block) {
            agg::ChunkColumns mapped = block;
            for (std::uint32_t& id : mapped.ids) id = remap(id);
            bytes.clear();
            agg::encode_chunk_columns(mapped, mapped.columns.size(), bytes);
            idx::put_agg_chunk_columns(
                build, chunk,
                std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                 bytes.size()));
            return true;
        });

    std::sort(referenced.begin(), referenced.end());
    referenced.erase(std::unique(referenced.begin(), referenced.end()),
                     referenced.end());
    for (const std::uint32_t id : referenced)
        idx::put_agg_dictionary_entry(build, id, target.intern.resolve(id));
}

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
    std::optional<agg::tier::Config> config;

    default_runtime().run_blocking(
        "shard merge", [&](CoroScope&) -> coro::CoroTask<void> {
            idx::Index out = unwrap(idx::Index::open(out_idx));

            for (const auto& shard : manifest->shards) {
                if (shard.empty()) continue;
                const std::string idx = (fs::path(root) / shard.path).string();
                idx::store::IndexDatabase sdb(
                    idx, idx::store::IndexOpenMode::ReadOnly);
                agg::AggInternTable shard_intern;
                agg::tier::load_dictionary(sdb, shard_intern);

                if (auto shard_config = agg::tier::read_config(sdb)) {
                    if (!config) {
                        config = shard_config;
                        agg::tier::write_config(out, *config);
                    } else if (config->params_hash !=
                               shard_config->params_hash) {
                        throw DFTUtilsException(
                            ErrorCode::InvalidArgument,
                            "shard " + shard.path +
                                " was aggregated with another config than the "
                                "first shard");
                    }
                }

                const auto registry = sdb.query_all_file_registry();
                std::vector<std::pair<std::string_view, int>> entries;
                entries.reserve(registry.size());
                for (const auto& [path, entry] : registry)
                    entries.emplace_back(path, entry.file_id);
                std::sort(entries.begin(), entries.end());

                for (const auto& [path, source_id] : entries) {
                    const auto stat = sdb.get_file_stat(path);
                    const auto hash = sdb.get_file_hash(path);
                    if (!stat || !hash)
                        throw DFTUtilsException(
                            ErrorCode::Parse,
                            "shard " + shard.path +
                                " holds a malformed file record");
                    auto build = unwrap(out.build(
                        path, idx::SourceStat{stat->mtime, stat->size, *hash}));

                    const auto state = sdb.extension_state(
                        source_id, idx::store::IndexExtension::Agg);
                    if (config && state && state->ready &&
                        state->params_hash == config->params_hash) {
                        copy_agg_data(sdb, source_id, build, shard_intern,
                                      out_intern);
                        build.finish(idx::store::IndexExtension::Agg,
                                     config->params_hash);
                    }
                    build.finish(idx::store::IndexExtension::Members, 0);
                    unwrap(co_await build.commit());
                    ++files;
                }
            }
        });

    write_shard_set(out_root, {out_idx});
    return files;
}

}  // namespace dftracer::utils::trace::views
