#include <dftracer/utils/index/store/store_layout.h>

#include <string>

namespace dftracer::utils::index::store::store_layout {

std::string namespace_name(layout::Ext ext) {
    std::string name(NAMESPACE_PREFIX);
    name.append(layout::ext_name(ext));
    return name;
}

std::string plugin_namespace_name(std::string_view plugin) {
    std::string name(PLUGIN_NAMESPACE_PREFIX);
    name.append(plugin);
    return name;
}

std::string agg_meta_namespace_name(std::string_view record) {
    std::string name(AGG_META_NAMESPACE_PREFIX);
    name.append(record);
    return name;
}

}  // namespace dftracer::utils::index::store::store_layout
