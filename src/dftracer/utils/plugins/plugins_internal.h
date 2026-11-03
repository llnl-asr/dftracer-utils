// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_PLUGINS_INTERNAL_H
#define DFTRACER_UTILS_PLUGINS_PLUGINS_INTERNAL_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/plugins/plugins.h>

#include <cstddef>

struct dftu_plugin;

// The parts of the plugin set that speak the C ABI: kept out of the public
// header so plugins.h stays ABI-free.
namespace dftracer::utils::plugins {

namespace detail {

/// Union of the loaded plugins' plan_duql strings for coarse index pruning:
/// the weakest predicate that still selects every event any plugin's own filter
/// keeps, so per-fold results are unchanged and only chunks/members no plugin
/// wants are skipped. `plan_queries` is one entry per plugin (nullptr/empty =
/// that plugin declared none). nullopt means match all (prune nothing): a
/// plugin wants every event, or a query failed to parse. Identical strings are
/// deduplicated so the union never repeats a term.
dstd::Optional<duql::Query> plugin_union_prune_query(
    const dstd::Vector<const char*>& plan_queries);

}  // namespace detail

/// Test seam: build a set from already-built plugins, skipping dlopen and the
/// ABI gate. The caller keeps ownership of each struct (no destroy, no dlclose
/// runs for it) and must outlive the set.
dstd::Result<Plugins> build_injected_plugins(
    dstd::Vector<dftu_plugin*> plugins);

/// The fold order build() computed: a permutation of plugin indices with every
/// name's provider before the plugins that consume it. Independent plugins keep
/// their registration order.
dstd::Vector<std::size_t> fold_order(const Plugins& set);

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_PLUGINS_INTERNAL_H
