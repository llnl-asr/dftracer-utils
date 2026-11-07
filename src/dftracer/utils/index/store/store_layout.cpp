// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/store_layout.h>

namespace dftracer::utils::index::store::store_layout {

dstd::String namespace_name(layout::Ext ext) {
    dstd::String name(NAMESPACE_PREFIX);
    name.append(layout::ext_name(ext));
    return name;
}

dstd::String plugin_namespace_name(dstd::StringView plugin) {
    dstd::String name(PLUGIN_NAMESPACE_PREFIX);
    name.append(plugin);
    return name;
}

dstd::String agg_meta_namespace_name(dstd::StringView record) {
    dstd::String name(AGG_META_NAMESPACE_PREFIX);
    name.append(record);
    return name;
}

}  // namespace dftracer::utils::index::store::store_layout
