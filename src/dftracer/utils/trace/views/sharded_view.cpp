// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/common/versions.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/sharded_view.h>

#include <algorithm>
#include <cstdint>
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
    const std::size_t ways =
        common::concurrent_spill_ways(configured, shards.size());
    const std::uint64_t share = common::share_spill_budget(configured, ways);
    partials.reserve(shards.size());
    for (std::size_t at = 0; at < shards.size(); at += ways) {
        const std::size_t end = std::min(shards.size(), at + ways);
        std::vector<dataframe::LazyResult<std::string>> plans;
        plans.reserve(end - at);
        for (std::size_t i = at; i < end; ++i)
            plans.push_back(shards[i].memory_budget(share).aggregate_partial());
        dstd::Vector<coro::CoroTask<std::string>> runs;
        runs.reserve(plans.size());
        for (const auto& p : plans) runs.push_back(p.collect());
        auto done = co_await coro::when_all(std::move(runs));
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
        throw common::DFTUtilsException(
            common::ErrorCode::Io,
            "no shard manifest at " +
                dftracer::utils::index::shard_manifest_path(root));
    }
    constexpr std::uint32_t INDEX_FORMAT_VERSION =
        static_cast<std::uint32_t>(DFTRACER_UTILS_INDEX_FORMAT_VERSION);
    if (manifest->format_version !=
        dstd::Optional<std::uint32_t>(INDEX_FORMAT_VERSION)) {
        throw common::DFTUtilsException(
            common::ErrorCode::InvalidArgument,
            "shard manifest format version " +
                (manifest->format_version
                     ? std::to_string(*manifest->format_version)
                     : std::string("(missing)")) +
                " does not match this build's " +
                std::to_string(INDEX_FORMAT_VERSION));
    }

    std::vector<std::string> dirs;
    dirs.reserve(manifest->shards.size());
    for (const auto& shard : manifest->shards) {
        if (shard.empty()) continue;
        dirs.push_back(std::string((dstd::PathView(root) / shard.path).str()));
    }
    return ShardedView(std::move(dirs));
}

