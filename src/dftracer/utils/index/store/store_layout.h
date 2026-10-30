#ifndef DFTRACER_UTILS_INDEX_STORE_STORE_LAYOUT_H
#define DFTRACER_UTILS_INDEX_STORE_STORE_LAYOUT_H

#include <dftracer/utils/index/store/layout.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Maps the index keys of index/store/layout onto the store, one namespace per
// extension and one scope per file. Pure mapping, no I/O.
namespace dftracer::utils::index::store::store_layout {

std::string namespace_name(layout::Ext ext);

inline constexpr std::string_view PLUGIN_NAMESPACE_PREFIX = "dftu.idx.plugin.";

/// Every plugin has its own namespace, so rebuilding one never hides another's
/// data.
std::string plugin_namespace_name(std::string_view plugin);

inline constexpr std::string_view AGG_META_NAMESPACE_PREFIX = "dftu.idx.agg.";

/// Every record has its own namespace, so writing one never hides another.
std::string agg_meta_namespace_name(std::string_view record);

/// A plugin namespace is at the version its manifest entry carries, so it has
/// no fixed version here.
std::uint32_t namespace_version(layout::Ext ext);

struct KeyTarget {
    layout::Ext ext = layout::Ext::Host;
    std::uint8_t kind = 0;
    std::uint32_t file_id = layout::INDEX_WIDE;
    /// Empty for every extension but a plugin.
    std::string plugin;
    /// Empty for every key but an Ext::Agg META key.
    std::string meta;
    /// [kind] and the key without its extension and file id.
    std::string inner_key;
};

/// nullopt for a host key, for an (extension, kind) pair the index does not
/// write, and for a key shorter than its prefix.
std::optional<KeyTarget> target_of(std::string_view key);

enum class HostKind : std::uint8_t {
    Format,
    ExtRegistry,
    FileByPath,
    FileById,
    NextFileId,
    Manifest,
    PluginManifest,
};

struct HostTarget {
    HostKind kind = HostKind::Format;
    /// 0 except for a FileById, a Manifest or a PluginManifest.
    std::uint32_t file_id = 0;
    /// nullopt when the name or the extension is unknown.
    std::optional<layout::Ext> ext;
    /// Without the terminator. Empty for a range that covers every plugin of a
    /// file.
    std::string plugin;
    std::string path;
};

/// nullopt for a data key, for an unknown host kind and for a key shorter than
/// its prefix.
std::optional<HostTarget> host_target_of(std::string_view key);

}  // namespace dftracer::utils::index::store::store_layout

#endif  // DFTRACER_UTILS_INDEX_STORE_STORE_LAYOUT_H
