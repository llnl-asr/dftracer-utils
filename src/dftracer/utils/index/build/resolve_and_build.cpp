// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/common/scratch.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/aggregation_run.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/corrupt_index.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_drain.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <system_error>

namespace dftracer::utils::index::build {

namespace {

// Rewrites only `tier` for files whose members are current, leaving their
// members, hash tables and aggregation as they are. `tier` holds the union of
// the files' stale extensions; rewriting a current one is correct, only
// slower.
coro::CoroTask<dstd::Vector<ResolverResult::Failure>> build_tier(
    dstd::Vector<dstd::String> files, const ResolveAndBuildInput& input,
    index::store::ExtensionMask tier, const dstd::String& schema,
    std::size_t parallelism) {
    auto config = dstd::make_shared<IndexBuildBatchConfig>();
    config->file_paths = std::move(files);
    config->index_dir = input.index_dir;
    config->checkpoint_size = input.checkpoint_size;
    config->parallelism = parallelism;
    config->tier_only = true;
    config->memory_budget = input.memory_budget;
    config->schema = schema;
    config->bloom_config = input.bloom_config;
    config->bloom_config.extensions = tier;
    DFTRACER_UTILS_LOG_INFO("Building the index tier for {} files",
                            config->file_paths.size());
    auto batch = co_await BatchBuilder::process(std::move(config));
    dstd::Vector<ResolverResult::Failure> failures;
    for (const auto& r : batch.results)
        if (!r.success && !r.was_skipped)
            failures.push_back({r.file_path, r.error_message});
    co_return failures;
}

// Aggregates files whose members are current, leaving every other extension.
coro::CoroTask<dstd::Vector<ResolverResult::Failure>> build_aggregation(
    dstd::Vector<dstd::String> files, const ResolveAndBuildInput& input,
    const dstd::String& index_path, const dstd::String& schema,
    std::size_t parallelism) {
    AggRun run = start_aggregation(index_path, input.aggregation_config);
    auto config = dstd::make_shared<IndexBuildBatchConfig>();
    config->file_paths = std::move(files);
    config->index_dir = input.index_dir;
    config->checkpoint_size = input.checkpoint_size;
    config->parallelism = parallelism;
    config->tier_only = true;
    config->build_bloom = false;
    config->memory_budget = input.memory_budget;
    config->schema = schema;
    config->agg_fold_factory = std::move(run.factory);
    DFTRACER_UTILS_LOG_INFO("Aggregating {} files", config->file_paths.size());
    auto batch = co_await BatchBuilder::process(std::move(config));
    finish_aggregation(run, batch);
    dstd::Vector<ResolverResult::Failure> failures;
    for (const auto& r : batch.results)
        if (!r.success && !r.was_skipped)
            failures.push_back({r.file_path, r.error_message});
    co_return failures;
}

coro::CoroTask<ResolverResult> resolve_and_build_once(
    ResolveAndBuildInput input) {
    std::size_t parallelism = input.parallelism;
    if (parallelism == 0) parallelism = default_runtime().threads();

    Resolver resolver;
    ResolverInput resolve_input;
    resolve_input.directory = std::move(input.directory);
    resolve_input.files = std::move(input.files);
    resolve_input.entries = std::move(input.entries);
    resolve_input.index_dir = input.index_dir;
    resolve_input.require_checkpoints = input.require_checkpoints;
    resolve_input.require_bloom = input.require_bloom;
    resolve_input.checkpoint_size = input.checkpoint_size;
    resolve_input.aggregation_config = input.aggregation_config;
    resolve_input.schema = input.schema;
    for (dstd::String& field : input.bloom_config.extra_dimensions)
        field = extra_dimension_name(field);
    if (input.build_bloom) resolve_input.bloom_config = input.bloom_config;

    auto result = co_await resolver(std::move(resolve_input));
    if (result.all_files.empty()) {
        co_return result;
    }

    // A forced rebuild rebuilds every file whole, so every tier is stale.
    dstd::HashSet<dstd::String> stale_tiers(
        result.stale_aggregation_roots.begin(),
        result.stale_aggregation_roots.end());
    if (input.force_rebuild)
        for (const auto& file : result.all_files) {
            auto root =
                trace::internal::determine_index_path(file, input.index_dir);
            const dstd::String store_path = index::store_dir(root);
            auto checking = runtime::fs::exists(dstd::PathView(store_path));
            const bool has_store = common::unwrap(co_await std::move(checking));
            if (has_store) stale_tiers.insert(std::move(root));
        }
    for (const auto& root : stale_tiers) clear_aggregation(root);

    dstd::Vector<dstd::String> files_needing_work;
    if (input.force_rebuild) {
        files_needing_work = result.all_files;
    } else {
        dstd::Set<dstd::String> files_needing_work_set;
        for (const auto& item : result.needs_checkpoint) {
            files_needing_work_set.insert(item.file_path);
        }
        files_needing_work.assign(files_needing_work_set.begin(),
                                  files_needing_work_set.end());

        // One tier build per recorded schema, which decodes its files.
        dstd::Map<dstd::String, dstd::Pair<dstd::Vector<dstd::String>,
                                           index::store::ExtensionMask>>
            tier_by_schema;
        for (const auto& item : result.needs_bloom) {
            auto& [files, tier] = tier_by_schema[item.schema];
            files.push_back(item.file_path);
            tier.add(item.extensions);
            tier.add(input.rebuild_extensions);
        }
        if (!input.rebuild_extensions.empty())
            for (const auto& item : result.cached) {
                auto& [files, tier] = tier_by_schema[item.schema];
                files.push_back(item.file_path);
                tier.add(input.rebuild_extensions);
            }
        for (auto& [schema, group] : tier_by_schema) {
            auto failures =
                co_await build_tier(std::move(group.first), input, group.second,
                                    schema, parallelism);
            for (auto& f : failures) result.failures.push_back(std::move(f));
        }

        dstd::Map<dstd::String, dstd::Vector<dstd::String>> agg_by_schema;
        for (const auto& item : result.needs_aggregation)
            agg_by_schema[item.schema].push_back(item.file_path);
        for (auto& [schema, files] : agg_by_schema) {
            auto failures = co_await build_aggregation(std::move(files), input,
                                                       result.index_path,
                                                       schema, parallelism);
            for (auto& f : failures) result.failures.push_back(std::move(f));
        }
    }

    if (!files_needing_work.empty()) {
        DFTRACER_UTILS_LOG_INFO(
            "Building index for {} files (checkpoint: {}, aggregation: {})",
            files_needing_work.size(), result.needs_checkpoint.size(),
            result.needs_aggregation.size());

        // Build a fresh single-root index on node-local scratch when the
        // destination is a slow filesystem, then publish it back. Incremental
        // and multi-root builds run in place.
        dstd::String build_index_dir = input.index_dir;
        dstd::String build_index_path = result.index_path;
        dstd::String staged_final_path;
        dstd::Optional<common::ScratchSession> scratch;
        // should_stage is cheap for the common local destination and short
        // circuits before any mount probing; only a network destination reaches
        // the single-root and size checks below.
        const dstd::String result_store_path =
            index::store_dir(result.index_path);
        auto checking_store =
            runtime::fs::exists(dstd::PathView(result_store_path));
        const bool has_result_store =
            common::unwrap(co_await std::move(checking_store));
        if (!has_result_store && common::should_stage(result.index_path)) {
            dstd::HashSet<dstd::String> roots;
            for (const auto& f : files_needing_work)
                roots.insert(
                    trace::internal::determine_index_path(f, input.index_dir));
            if (roots.size() == 1) {
                auto staged = common::staged_build_scratch(files_needing_work);
                if (staged) {
                    staged_final_path = *roots.begin();
                    build_index_dir = dstd::String(staged->dir().str());
                    build_index_path = trace::internal::determine_index_path(
                        files_needing_work.front(), build_index_dir);
                    DFTRACER_UTILS_LOG_INFO("Staging index build at {} -> {}",
                                            build_index_path,
                                            staged_final_path);
                    scratch.emplace(std::move(*staged));
                }
            }
        }
        const bool staging = scratch && scratch->valid();

        dstd::Optional<AggRun> agg_run{
            start_aggregation(build_index_path, input.aggregation_config)};

        auto agg_factory = std::move(agg_run->factory);
        const auto make_batch_config = [&]() {
            auto batch_config =
                dstd::make_shared<index::build::IndexBuildBatchConfig>();
            batch_config->file_paths = files_needing_work;
            if (result.all_file_sizes.size() == result.all_files.size()) {
                dstd::HashMap<dstd::StringView, std::uint64_t> size_of;
                size_of.reserve(result.all_files.size());
                for (std::size_t i = 0; i < result.all_files.size(); ++i)
                    size_of.emplace(result.all_files[i],
                                    result.all_file_sizes[i]);
                for (const auto& f : files_needing_work)
                    batch_config->file_costs.push_back(
                        size_of[dstd::StringView(f)]);
            }
            batch_config->index_dir = build_index_dir;
            batch_config->checkpoint_size = input.checkpoint_size;
            batch_config->parallelism = parallelism;
            batch_config->force_rebuild = input.force_rebuild;
            batch_config->whole_members = input.whole_members;
            batch_config->build_bloom = input.build_bloom;
            batch_config->memory_budget = input.memory_budget;
            batch_config->schema = input.schema;
            batch_config->bloom_config = input.bloom_config;
            batch_config->bloom_dimensions.assign(
                index::build::DEFAULT_BLOOM_DIMENSIONS.begin(),
                index::build::DEFAULT_BLOOM_DIMENSIONS.end());
            batch_config->bloom_dimensions.insert(
                batch_config->bloom_dimensions.end(),
                input.bloom_config.extra_dimensions.begin(),
                input.bloom_config.extra_dimensions.end());

            batch_config->agg_fold_factory =
                [factory = &agg_factory](dstd::StringIntern& intern) {
                    return (*factory)(intern);
                };
            return batch_config;
        };
        auto batch_config = make_batch_config();

        auto batch_result = co_await index::build::BatchBuilder::process(
            std::move(batch_config));
        // New members with a false header or a hole cost a second inflate here
        // only: the walk keeps those before the first bad one.
        auto walk_config = make_batch_config();
        walk_config->file_paths.clear();
        walk_config->file_costs.clear();
        walk_config->walk_members = true;
        for (const auto& r : batch_result.results)
            if (r.needs_member_walk)
                walk_config->file_paths.push_back(r.file_path);
        if (!walk_config->file_paths.empty()) {
            auto walked = co_await index::build::BatchBuilder::process(
                std::move(walk_config));
            for (auto& r : walked.results) {
                if (!r.success && !r.was_skipped)
                    result.failures.push_back({r.file_path, r.error_message});
                if (r.was_appended) ++result.appended_count;
            }
            for (auto& out : walked.agg_outputs)
                batch_result.agg_outputs.push_back(std::move(out));
        }
        for (const auto& r : batch_result.results) {
            if (!r.success && !r.was_skipped && !r.needs_member_walk)
                result.failures.push_back({r.file_path, r.error_message});
            if (r.was_appended) ++result.appended_count;
        }

        finish_aggregation(*agg_run, batch_result);

        // Publish once every handle to the staged store is closed, so the
        // re-resolve below reads the destination.
        if (staging) {
            agg_run.reset();
            index::release_root(build_index_path);
            index::release_root(staged_final_path);
            common::unwrap(common::publish_path(
                dstd::PathView(index::store_dir(build_index_path)),
                dstd::PathView(index::store_dir(staged_final_path))));
        }

        // Refresh_input queries the index for the files just indexed.
        ResolverInput refresh_input;
        refresh_input.entries =
            scanned_entries(result, result.needs_checkpoint);
        refresh_input.index_dir = input.index_dir;
        refresh_input.require_checkpoints = true;
        refresh_input.tail_is_current = true;

        if (!refresh_input.entries.empty()) {
            auto refresh_result = co_await resolver(refresh_input);

            for (auto& entry : refresh_result.cached) {
                result.cached.push_back(std::move(entry));
            }

            result.needs_checkpoint =
                std::move(refresh_result.needs_checkpoint);
        }

        result.needs_aggregation.clear();
    }

    DFTRACER_UTILS_LOG_INFO(
        "Resolve complete: {} total, {} cached, {} failed checkpoint",
        result.all_files.size(), result.cached.size(),
        result.needs_checkpoint.size());

    co_return result;
}

}  // namespace

coro::CoroTask<IndexWork> files_needing_index(ResolverInput input) {
    Resolver resolver;
    auto result = co_await resolver(std::move(input));
    dstd::Vector<FileWorkItem> items;
    dstd::HashSet<std::size_t> seen;
    for (const auto* group : {&result.needs_checkpoint, &result.needs_bloom,
                              &result.needs_aggregation})
        for (const auto& item : *group)
            if (seen.insert(item.file_index).second) items.push_back(item);
    IndexWork work;
    work.files = scanned_entries(result, items);
    work.stale_aggregation_roots = std::move(result.stale_aggregation_roots);
    co_return work;
}

coro::CoroTask<void> ensure_indexes_fresh(dstd::String directory,
                                          dstd::Vector<dstd::String> files,
                                          dstd::String index_dir,
                                          bool force_rebuild) {
    ResolveAndBuildInput in;
    in.directory = std::move(directory);
    in.files = std::move(files);
    in.index_dir = std::move(index_dir);
    in.require_checkpoints = true;
    in.force_rebuild = force_rebuild;
    co_await resolve_and_build_index(std::move(in));
}

coro::CoroTask<void> ensure_index_fresh(dstd::String directory,
                                        dstd::String file,
                                        dstd::String index_dir,
                                        bool force_rebuild) {
    dstd::Vector<dstd::String> files;
    if (!file.empty()) files.push_back(std::move(file));
    co_await ensure_indexes_fresh(std::move(directory), std::move(files),
                                  std::move(index_dir), force_rebuild);
}

namespace {

bool mentions_corruption(dstd::StringView text) {
    return text.find("is corrupt") != dstd::StringView::npos;
}

[[noreturn]] void throw_corrupt(const dstd::Set<dstd::String>& roots,
                                const dstd::String& cause, bool tier_cleared,
                                common::ErrorCode code) {
    throw common::DFTUtilsException(
        code, corrupt_index_message(roots, cause, tier_cleared));
}

}  // namespace

// A corrupt aggregation tier holds only data derived from the traces, so the
// build clears it and aggregates again, once. Corruption that persists reaches
// the caller with the index directories named. Any other error is passed on.
coro::CoroTask<ResolverResult> resolve_and_build_index(
    ResolveAndBuildInput input) {
    dstd::Set<dstd::String> roots;
    for (const auto& file : input.files)
        roots.insert(
            trace::internal::determine_index_path(file, input.index_dir));
    for (const auto& entry : input.entries)
        roots.insert(
            trace::internal::determine_index_path(entry.path, input.index_dir));
    dstd::String failure;
    try {
        co_return co_await resolve_and_build_once(input);
    } catch (const std::exception& e) {
        if (!mentions_corruption(e.what())) throw;
        failure = e.what();
    }
    DFTRACER_UTILS_LOG_WARN(
        "the aggregation tier of {} index(es) is corrupt ({}); clearing it and "
        "aggregating again",
        roots.size(), failure);
    for (const auto& root : roots) {
        const dstd::String store_path = index::store_dir(root);
        auto checking = runtime::fs::exists(dstd::PathView(store_path));
        const bool has_store = common::unwrap(co_await std::move(checking));
        if (has_store) {
            index::release_root(root);
            auto target = common::unwrap(
                index::Index::open(index::normalize_index_root(root)));
            index::schemas::dft::agg::tier::clear(target);
        }
    }
    dstd::String retry_failure;
    try {
        co_return co_await resolve_and_build_once(std::move(input));
    } catch (const std::exception& e) {
        if (!mentions_corruption(e.what())) throw;
        retry_failure = e.what();
    }
    throw_corrupt(roots, retry_failure, true, common::ErrorCode::Aggregation);
}

}  // namespace dftracer::utils::index::build