std::vector<ViewFile> ShardedView::shard_view_files(
    const std::string& shard_dir) const {
    // A shard's Members scopes store each file's full canonical path, so it
    // locates the trace directly and the index may live apart from it.
    auto snapshot =
        common::unwrap(dftracer::utils::index::plan::open_snapshot(shard_dir));
    const auto indexed = common::unwrap(snapshot.files());
    std::vector<ViewFile> files;
    files.reserve(indexed.size());
    for (const auto& file : indexed) {
        ViewFile vf;
        vf.file_path = std::string(file.path());
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

    std::vector<dstd::StringView> pv(partials.begin(), partials.end());
    co_return configure(View::from_files({})).merge_partials(pv);
}

coro::CoroTask<ExportStats> ShardedView::aggregate_counters(
    Configure configure, ExportSink& sink) const {
    std::vector<View> views = shards(configure);
    std::vector<std::string> partials =
        co_await collect_partials(std::move(views));

    std::vector<dstd::StringView> pv(partials.begin(), partials.end());
    co_return configure(View::from_files({})).merge_counter_partials(pv, sink);
}

void write_shard_set(const std::string& root,
                     const std::vector<std::string>& shard_dirs) {
    dftracer::utils::index::store::IndexShardManifest manifest;
    manifest.format_version =
        static_cast<std::uint32_t>(DFTRACER_UTILS_INDEX_FORMAT_VERSION);
    manifest.shards.reserve(shard_dirs.size());
    for (const auto& dir : shard_dirs) {
        auto snapshot =
            common::unwrap(dftracer::utils::index::plan::open_snapshot(dir));

        dftracer::utils::index::store::IndexShardEntry entry;
        entry.num_files =
            static_cast<std::uint64_t>(common::unwrap(snapshot.files()).size());

        const dstd::Path rel =
            dstd::relative(dstd::PathView(dir), dstd::PathView(root));
        entry.path = (rel.empty() || rel.str().rfind("..", 0) == 0)
                         ? dir
                         : std::string(rel.str());
        manifest.shards.push_back(std::move(entry));
    }
    dftracer::utils::index::store::write_shard_manifest(root, manifest);
}

std::size_t consolidate_shard_set(const std::string& root) {
    auto manifest = dftracer::utils::index::store::read_shard_manifest(root);
    if (!manifest) {
        throw common::DFTUtilsException(
            common::ErrorCode::Io,
            "no shard manifest at " +
                dftracer::utils::index::shard_manifest_path(root));
    }

    std::vector<std::string> shard_roots;
    shard_roots.reserve(manifest->shards.size());
    std::size_t files = 0;
    for (const auto& shard : manifest->shards) {
        if (shard.empty()) continue;
        shard_roots.push_back(
            std::string((dstd::PathView(root) / shard.path).str()));
        files += shard.num_files;
    }

    dftracer::utils::index::release_root(root);
    for (const auto& shard_root : shard_roots)
        dftracer::utils::index::release_root(shard_root);

    const dstd::String target = dftracer::utils::index::store_dir(root);
    for (const auto& shard_root : shard_roots) {
        common::unwrap(dftracer::utils::store::Store::adopt(
            dftracer::utils::index::store_dir(shard_root), target));
    }
    dftracer::utils::index::release_root(root);

    // Cleanup after the merge, so a failed removal leaves only stray files.
    (void)dstd::fs::remove(
        dstd::PathView(dftracer::utils::index::shard_manifest_path(root)));
    for (const auto& shard : manifest->shards)
        (void)dstd::fs::remove_all((dstd::PathView(root) / shard.path).view());
    (void)dstd::fs::remove(
        dstd::PathView(dftracer::utils::index::shards_dir(root)));
    return files;
}

namespace {

namespace agg = dftracer::utils::index::schemas::dft::agg;
namespace idx = dftracer::utils::index;

// Copies the partials and column blocks of one file, re-keying their
// dictionary ids into the consolidated table.
void copy_agg_data(const idx::Snapshot& source_db,
                   const idx::FileRef& source_file, idx::FileBuild& build,
                   const agg::AggInternTable& source,
                   agg::AggInternTable& target) {
    std::vector<std::uint32_t> referenced;
    const auto remap = [&](std::uint32_t id) {
        const std::uint32_t mapped =
            target.intern.get_or_insert(source.intern.resolve(id));
        referenced.push_back(mapped);
        return mapped;
    };
    common::unwrap(idx::for_each_agg_partial(
        source_db, source_file,
        [&](std::uint64_t chunk, std::uint32_t cat, std::uint32_t name,
            std::uint32_t host, std::uint32_t extra, agg::PartialKind kind,
            dstd::Span<const std::uint8_t> body) {
            auto parsed = agg::AggPartial::deserialize(
                dstd::Span<const std::uint8_t>(body.data(), body.size()));
            if (!parsed)
                throw common::DFTUtilsException(
                    common::ErrorCode::Parse,
                    "aggregation partial: malformed record");
            idx::put_agg_partial(
                build, chunk, remap(cat), remap(name), remap(host),
                remap(extra), static_cast<std::uint8_t>(kind),
                dstd::StringView(reinterpret_cast<const char*>(body.data()),
                                 body.size()));
            return true;
        }));

    // A column block keeps a block-local table of dictionary ids, which is
    // rewritten before the copy.
    dstd::Vector<std::uint8_t> bytes;
    common::unwrap(idx::for_each_agg_chunk_columns(
        source_db, source_file,
        [&](std::uint64_t chunk, const agg::ChunkColumns& block) {
            agg::ChunkColumns mapped = block;
            for (std::uint32_t& id : mapped.ids) id = remap(id);
            bytes.clear();
            agg::encode_chunk_columns(mapped, mapped.columns.size(), bytes);
            idx::put_agg_chunk_columns(
                build, chunk,
                dstd::StringView(reinterpret_cast<const char*>(bytes.data()),
                                 bytes.size()));
            return true;
        }));

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
        throw common::DFTUtilsException(
            common::ErrorCode::Io,
            "no shard manifest at " +
                dftracer::utils::index::shard_manifest_path(root));
    }

    const dstd::String out_idx = internal::determine_index_path("x", out_root);

    agg::AggInternTable out_intern;
    std::size_t files = 0;
    dstd::Optional<agg::tier::Config> config;

    default_runtime().block_on(
        coro::named("shard merge", [&]() -> coro::CoroTask<void> {
            idx::Index out = common::unwrap(idx::Index::open(out_idx));

            for (const auto& shard : manifest->shards) {
                if (shard.empty()) continue;
                const std::string idx(
                    (dstd::PathView(root) / shard.path).str());
                auto snapshot = common::unwrap(idx::plan::open_snapshot(idx));
                agg::AggInternTable shard_intern;
                common::unwrap(idx::agg_dictionary(snapshot, shard_intern));

                const auto shard_config =
                    common::unwrap(agg::tier::read_config(snapshot));
                if (shard_config) {
                    if (!config) {
                        config = shard_config;
                        agg::tier::write_config(out, *config);
                    } else if (config->params_hash !=
                               shard_config->params_hash) {
                        throw common::DFTUtilsException(
                            common::ErrorCode::InvalidArgument,
                            "shard " + shard.path +
                                " was aggregated with another config than the "
                                "first shard");
                    }
                }

                for (const auto& file : common::unwrap(snapshot.files())) {
                    auto build =
                        common::unwrap(out.build(file.path(), file.stat()));

                    if (config &&
                        snapshot.extension(
                            file, idx::store::IndexExtension::Agg,
                            config->params_hash) == idx::Freshness::Current) {
                        copy_agg_data(snapshot, file, build, shard_intern,
                                      out_intern);
                        build.finish(idx::store::IndexExtension::Agg,
                                     config->params_hash);
                    }
                    build.finish(idx::store::IndexExtension::Members, 0);
                    common::unwrap(co_await build.commit());
                    ++files;
                }
            }
        }()));

    return files;
}

}  // namespace dftracer::utils::trace::views
