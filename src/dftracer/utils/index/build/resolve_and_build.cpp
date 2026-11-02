// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/common/scratch.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/build/aggregation_run.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/corrupt_index.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_drain.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_fold.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace dftracer::utils::index::build {

namespace {

// Rewrites only `tier` for files whose members are current, leaving their
// members, hash tables and aggregation as they are. `tier` holds the union of
// the files' stale extensions; rewriting a current one is correct, only
// slower.
coro::CoroTask<std::vector<ResolverResult::Failure>> build_tier(
    CoroScope* scope, std::vector<std::string> files,
    const ResolveAndBuildInput& input, index::store::ExtensionMask tier,
    const std::string& schema, std::size_t parallelism) {
    auto config = std::make_shared<IndexBuildBatchConfig>();
    config->file_paths = std::move(files);
    config->index_dir = input.index_dir;
    config->checkpoint_size = input.checkpoint_size;
    config->parallelism = parallelism;
    config->tier_only = true;
    config->memory_budget = input.memory_budget;
    config->schema = schema;
    config->bloom_config = input.bloom_config;
    config->bloom_config.extensions = tier;
    DFTRACER_UTILS_LOG_INFO("Building the index tier for %zu files",
                            config->file_paths.size());
    auto batch = co_await BatchBuilder::process(scope, std::move(config));
    std::vector<ResolverResult::Failure> failures;
    for (const auto& r : batch.results)
        if (!r.success && !r.was_skipped)
            failures.push_back({r.file_path, r.error_message});
    co_return failures;
}

namespace agg = index::schemas::dft::agg;

// Aggregates files whose members are current, leaving every other extension.
coro::CoroTask<std::vector<ResolverResult::Failure>> build_aggregation(
    CoroScope* scope, std::vector<std::string> files,
    const ResolveAndBuildInput& input, const std::string& index_path,
    const std::string& schema, std::size_t parallelism) {
    AggRun run = start_aggregation(index_path, input.aggregation_config);
    auto config = std::make_shared<IndexBuildBatchConfig>();
    config->file_paths = std::move(files);
    config->index_dir = input.index_dir;
    config->checkpoint_size = input.checkpoint_size;
    config->parallelism = parallelism;
    config->tier_only = true;
    config->build_bloom = false;
    config->memory_budget = input.memory_budget;
    config->schema = schema;
    config->agg_fold_factory = run.factory;
    DFTRACER_UTILS_LOG_INFO("Aggregating %zu files", config->file_paths.size());
    auto batch = co_await BatchBuilder::process(scope, std::move(config));
    finish_aggregation(run, batch);
    std::vector<ResolverResult::Failure> failures;
    for (const auto& r : batch.results)
        if (!r.success && !r.was_skipped)
            failures.push_back({r.file_path, r.error_message});
    co_return failures;
}

coro::CoroTask<ResolverResult> resolve_and_build_once(
    CoroScope* scope, ResolveAndBuildInput input) {
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
    for (std::string& field : input.bloom_config.extra_dimensions)
        field = extra_dimension_name(field);
    if (input.build_bloom) resolve_input.bloom_config = input.bloom_config;

    auto result = co_await resolver(std::move(resolve_input));
    if (result.all_files.empty()) {
        co_return result;
    }

    // A forced rebuild rebuilds every file whole, so every tier is stale.
    std::set<std::string> stale_tiers(result.stale_aggregation_roots.begin(),
                                      result.stale_aggregation_roots.end());
    if (input.force_rebuild)
        for (const auto& file : result.all_files) {
            auto root =
                trace::internal::determine_index_path(file, input.index_dir);
            if (common::fs::exists(index::store_dir(root)))
                stale_tiers.insert(std::move(root));
        }
    for (const auto& root : stale_tiers) clear_aggregation(root);

    std::vector<std::string> files_needing_work;
    if (input.force_rebuild) {
        files_needing_work = result.all_files;
    } else {
        std::set<std::string> files_needing_work_set;
        for (const auto& item : result.needs_checkpoint) {
            files_needing_work_set.insert(item.file_path);
        }
        files_needing_work.assign(files_needing_work_set.begin(),
                                  files_needing_work_set.end());

        // One tier build per recorded schema, which decodes its files.
        std::map<std::string, std::pair<std::vector<std::string>,
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
                co_await build_tier(scope, std::move(group.first), input,
                                    group.second, schema, parallelism);
            for (auto& f : failures) result.failures.push_back(std::move(f));
        }

        std::map<std::string, std::vector<std::string>> agg_by_schema;
        for (const auto& item : result.needs_aggregation)
            agg_by_schema[item.schema].push_back(item.file_path);
        for (auto& [schema, files] : agg_by_schema) {
            auto failures = co_await build_aggregation(scope, std::move(files),
                                                       input, result.index_path,
                                                       schema, parallelism);
            for (auto& f : failures) result.failures.push_back(std::move(f));
        }
    }

