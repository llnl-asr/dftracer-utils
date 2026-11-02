// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_CACHE_ROLLUP_STORE_H
#define DFTRACER_UTILS_INDEX_CACHE_ROLLUP_STORE_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/index/cache/cache_store.h>
#include <dftracer/utils/trace/views/view_aggregate.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::cache {

/// Open (or reuse) the rollup store at `path` (see rollup_cache_path). A
/// ReadWrite open creates the folder. Null when the store cannot be opened, or
/// for a ReadOnly open of a folder with no store. Never throws.
std::shared_ptr<CacheStore> open_rollup_db(const std::string& path,
                                           CacheStore::Mode mode);

/// Persist the engine partial `state` as the rollup identified by `sig`, in
/// one write: one row per group holding that group's serialized AggState
/// (agg_extract_group + agg_serialize, the same blob spill writes), its usage
/// record, and a descriptor recording the
/// view's shape (`rest_sig` = the plan hash excluding group_by,
/// `time_bucket_us` = the rollup's time grain for coarsening, and `group_by`
/// itself) so the planner can test subsumption. `state`'s key layout must be
/// [time_bucket?, group_by...]; a read-back re-aggregates the per-group
/// partials with agg_merge / agg_regroup. Then evicts the least recently used
/// rollups until the store holds at most cache_max_bytes().
void persist_rollup(CacheStore& db, std::uint64_t sig, std::uint64_t rest_sig,
                    std::uint64_t time_bucket_us,
                    const std::vector<trace::views::GroupKey>& group_by,
                    const dftracer::utils::dataframe::AggState& state);

/// True if a rollup for `sig` has been persisted.
bool rollup_exists(const CacheStore& db, std::uint64_t sig);

/// Read every per-group blob for `sig` back into one merged fine-grain AggState
/// (key layout [time_bucket?, group_by...]), or null if the rollup is empty.
dftracer::utils::dataframe::AggStatePtr read_rollup(const CacheStore& db,
                                                    std::uint64_t sig);

/// A materialized view's identity: a stable hash over the plan's file set,
/// record schema and query shape. Per-rank scans of the same plan share it, so
/// their partials land under the same rollup key. Throws INVALID_ARGUMENT
/// until the plan's record schema is resolved (plan_record_schema).
std::uint64_t plan_signature(const trace::views::detail::ViewPlan& plan);

/// The plan hash EXCLUDING group_by AND time_bucket: two views with the same
/// rest_signature differ only in grouping and/or time grain, so one can serve
/// the other by re-aggregating (and re-bucketing to a coarser grain).
std::uint64_t rest_signature(const trace::views::detail::ViewPlan& plan);

/// Find a stored rollup that subsumes `plan`, re-aggregate its AggState
/// partials to `plan`'s grouping (agg_regroup + agg_merge), and finalize to the
/// result DataFrame (finalize_engine_result), or nullopt. A rollup R subsumes
/// plan Q when they share a rest_signature (same files/filter/agg/window) and
/// Q's group keys are a subset of R's - then Q is R rolled up over the dropped
/// dimensions. Exact match is the identity case. This is the materialized-view
/// query rewrite. Marks the rollup it serves as used when `db` is writable.
std::optional<dftracer::utils::dataframe::DataFrame> find_subsuming_rollup(
    CacheStore& db, const trace::views::detail::ViewPlan& plan);

/// The index root `plan`'s files share; empty when they share none, or for a
/// plan a cached result cannot serve (a rank key).
std::string cache_dir(const trace::views::detail::ViewPlan& plan);

/// Where `plan`'s rollups live: plan.rollup_root, else `cache_dir/rollups`.
/// Empty when there is none.
std::string rollup_cache_path(const trace::views::detail::ViewPlan& plan);

}  // namespace dftracer::utils::index::cache

#endif  // DFTRACER_UTILS_INDEX_CACHE_ROLLUP_STORE_H
