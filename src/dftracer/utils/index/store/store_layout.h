// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_STORE_LAYOUT_H
#define DFTRACER_UTILS_INDEX_STORE_STORE_LAYOUT_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/layout.h>

#include <cstdint>
#include <string>
#include <string_view>

// Maps the index keys of index/store/layout onto the store, one namespace per
// extension and one scope per file. Pure mapping, no I/O.
namespace dftracer::utils::index::store::store_layout {

std::string namespace_name(layout::Ext ext);

inline constexpr dstd::StringView NAMESPACE_PREFIX = "dftu.idx.";

inline constexpr dstd::StringView PLUGIN_NAMESPACE_PREFIX = "dftu.idx.plugin.";

/// Every plugin has its own namespace, so rebuilding one never hides another's
/// data.
std::string plugin_namespace_name(dstd::StringView plugin);

inline constexpr dstd::StringView AGG_META_NAMESPACE_PREFIX = "dftu.idx.agg.";

/// Every record has its own namespace, so writing one never hides another.
std::string agg_meta_namespace_name(dstd::StringView record);

}  // namespace dftracer::utils::index::store::store_layout

#endif  // DFTRACER_UTILS_INDEX_STORE_STORE_LAYOUT_H