    if (!files_needing_work.empty()) {
        DFTRACER_UTILS_LOG_INFO(
            "Building index for %zu files (checkpoint: %zu, aggregation: %zu)",
            files_needing_work.size(), result.needs_checkpoint.size(),
            result.needs_aggregation.size());

        // Build a fresh single-root index on node-local scratch when the
        // destination is a slow filesystem, then publish it back. Incremental
        // and multi-root builds run in place.
        std::string build_index_dir = input.index_dir;
        std::string build_index_path = result.index_path;
        std::string staged_final_path;
        std::optional<common::ScratchSession> scratch;
        // should_stage is cheap for the common local destination and short
        // circuits before any mount probing; only a network destination reaches
        // the single-root and size checks below.
        if (!common::fs::exists(index::store_dir(result.index_path)) &&
            common::should_stage(result.index_path)) {
            std::set<std::string> roots;
            for (const auto& f : files_needing_work)
                roots.insert(
                    trace::internal::determine_index_path(f, input.index_dir));
            if (roots.size() == 1) {
                auto staged = common::staged_build_scratch(files_needing_work);
                if (staged) {
                    staged_final_path = *roots.begin();
                    build_index_dir = staged->dir();
                    build_index_path = trace::internal::determine_index_path(
                        files_needing_work.front(), build_index_dir);
                    DFTRACER_UTILS_LOG_INFO("Staging index build at %s -> %s",
                                            build_index_path.c_str(),
                                            staged_final_path.c_str());
                    scratch.emplace(std::move(*staged));
                }
            }
        }
        const bool staging = scratch && scratch->valid();

        std::optional<AggRun> agg_run{
            start_aggregation(build_index_path, input.aggregation_config)};

        auto batch_config =
            std::make_shared<index::build::IndexBuildBatchConfig>();
        batch_config->file_paths = files_needing_work;
        if (result.all_file_sizes.size() == result.all_files.size()) {
            dstd::HashMap<std::string_view, std::uint64_t> size_of;
            size_of.reserve(result.all_files.size());
            for (std::size_t i = 0; i < result.all_files.size(); ++i)
                size_of.emplace(result.all_files[i], result.all_file_sizes[i]);
            for (const auto& f : files_needing_work)
                batch_config->file_costs.push_back(
                    size_of[std::string_view(f)]);
        }
        batch_config->index_dir = build_index_dir;
        batch_config->checkpoint_size = input.checkpoint_size;
        batch_config->parallelism = parallelism;
        batch_config->force_rebuild = input.force_rebuild;
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

        batch_config->agg_fold_factory = agg_run->factory;

        auto batch_result = co_await index::build::BatchBuilder::process(
            scope, std::move(batch_config));
        for (const auto& r : batch_result.results) {
            if (!r.success && !r.was_skipped)
                result.failures.push_back({r.file_path, r.error_message});
        }

        finish_aggregation(*agg_run, batch_result);

        // Publish once every handle to the staged store is closed, so the
        // re-resolve below reads the destination.
        if (staging) {
            agg_run.reset();
            index::release_root(build_index_path);
            index::release_root(staged_final_path);
            common::publish_path(index::store_dir(build_index_path),
                                 index::store_dir(staged_final_path));
        }

        // Refresh_input queries the index for the files just indexed.
        ResolverInput refresh_input;
        refresh_input.entries =
            scanned_entries(result, result.needs_checkpoint);
        refresh_input.index_dir = input.index_dir;
        refresh_input.require_checkpoints = true;

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
        "Resolve complete: %zu total, %zu cached, %zu failed checkpoint",
        result.all_files.size(), result.cached.size(),
        result.needs_checkpoint.size());

