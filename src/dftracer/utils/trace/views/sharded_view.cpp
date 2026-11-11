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
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/trace/index/store/layout.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/store/shard_manifest.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/sharded_view.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace dftracer::utils::trace::views {

namespace {

// Each shard's aggregate_partial, run concurrently in waves as wide as the
// spill budget allows, the budget split evenly across a wave.
coro::CoroTask<dstd::Result<dstd::Vector<dstd::String>>> collect_partials(
    dstd::Vector<View> shards) {
    dstd::Vector<dstd::String> partials;
    if (shards.empty()) co_return partials;
    const std::uint64_t configured = shards.front().memory_budget_bytes();
    const std::size_t ways =
        common::concurrent_spill_ways(configured, shards.size());
    const std::uint64_t share = common::share_spill_budget(configured, ways);
    partials.reserve(shards.size());
    for (std::size_t at = 0; at < shards.size(); at += ways) {
        const std::size_t end = std::min(shards.size(), at + ways);
        dstd::Vector<dataframe::LazyResult<dstd::String>> plans;
        plans.reserve(end - at);
        for (std::size_t i = at; i < end; ++i)
            plans.push_back(shards[i].memory_budget(share).aggregate_partial());
        dstd::Vector<coro::CoroTask<dstd::Result<dstd::String>>> runs;
        runs.reserve(plans.size());
        for (const auto& p : plans) runs.push_back(p.collect());
        auto done = co_await coro::when_all(std::move(runs));
        for (auto& d : done) {
            if (!d) co_return d.error();
            partials.push_back(std::move(*d));
        }
    }
    co_return partials;
}

dstd::Result<
    dstd::Optional<dftracer::utils::trace::index::store::IndexShardManifest>>
manifest_at(const dstd::String& root) {
    return dftracer::utils::trace::index::store::read_shard_manifest(root);
}

}  // namespace

ShardedView ShardedView::from_shard_dirs(
    dstd::Vector<dstd::String> shard_dirs) {
    return ShardedView(std::move(shard_dirs));
}

dstd::Result<ShardedView> ShardedView::from_manifest(const dstd::String& root) {
    auto read = manifest_at(root);
    if (!read) return read.error();
    const auto& manifest = *read;
    if (!manifest) {
        return common::make_error<"no shard manifest at {}">(
            common::ErrorCode::Io,
            dftracer::utils::trace::index::shard_manifest_path(root));
    }
    constexpr std::uint32_t INDEX_FORMAT_VERSION =
        static_cast<std::uint32_t>(DFTRACER_UTILS_INDEX_FORMAT_VERSION);
    if (manifest->format_version !=
        dstd::Optional<std::uint32_t>(INDEX_FORMAT_VERSION)) {
        const dstd::String found =
            manifest->format_version
                ? dstd::to_string(*manifest->format_version)
                : dstd::String("(missing)");
        return common::make_error<
            "shard manifest format version {} does not match this build's {}">(
            common::ErrorCode::InvalidArgument, found, INDEX_FORMAT_VERSION);
    }

    dstd::Vector<dstd::String> dirs;
    dirs.reserve(manifest->shards.size());
    for (const auto& shard : manifest->shards) {
        if (shard.empty()) continue;
        dirs.push_back(dstd::String((dstd::PathView(root) / shard.path).str()));
    }
    return ShardedView(std::move(dirs));
}

dstd::Result<dstd::Vector<ViewFile>> ShardedView::shard_view_files(
    const dstd::String& shard_dir) const {
    // A shard's Members scopes store each file's full canonical path, so it
    // locates the trace directly and the index may live apart from it.
    auto snapshot =
        dftracer::utils::trace::index::plan::open_snapshot(shard_dir);
    if (!snapshot) return snapshot.error();
    const auto indexed = snapshot->files();
    if (!indexed) return indexed.error();
    dstd::Vector<ViewFile> files;
    files.reserve(indexed->size());
    for (const auto& file : *indexed) {
        ViewFile vf;
        vf.file_path = dstd::String(file.path());
        vf.index_path = shard_dir;
        files.push_back(std::move(vf));
    }
    return files;
}

dstd::Result<dstd::Vector<View>> ShardedView::shards(
    const Configure& configure) const {
    dstd::Vector<View> out;
    out.reserve(shard_dirs_.size());
    for (const auto& dir : shard_dirs_) {
        auto listed = shard_view_files(dir);
        if (!listed) return listed.error();
        if (listed->empty()) continue;
        out.push_back(configure(View::from_files(std::move(*listed))));
    }
    return out;
}

coro::CoroTask<dstd::Result<dftracer::utils::dataframe::DataFrame>>
ShardedView::aggregate(Configure configure) const {
    auto views = shards(configure);
    if (!views) co_return views.error();
    auto collected = co_await collect_partials(std::move(*views));
    if (!collected) co_return collected.error();
    const dstd::Vector<dstd::String>& partials = *collected;

    dstd::Vector<dstd::StringView> pv(partials.begin(), partials.end());
    co_return configure(View::from_files({})).merge_partials(pv);
}

coro::CoroTask<dstd::Result<ExportStats>> ShardedView::aggregate_counters(
    Configure configure, ExportSink& sink) const {
    auto views = shards(configure);
    if (!views) co_return views.error();
    auto collected = co_await collect_partials(std::move(*views));
    if (!collected) co_return collected.error();
    const dstd::Vector<dstd::String>& partials = *collected;

    dstd::Vector<dstd::StringView> pv(partials.begin(), partials.end());
    co_return configure(View::from_files({})).merge_counter_partials(pv, sink);
}

dstd::Result<void> write_shard_set(
    const dstd::String& root, const dstd::Vector<dstd::String>& shard_dirs) {
    dftracer::utils::trace::index::store::IndexShardManifest manifest;
    manifest.format_version =
        static_cast<std::uint32_t>(DFTRACER_UTILS_INDEX_FORMAT_VERSION);
    manifest.shards.reserve(shard_dirs.size());
    for (const auto& dir : shard_dirs) {
        auto snapshot = dftracer::utils::trace::index::plan::open_snapshot(dir);
        if (!snapshot) return snapshot.error();
        const auto indexed = snapshot->files();
        if (!indexed) return indexed.error();

        dftracer::utils::trace::index::store::IndexShardEntry entry;
        entry.file_count = static_cast<std::uint64_t>(indexed->size());

        const dstd::Path rel =
            dstd::relative(dstd::PathView(dir), dstd::PathView(root));
        entry.path = (rel.empty() || rel.str().rfind("..", 0) == 0)
                         ? dir
                         : dstd::String(rel.str());
        manifest.shards.push_back(std::move(entry));
    }
    return dftracer::utils::trace::index::store::write_shard_manifest(root,
                                                                      manifest);
}

dstd::Result<std::size_t> consolidate_shard_set(const dstd::String& root) {
    auto read = manifest_at(root);
    if (!read) return read.error();
    const auto& manifest = *read;
    if (!manifest) {
        return common::make_error<"no shard manifest at {}">(
            common::ErrorCode::Io,
            dftracer::utils::trace::index::shard_manifest_path(root));
    }

    dstd::Vector<dstd::String> shard_roots;
    shard_roots.reserve(manifest->shards.size());
    std::size_t files = 0;
    for (const auto& shard : manifest->shards) {
        if (shard.empty()) continue;
        shard_roots.push_back(
            dstd::String((dstd::PathView(root) / shard.path).str()));
        files += shard.file_count;
    }

    dftracer::utils::trace::index::release_root(root);
    for (const auto& shard_root : shard_roots)
        dftracer::utils::trace::index::release_root(shard_root);

    const dstd::String target = dftracer::utils::trace::index::store_dir(root);
    for (const auto& shard_root : shard_roots) {
        auto adopted = dftracer::utils::store::Store::adopt(
            dftracer::utils::trace::index::store_dir(shard_root), target);
        if (!adopted) return adopted.error();
    }
    dftracer::utils::trace::index::release_root(root);

    // Cleanup after the merge, so a failed removal leaves only stray files.
    dstd::fs::remove(
        dstd::PathView(
            dftracer::utils::trace::index::shard_manifest_path(root)))
        .discard();
    for (const auto& shard : manifest->shards)
        dstd::fs::remove_all((dstd::PathView(root) / shard.path).view())
            .discard();
    dstd::fs::remove(
        dstd::PathView(dftracer::utils::trace::index::shards_dir(root)))
        .discard();
    return files;
}

namespace {

// Copies the partials, per-file partials and column blocks of one file,
// re-keying their dictionary ids into the consolidated table.
dstd::Result<void> copy_agg_data(
    const trace::index::Snapshot& source_db,
    const trace::index::FileRef& source_file, trace::index::FileBuild& build,
    const trace::index::schemas::dft::agg::AggInternTable& source,
    trace::index::schemas::dft::agg::AggInternTable& target) {
    dstd::Vector<std::uint32_t> referenced;
    const auto remap = [&](std::uint32_t id) {
        const std::uint32_t mapped =
            target.intern.get_or_insert(source.intern.resolve(id));
        referenced.push_back(mapped);
        return mapped;
    };
    dstd::Optional<dstd::Error> malformed;
    auto walked = trace::index::for_each_agg_partial(
        source_db, source_file,
        [&](std::uint64_t chunk, std::uint32_t cat, std::uint32_t name,
            std::uint32_t host, std::uint32_t extra,
            trace::index::schemas::dft::agg::PartialKind kind,
            dstd::Span<const std::uint8_t> body) {
            auto parsed =
                trace::index::schemas::dft::agg::AggPartial::deserialize(
                    dstd::Span<const std::uint8_t>(body.data(), body.size()));
            if (!parsed) {
                malformed =
                    dstd::Error(common::ErrorCode::Parse,
                                "aggregation partial: malformed record");
                return false;
            }
            trace::index::put_agg_partial(
                build, chunk, remap(cat), remap(name), remap(host),
                remap(extra), static_cast<std::uint8_t>(kind),
                dstd::StringView(reinterpret_cast<const char*>(body.data()),
                                 body.size()));
            return true;
        });
    if (!walked) return walked.error();
    if (malformed) return *malformed;

    walked = trace::index::for_each_agg_file_partial(
        source_db, source_file,
        [&](std::uint64_t chunk, std::uint32_t file_id,
            trace::index::schemas::dft::agg::PartialKind kind,
            dstd::Span<const std::uint8_t> body) {
            auto parsed =
                trace::index::schemas::dft::agg::AggPartial::deserialize(
                    dstd::Span<const std::uint8_t>(body.data(), body.size()));
            if (!parsed) {
                malformed =
                    dstd::Error(common::ErrorCode::Parse,
                                "aggregation file partial: malformed record");
                return false;
            }
            trace::index::put_agg_file_partial(
                build, chunk,
                file_id == trace::index::store::layout::agg_kind::NO_FILE
                    ? file_id
                    : remap(file_id),
                static_cast<std::uint8_t>(kind),
                dstd::StringView(reinterpret_cast<const char*>(body.data()),
                                 body.size()));
            return true;
        });
    if (!walked) return walked.error();
    if (malformed) return *malformed;

    // A column block keeps a block-local table of dictionary ids, which is
    // rewritten before the copy.
    dstd::Vector<std::uint8_t> bytes;
    walked = trace::index::for_each_agg_chunk_columns(
        source_db, source_file,
        [&](std::uint64_t chunk,
            const trace::index::schemas::dft::agg::ChunkColumns& block) {
            trace::index::schemas::dft::agg::ChunkColumns mapped = block;
            for (std::uint32_t& id : mapped.ids) id = remap(id);
            bytes.clear();
            trace::index::schemas::dft::agg::encode_chunk_columns(
                mapped, mapped.columns.size(), bytes);
            trace::index::put_agg_chunk_columns(
                build, chunk,
                dstd::StringView(reinterpret_cast<const char*>(bytes.data()),
                                 bytes.size()));
            return true;
        });
    if (!walked) return walked.error();

    std::sort(referenced.begin(), referenced.end());
    referenced.erase(std::unique(referenced.begin(), referenced.end()),
                     referenced.end());
    for (const std::uint32_t id : referenced)
        trace::index::put_agg_dictionary_entry(build, id,
                                               target.intern.resolve(id));
    return {};
}

}  // namespace

dstd::Result<std::size_t> merge_shard_set(const dstd::String& root,
                                          const dstd::String& out_root) {
    auto read = manifest_at(root);
    if (!read) return read.error();
    const auto& manifest = *read;
    if (!manifest) {
        return common::make_error<"no shard manifest at {}">(
            common::ErrorCode::Io,
            dftracer::utils::trace::index::shard_manifest_path(root));
    }

    const dstd::String out_idx = internal::determine_index_path("x", out_root);

    trace::index::schemas::dft::agg::AggInternTable out_intern;
    std::size_t files = 0;
    dstd::Optional<trace::index::schemas::dft::agg::tier::Config> config;

    auto merged = default_runtime().block_on(
        coro::named("shard merge", [&]() -> coro::CoroTask<dstd::Result<void>> {
            auto opened = trace::index::Index::open(out_idx);
            if (!opened) co_return opened.error();
            trace::index::Index out = std::move(*opened);

            for (const auto& shard : manifest->shards) {
                if (shard.empty()) continue;
                const dstd::String idx(
                    (dstd::PathView(root) / shard.path).str());
                auto snapshot = trace::index::plan::open_snapshot(idx);
                if (!snapshot) co_return snapshot.error();
                trace::index::schemas::dft::agg::AggInternTable shard_intern;
                auto dictionary =
                    trace::index::agg_dictionary(*snapshot, shard_intern);
                if (!dictionary) co_return dictionary.error();

                const auto shard_config =
                    trace::index::schemas::dft::agg::tier::read_config(
                        *snapshot);
                if (!shard_config) co_return shard_config.error();
                if (*shard_config) {
                    if (!config) {
                        config = *shard_config;
                        auto written =
                            trace::index::schemas::dft::agg::tier::write_config(
                                out, *config);
                        if (!written) co_return written.error();
                    } else if (config->params_hash !=
                               (*shard_config)->params_hash) {
                        co_return common::make_error<
                            "shard {} was aggregated with another config than "
                            "the first shard">(
                            common::ErrorCode::InvalidArgument, shard.path);
                    }
                }

                const auto indexed = snapshot->files();
                if (!indexed) co_return indexed.error();
                for (const auto& file : *indexed) {
                    auto build = out.build(file.path(), file.stat());
                    if (!build) co_return build.error();

                    if (config &&
                        snapshot->extension(
                            file, trace::index::store::IndexExtension::Agg,
                            config->params_hash) ==
                            trace::index::Freshness::Current) {
                        auto copied = copy_agg_data(*snapshot, file, *build,
                                                    shard_intern, out_intern);
                        if (!copied) co_return copied.error();
                        build->finish(trace::index::store::IndexExtension::Agg,
                                      config->params_hash);
                    }
                    build->finish(trace::index::store::IndexExtension::Members,
                                  0);
                    auto committed = co_await build->commit();
                    if (!committed) co_return committed.error();
                    ++files;
                }
            }
            co_return dstd::Result<void>{};
        }()));
    if (!merged) return merged.error();

    return files;
}

}  // namespace dftracer::utils::trace::views
