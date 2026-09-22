#ifndef DFTRACER_UTILS_PLUGINS_PLUGINS_INTERNAL_H
#define DFTRACER_UTILS_PLUGINS_PLUGINS_INTERNAL_H

#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/query/query.h>

#include <cstddef>
#include <optional>
#include <vector>

struct dftu_plugin;

// The parts of the plugin set that speak the C ABI: kept out of the public
// header so plugins.h stays ABI-free.
namespace dftracer::utils::plugins {

namespace detail {

/// Union of the loaded plugins' plan_query strings for coarse index pruning:
/// the weakest predicate that still selects every event any plugin's own filter
/// keeps, so per-fold results are unchanged and only chunks/members no plugin
/// wants are skipped. `plan_queries` is one entry per plugin (nullptr/empty =
/// that plugin declared none). nullopt means match all (prune nothing): a
/// plugin wants every event, or a query failed to parse. Identical strings are
/// deduplicated so the union never repeats a term.
std::optional<query::Query> plugin_union_prune_query(
    const std::vector<const char*>& plan_queries);

}  // namespace detail

/// Test seam: build a set from already-built plugins, skipping dlopen and the
/// ABI gate. The caller keeps ownership of each struct (no destroy, no dlclose
/// runs for it) and must outlive the set.
Result<Plugins> build_injected_plugins(std::vector<dftu_plugin*> plugins);

/// The fold order build() computed: a permutation of plugin indices with every
/// name's provider before the plugins that consume it. Independent plugins keep
/// their registration order.
std::vector<std::size_t> fold_order(const Plugins& set);

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_PLUGINS_INTERNAL_H