    co_return result;
}

}  // namespace

coro::CoroTask<IndexWork> files_needing_index(ResolverInput input) {
    Resolver resolver;
    auto result = co_await resolver(std::move(input));
    std::vector<FileWorkItem> items;
    std::set<std::size_t> seen;
    for (const auto* group : {&result.needs_checkpoint, &result.needs_bloom,
                              &result.needs_aggregation})
        for (const auto& item : *group)
            if (seen.insert(item.file_index).second) items.push_back(item);
    IndexWork work;
    work.files = scanned_entries(result, items);
    work.stale_aggregation_roots = std::move(result.stale_aggregation_roots);
    co_return work;
}

coro::CoroTask<void> ensure_indexes_fresh(CoroScope* scope,
                                          std::string directory,
                                          std::vector<std::string> files,
                                          std::string index_dir,
                                          bool force_rebuild) {
    ResolveAndBuildInput in;
    in.directory = std::move(directory);
    in.files = std::move(files);
    in.index_dir = std::move(index_dir);
    in.require_checkpoints = true;
    in.force_rebuild = force_rebuild;
    co_await resolve_and_build_index(scope, std::move(in));
}

coro::CoroTask<void> ensure_index_fresh(CoroScope* scope, std::string directory,
                                        std::string file, std::string index_dir,
                                        bool force_rebuild) {
    std::vector<std::string> files;
    if (!file.empty()) files.push_back(std::move(file));
    co_await ensure_indexes_fresh(scope, std::move(directory), std::move(files),
                                  std::move(index_dir), force_rebuild);
}

namespace {

bool mentions_corruption(std::string_view text) {
    return text.find("is corrupt") != std::string_view::npos;
}

[[noreturn]] void throw_corrupt(const std::set<std::string>& roots,
                                const std::string& cause, bool tier_cleared,
                                common::ErrorCode code) {
    throw common::DFTUtilsException(
        code, corrupt_index_message(roots, cause, tier_cleared));
}

}  // namespace

// A corrupt aggregation tier holds only data derived from the traces, so the
// build clears it and aggregates again, once. Corruption that persists reaches
// the caller with the index directories named. Any other error is passed on.
coro::CoroTask<ResolverResult> resolve_and_build_index(
    CoroScope* scope, ResolveAndBuildInput input) {
    std::set<std::string> roots;
    for (const auto& file : input.files)
        roots.insert(
            trace::internal::determine_index_path(file, input.index_dir));
    for (const auto& entry : input.entries)
        roots.insert(trace::internal::determine_index_path(entry.path.string(),
                                                           input.index_dir));
    std::string failure;
    try {
        co_return co_await resolve_and_build_once(scope, input);
    } catch (const std::exception& e) {
        if (!mentions_corruption(e.what())) throw;
        failure = e.what();
    }
    DFTRACER_UTILS_LOG_WARN(
        "the aggregation tier of %zu index(es) is corrupt (%s); clearing it "
        "and aggregating again",
        roots.size(), failure.c_str());
    for (const auto& root : roots)
        if (common::fs::exists(index::store_dir(root))) {
            index::release_root(root);
            auto target = common::unwrap(index::Index::open(
                index::store::internal::normalize_index_root(root)));
            agg::tier::clear(target);
        }
    std::string retry_failure;
    try {
        co_return co_await resolve_and_build_once(scope, std::move(input));
    } catch (const std::exception& e) {
        if (!mentions_corruption(e.what())) throw;
        retry_failure = e.what();
    }
    throw_corrupt(roots, retry_failure, true, common::ErrorCode::Aggregation);
}

}  // namespace dftracer::utils::index::build
