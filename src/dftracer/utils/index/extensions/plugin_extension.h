// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_PLUGIN_EXTENSION_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_PLUGIN_EXTENSION_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/file.h>
#include <dftracer/utils/plugins/abi/index.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index {
class FileBuild;
}

namespace dftracer::utils::index::extensions {

/// An index extension a plugin registered (plugins/abi/index.h).
struct PluginExtension {
    std::string name;
    ::dftu_index_extension vt;
    void* self = nullptr;

    /// Built at this extension's version and params hash, and not failed.
    bool current(const store::ExtensionState& state) const {
        return state.ready && state.version == vt.version &&
               state.params_hash == vt.params_hash;
    }
    std::uint64_t max_payload() const;
};

using PluginExtensionPtr = dstd::SharedPtr<const PluginExtension>;

/// Adds `vt`/`self` under `name`. Returns false, logging why, when `vt` or a
/// required callback is NULL or the name is taken.
bool register_plugin_extension(dstd::StringView name,
                               const ::dftu_index_extension* vt, void* self);
void unregister_plugin_extension(dstd::StringView name);
/// The registered extensions, sorted by name. The entries stay valid after
/// unregistration, but their callbacks only while the plugin is loaded.
std::vector<PluginExtensionPtr> plugin_extensions();
PluginExtensionPtr find_plugin_extension(dstd::StringView name);

/// One file's builders, one per extension, fed chunk by chunk.
class PluginBuilders {
   public:
    explicit PluginBuilders(std::vector<PluginExtensionPtr> extensions);
    ~PluginBuilders();
    PluginBuilders(const PluginBuilders&) = delete;
    PluginBuilders& operator=(const PluginBuilders&) = delete;

    /// Record lines of chunk `granule`; a new granule ends the previous one.
    void step(std::uint64_t granule, dstd::Span<const dstd::StringView> lines);
    /// Ends the last chunk and the file.
    void finish();
    /// Writes each extension's data into `build`, which replaces the file's
    /// copy of it when committed.
    void write(FileBuild& build);

   private:
    struct Built {
        PluginExtensionPtr ext;
        void* builder = nullptr;
        bool chunk_failed = false;
        std::vector<dstd::Pair<std::uint64_t, std::string>> granules;
        dstd::Optional<std::string> file;
    };
    void end_granule();

    std::vector<Built> built_;
    dstd::Optional<std::uint64_t> granule_;
    std::vector<::dftu_bytes> lines_;
};

}  // namespace dftracer::utils::index::extensions

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_PLUGIN_EXTENSION_H
