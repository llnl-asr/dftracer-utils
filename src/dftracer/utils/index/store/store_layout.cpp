#include <dftracer/utils/index/store/store_layout.h>

#include <string>

namespace dftracer::utils::index::store::store_layout {

namespace {

constexpr std::size_t KIND_PREFIX_BYTES = 3;
constexpr std::size_t MANIFEST_EXT_OFFSET = 4;
constexpr std::size_t MANIFEST_EXT_BYTES = 2;

bool is_data_kind(layout::Ext ext, std::uint8_t kind) {
    for (const layout::KindSpec& spec : layout::kinds()) {
        if (spec.ext == ext && spec.kind == kind) return true;
    }
    return false;
}

/// A granule key carries the plugin up to a terminator. Other plugin keys carry
/// the whole tail.
std::string plugin_of(std::uint8_t kind, std::string_view tail) {
    if (kind != layout::path_kind::DATA) return std::string(tail);
    return std::string(tail.substr(0, tail.find('\0')));
}

}  // namespace

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

std::uint32_t namespace_version(layout::Ext ext) {
    return layout::ext_version(ext);
}

std::optional<KeyTarget> target_of(std::string_view key) {
    if (key.size() < layout::KEY_PREFIX_BYTES) return std::nullopt;
    const auto ext =
        static_cast<layout::Ext>(layout::read_u16(key.substr(0, 2)));
    if (ext == layout::Ext::Host) return std::nullopt;
    const auto kind = static_cast<std::uint8_t>(key[2]);
    if (!is_data_kind(ext, kind)) return std::nullopt;
    KeyTarget target;
    target.ext = ext;
    target.kind = kind;
    target.file_id = layout::read_u32(key.substr(3, 4));
    const std::string_view tail = key.substr(layout::KEY_PREFIX_BYTES);
    if (ext == layout::Ext::Plugin) target.plugin = plugin_of(kind, tail);
    if (ext == layout::Ext::Agg && kind == layout::agg_kind::META)
        target.meta = std::string(tail);
    target.inner_key = std::string(1, static_cast<char>(kind));
    target.inner_key.append(tail);
    return target;
}

std::optional<HostTarget> host_target_of(std::string_view key) {
    if (key.size() < KIND_PREFIX_BYTES) return std::nullopt;
    if (layout::read_u16(key.substr(0, 2)) !=
        static_cast<std::uint16_t>(layout::Ext::Host)) {
        return std::nullopt;
    }
    HostTarget target;
    const std::string_view tail = key.substr(KIND_PREFIX_BYTES);
    switch (static_cast<std::uint8_t>(key[2])) {
        case layout::host::FORMAT:
            target.kind = HostKind::Format;
            return target;
        case layout::host::EXT:
            target.kind = HostKind::ExtRegistry;
            target.ext = parse_extension(tail);
            return target;
        case layout::host::FILE_BY_PATH:
            target.kind = HostKind::FileByPath;
            target.path = std::string(tail);
            return target;
        case layout::host::FILE_BY_ID:
            if (tail.size() < 4) return std::nullopt;
            target.kind = HostKind::FileById;
            target.file_id = layout::read_u32(tail);
            return target;
        case layout::host::NEXT_FILE_ID:
            target.kind = HostKind::NextFileId;
            return target;
        case layout::host::MANIFEST: {
            if (tail.size() < MANIFEST_EXT_OFFSET + MANIFEST_EXT_BYTES) {
                return std::nullopt;
            }
            target.file_id = layout::read_u32(tail);
            const auto ext = static_cast<layout::Ext>(layout::read_u16(
                tail.substr(MANIFEST_EXT_OFFSET, MANIFEST_EXT_BYTES)));
            target.ext = ext;
            if (ext != layout::Ext::Plugin) {
                target.kind = HostKind::Manifest;
                return target;
            }
            target.kind = HostKind::PluginManifest;
            std::string_view name =
                tail.substr(MANIFEST_EXT_OFFSET + MANIFEST_EXT_BYTES);
            if (!name.empty() && name.back() == '\0') name.remove_suffix(1);
            target.plugin = std::string(name);
            return target;
        }
        default:
            return std::nullopt;
    }
}

}  // namespace dftracer::utils::index::store::store_layout
