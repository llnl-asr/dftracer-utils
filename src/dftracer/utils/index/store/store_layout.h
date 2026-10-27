#ifndef DFTRACER_UTILS_INDEX_STORE_STORE_LAYOUT_H
#define DFTRACER_UTILS_INDEX_STORE_STORE_LAYOUT_H

#include <dftracer/utils/index/store/layout.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// How the index keys of index/store/layout map onto the store: one namespace
// per extension, one scope per file, and the key without its extension and
// file id as the key inside that scope's copy. Pure mapping, no I/O.
namespace dftracer::utils::index::store::store_layout {

/// The store namespace holding the data of `ext`: "dftu.idx." and its name.
std::string namespace_name(layout::Ext ext);

/// The store namespace holding the data of plugin extension `plugin`. Every
/// plugin has its own, so rebuilding one never hides another's data.
std::string plugin_namespace_name(std::string_view plugin);

/// The format version of the namespace of `ext`. A plugin namespace is at the
/// version its manifest entry carries, so it has no fixed version here.
std::uint32_t namespace_version(layout::Ext ext);

/// Where one data key lands in the store.
struct KeyTarget {
    layout::Ext ext = layout::Ext::Host;
    std::uint8_t kind = 0;
    std::uint32_t file_id = layout::INDEX_WIDE;
    /// The plugin whose namespace holds the key; empty for every other
    /// extension.
    std::string plugin;
    /// The key inside the copy: [kind] and the tail of the key, which is the
    /// key without its extension and file id.
    std::string inner_key;
};

/// The target of the data key `key`; nullopt for a host key, for an (extension,
/// kind) pair the index does not write, and for a key shorter than its prefix.
std::optional<KeyTarget> target_of(std::string_view key);

/// What a host key names.
enum class HostKind : std::uint8_t {
    Format,
    ExtRegistry,
    FileByPath,
    FileById,
    NextFileId,
    Manifest,
    PluginManifest,
};

/// The kind of a host key and what it carries.
struct HostTarget {
    HostKind kind = HostKind::Format;
    /// The file a FileById, a Manifest or a PluginManifest names; 0 otherwise.
    std::uint32_t file_id = 0;
    /// The extension an ExtRegistry lists or a Manifest belongs to; nullopt
    /// when the name or the extension is unknown.
    std::optional<layout::Ext> ext;
    /// The plugin of a PluginManifest key, without its terminator; empty for a
    /// range that covers every plugin of a file.
    std::string plugin;
    /// The logical path of a FileByPath key.
    std::string path;
};

/// The target of the host key `key`; nullopt for a data key, for an unknown
/// host kind and for a key shorter than its prefix.
std::optional<HostTarget> host_target_of(std::string_view key);

}  // namespace dftracer::utils::index::store::store_layout

#endif  // DFTRACER_UTILS_INDEX_STORE_STORE_LAYOUT_H
