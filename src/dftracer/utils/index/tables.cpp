// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/index_internal.h>
#include <dftracer/utils/index/store/internal/index_decoding.h>
#include <dftracer/utils/index/store/internal/index_encoding.h>
#include <dftracer/utils/index/store/internal/statistics_codec.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/index/tables.h>

#include <algorithm>
#include <exception>

namespace dftracer::utils::index {

using store::layout::Ext;

namespace store {

ColumnType merge_column_type(ColumnType a, ColumnType b) {
    if (a == b) return a;
    if (a == ColumnType::Unknown) return b;
    if (b == ColumnType::Unknown) return a;
    if ((a == ColumnType::Int64 && b == ColumnType::Float64) ||
        (a == ColumnType::Float64 && b == ColumnType::Int64))
        return ColumnType::Float64;
    return ColumnType::Json;
}

const char* column_type_name(ColumnType t) {
    switch (t) {
        case ColumnType::Int64:
            return "int64";
        case ColumnType::Float64:
            return "float64";
        case ColumnType::String:
            return "string";
        case ColumnType::Json:
            return "json";
        case ColumnType::Unknown:
            return "";
    }
    return "";
}

ColumnType seen_column_type(std::uint8_t seen) {
    auto has = [seen](PathType t) {
        return (seen >> static_cast<unsigned>(t)) & 1U;
    };
    const bool other = has(PathType::Uint) || has(PathType::Double) ||
                       has(PathType::Bool) || has(PathType::Int);
    if (has(PathType::Mixed) || (has(PathType::String) && other))
        return ColumnType::Json;
    if (has(PathType::String)) return ColumnType::String;
    if (has(PathType::Uint) || has(PathType::Double))
        return ColumnType::Float64;
    if (has(PathType::Bool) || has(PathType::Int)) return ColumnType::Int64;
    return ColumnType::Unknown;
}

}  // namespace store

namespace {

// A key is [kind][tail]. Its namespace names the extension and its scope
// names the file.
dstd::String kind_key(std::uint8_t kind) {
    return dstd::String(1, static_cast<char>(kind));
}

dstd::String file_key(std::uint8_t kind) { return kind_key(kind); }

dstd::String file_key(std::uint8_t kind, dstd::StringView tail) {
    dstd::String key = kind_key(kind);
    key.append(tail);
    return key;
}

dstd::String granule_key(std::uint8_t kind, std::uint64_t granule) {
    dstd::String key = kind_key(kind);
    dstd::append_u32_be(key, static_cast<std::uint32_t>(granule));
    return key;
}

dstd::String path_prefix(std::uint8_t kind, dstd::StringView path) {
    dstd::String key = kind_key(kind);
    key.append(path);
    key.push_back('\0');
    return key;
}

dstd::String path_granule_key(dstd::StringView path, std::uint64_t granule) {
    dstd::String key = path_prefix(store::layout::path_kind::DATA, path);
    dstd::append_u32_be(key, static_cast<std::uint32_t>(granule));
    return key;
}

dstd::String agg_partial_key(std::uint64_t chunk, std::uint32_t cat_id,
                             std::uint32_t name_id, std::uint32_t host_id,
                             std::uint32_t extra_id, std::uint8_t kind) {
    dstd::String key = granule_key(store::layout::agg_kind::PARTIAL, chunk);
    dstd::append_u32_be(key, cat_id);
    dstd::append_u32_be(key, name_id);
    dstd::append_u32_be(key, host_id);
    dstd::append_u32_be(key, extra_id);
    key.push_back(static_cast<char>(kind));
    return key;
}

dstd::StringView as_text(dstd::ByteView bytes) {
    return dstd::StringView(bytes.as<char>(), bytes.size());
}

dstd::String own(dstd::StringView bytes) {
    return dstd::String(bytes.data(), bytes.size());
}

std::uint32_t tail_u32(dstd::StringView key, std::size_t offset) {
    return store::layout::read_u32(key.substr(offset));
}

dstd::Unexpected<dstd::Error> parse_error(const std::exception& error) {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::Parse, error.what()));
}

dftracer::utils::store::Namespace read_ns(
    const dftracer::utils::store::NamespaceSpec& spec) {
    dftracer::utils::store::Namespace ns;
    ns.spec = spec;
    return ns;
}

dftracer::utils::store::Namespace read_ns(Ext ext) {
    return read_ns(ext_spec(ext));
}

bool ready(const Snapshot& snapshot, const FileRef& file, Ext ext) {
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return false;
    return snap->copy_info(read_ns(ext), IndexAccess::scope(file)).state ==
           dftracer::utils::store::ScopeState::Ready;
}

/// Calls fn(key, value) for each record under `prefix`. fn returns false to
/// stop. A decoder that throws becomes a Parse error.
template <class Fn>
dstd::Result<void> scan_payloads(const Snapshot& snapshot, const FileRef& file,
                                 const dftracer::utils::store::Namespace& ns,
                                 dstd::StringView prefix, Fn&& fn) {
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return {};
    dftracer::utils::store::Cursor cursor =
        snap->scan(ns, IndexAccess::scope(file), dstd::ByteView(prefix));
    for (; !cursor.at_end(); cursor.next()) {
        auto value = cursor.value();
        if (!value) return dstd::Unexpected<dstd::Error>(value.error());
        const dstd::StringView body = as_text(*value);
        bool more = true;
        try {
            more = fn(as_text(cursor.key()), body);
        } catch (const std::exception& error) {
            return parse_error(error);
        }
        if (!more) break;
    }
    if (cursor.error()) return dstd::Unexpected<dstd::Error>(*cursor.error());
    return {};
}

/// Calls fn(inner key) for each record under `prefix`, whatever its value. fn
/// returns false to stop.
template <class Fn>
dstd::Result<void> scan_keys(const Snapshot& snapshot, const FileRef& file,
                             const dftracer::utils::store::Namespace& ns,
                             dstd::StringView prefix, Fn&& fn) {
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return {};
    dftracer::utils::store::Cursor cursor =
        snap->scan(ns, IndexAccess::scope(file), dstd::ByteView(prefix));
    for (; !cursor.at_end(); cursor.next()) {
        if (!fn(as_text(cursor.key()))) break;
    }
    if (cursor.error()) return dstd::Unexpected<dstd::Error>(*cursor.error());
    return {};
}

/// fn(value) of the record at `key`. Empty when the key is absent.
template <class Fn>
auto get_decoded(const Snapshot& snapshot, const FileRef& file,
                 const dftracer::utils::store::Namespace& ns,
                 const dstd::String& key, Fn&& fn)
    -> dstd::Result<dstd::Optional<decltype(fn(dstd::StringView{}))>> {
    using Out = decltype(fn(dstd::StringView{}));
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return dstd::Optional<Out>{};
    auto found = snap->get(ns, IndexAccess::scope(file), dstd::ByteView(key));
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) return dstd::Optional<Out>{};
    try {
        return dstd::Optional<Out>(fn(as_text((*found)->bytes())));
    } catch (const std::exception& error) {
        return parse_error(error);
    }
}

dstd::Result<dstd::Optional<dstd::String>> get_string(const Snapshot& snapshot,
                                                      const FileRef& file,
                                                      Ext ext,
                                                      const dstd::String& key) {
    return get_decoded(snapshot, file, read_ns(ext), key,
                       [](dstd::StringView body) { return own(body); });
}

/// The highest format version of each plugin namespace the store declares.
dstd::Result<dstd::Map<dstd::String, std::uint32_t>> plugin_versions(
    const dftracer::utils::store::Snapshot& snap) {
    const dstd::StringView prefix(store::store_layout::PLUGIN_NAMESPACE_PREFIX);
    auto declared = snap.namespaces();
    if (!declared) return dstd::Unexpected<dstd::Error>(declared.error());
    dstd::Map<dstd::String, std::uint32_t> versions;
    for (const dftracer::utils::store::NamespaceDecl& entry : *declared) {
        const dstd::StringView name(entry.name.data(), entry.name.size());
        if (!name.starts_with(prefix)) continue;
        std::uint32_t& known =
            versions[dstd::String(name.substr(prefix.size()))];
        known = std::max(known, entry.format_version);
    }
    return versions;
}

/// The plugin namespace the file holds a copy of. Nullopt when it holds none.
dstd::Result<dstd::Optional<dftracer::utils::store::Namespace>>
plugin_namespace(const Snapshot& snapshot, const FileRef& file,
                 dstd::StringView name) {
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr)
        return dstd::Optional<dftracer::utils::store::Namespace>{};
    auto versions = plugin_versions(*snap);
    if (!versions) return dstd::Unexpected<dstd::Error>(versions.error());
    const auto known = versions->find(dstd::String(name));
    if (known == versions->end())
        return dstd::Optional<dftracer::utils::store::Namespace>{};
    const dftracer::utils::store::Namespace ns =
        read_ns(plugin_spec(name, known->second));
    const dftracer::utils::store::CopyInfo info =
        snap->copy_info(ns, IndexAccess::scope(file));
    if (info.state != dftracer::utils::store::ScopeState::Ready &&
        info.state != dftracer::utils::store::ScopeState::Failed)
        return dstd::Optional<dftracer::utils::store::Namespace>{};
    return dstd::Optional<dftracer::utils::store::Namespace>(ns);
}

void put_value(FileBuild& build, Ext ext, const dstd::String& key,
               dstd::StringView body) {
    IndexAccess::put(build, ext, dstd::ByteView(key), dstd::ByteView(body));
}

void put_bare(FileBuild& build, Ext ext, const dstd::String& key) {
    IndexAccess::put(build, ext, dstd::ByteView(key), dstd::ByteView());
}

bool builtin(FileBuild& build, store::IndexExtension ext) {
    if (ext != Ext::Plugin) return true;
    IndexAccess::record_error(build,
                              dstd::Error(dstd::Condition::InvalidArgument,
                                          "index: not a built-in extension"));
    return false;
}

constexpr std::uint32_t AGG_META_VERSION = 0;

bool builtin_ext(Ext ext) { return ext != Ext::Plugin; }

dftracer::utils::store::NamespaceSpec agg_meta_spec(dstd::StringView name) {
    dftracer::utils::store::NamespaceSpec spec;
    const dstd::String full = store::store_layout::agg_meta_namespace_name(
        dstd::StringView(name.data(), name.size()));
    spec.name.assign(full.data(), full.size());
    spec.format_version = AGG_META_VERSION;
    return spec;
}

dstd::Result<void> invalid_result(dstd::StringView message) {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument, message));
}

dstd::Expected<schemas::dft::agg::ChunkColumns, dstd::Error> decode_block(
    dstd::StringView body) {
    auto block =
        schemas::dft::agg::decode_chunk_columns(dstd::Span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    if (!block)
        return dstd::Unexpected<dstd::Error>(dstd::Error(
            dstd::Condition::Parse, "chunk columns: malformed record"));
    return std::move(*block);
}

/// An id the file holds no entry for gets the empty text.
void fill_texts(const dstd::HashMap<std::uint32_t, dstd::String>& texts,
                schemas::dft::agg::ChunkColumns& block) {
    block.texts.resize(block.ids.size());
    for (std::size_t i = 0; i < block.ids.size(); ++i) {
        const auto at = texts.find(block.ids[i]);
        if (at != texts.end()) block.texts[i] = at->second;
    }
}

}  // namespace

dftracer::utils::store::NamespaceSpec ext_spec(store::IndexExtension ext) {
    dftracer::utils::store::NamespaceSpec spec;
    const dstd::String name = store::store_layout::namespace_name(ext);
    spec.name.assign(name.data(), name.size());
    spec.format_version = store::layout::ext_version(ext);
    // A grown file adds a copy of only its new keys, so a build replaces older
    // copies by asking for it at publish.
    spec.shared = true;
    return spec;
}

dftracer::utils::store::NamespaceSpec plugin_spec(dstd::StringView name,
                                                  std::uint32_t version) {
    dftracer::utils::store::NamespaceSpec spec;
    const dstd::String full = store::store_layout::plugin_namespace_name(name);
    spec.name.assign(full.data(), full.size());
    spec.format_version = version;
    return spec;
}

dstd::Result<dstd::Vector<gzip::GzipMemberRecord>> members(
    const Snapshot& snapshot, const FileRef& file) {
    dstd::Vector<gzip::GzipMemberRecord> out;
    if (!ready(snapshot, file, Ext::Members)) return out;
    auto scanned = scan_payloads(
        snapshot, file, read_ns(Ext::Members),
        kind_key(store::layout::members::MEMBER),
        [&](dstd::StringView key, dstd::StringView body) {
            out.push_back(store::internal::decoding::decode_gzip_member(
                tail_u32(key, 1), body));
            return true;
        });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

dstd::Result<dstd::Vector<gzip::ChunkSpan>> spans(const Snapshot& snapshot,
                                                  const FileRef& file) {
    auto found = members(snapshot, file);
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    dstd::Vector<gzip::ChunkSpan> out;
    out.reserve(found->size());
    for (const auto& member : *found) {
        out.push_back({member.uc_offset, member.uc_size, member.first_line_num,
                       member.last_line_num, member.opens_mid_line});
    }
    return out;
}

dstd::Result<dstd::Optional<dstd::String>> restart_window(
    const Snapshot& snapshot, const FileRef& file, std::uint64_t member_idx) {
    return get_string(snapshot, file, Ext::Members,
                      granule_key(store::layout::members::RESTART, member_idx));
}

dstd::Result<dstd::Optional<store::FileMetadataResult>> file_metadata(
    const Snapshot& snapshot, const FileRef& file) {
    return get_decoded(
        snapshot, file, read_ns(Ext::Members),
        file_key(store::layout::members::METADATA), [](dstd::StringView body) {
            return store::internal::decoding::decode_metadata_record(body);
        });
}

dstd::Result<dstd::Vector<schemas::dft::ChunkStatisticsResult>> chunk_stats(
    const Snapshot& snapshot, const FileRef& file) {
    dstd::Vector<schemas::dft::ChunkStatisticsResult> out;
    auto scanned = scan_payloads(
        snapshot, file, read_ns(Ext::Stats),
        kind_key(store::layout::stats_kind::CHUNK_STATS),
        [&](dstd::StringView key, dstd::StringView body) {
            out.push_back(
                {tail_u32(key, 1),
                 store::internal::decoding::decode_chunk_statistics_value(
                     body)});
            return true;
        });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

dstd::Result<dstd::Optional<schemas::dft::MergedStatisticsResult>> scalar_stats(
    const Snapshot& snapshot, const FileRef& file) {
    return get_decoded(
        snapshot, file, read_ns(Ext::Stats),
        file_key(store::layout::stats_kind::FILE_SCALAR_STATS),
        [](dstd::StringView body) {
            return store::internal::decode_file_scalar_stats_value(body);
        });
}

dstd::Result<dstd::Optional<dstd::StringViewMap<std::uint64_t>>> pid_tid_counts(
    const Snapshot& snapshot, const FileRef& file) {
    return get_decoded(
        snapshot, file, read_ns(Ext::Stats),
        file_key(store::layout::stats_kind::FILE_PID_TID_COUNTS),
        [](dstd::StringView body) {
            return store::internal::decoding::decode_count_map_value(body);
        });
}

dstd::Result<dstd::Optional<dstd::HashMap<std::uint64_t, store::ChunkMetadata>>>
chunk_meta(const Snapshot& snapshot, const FileRef& file) {
    using Map = dstd::HashMap<std::uint64_t, store::ChunkMetadata>;
    if (!ready(snapshot, file, Ext::Metadata)) return dstd::Optional<Map>{};
    Map out;
    auto scanned = scan_payloads(
        snapshot, file, read_ns(Ext::Metadata),
        kind_key(store::layout::metadata_kind::CHUNK),
        [&](dstd::StringView key, dstd::StringView body) {
            if (auto metadata = store::layout::decode_chunk_metadata(body))
                out.emplace(tail_u32(key, 1), std::move(*metadata));
            return true;
        });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return dstd::Optional<Map>(std::move(out));
}

dstd::Result<dstd::Vector<dstd::Pair<dstd::String, store::PathStat>>> catalog(
    const Snapshot& snapshot, const FileRef& file) {
    dstd::Vector<dstd::Pair<dstd::String, store::PathStat>> out;
    auto scanned =
        scan_payloads(snapshot, file, read_ns(Ext::Catalog),
                      kind_key(store::layout::catalog_kind::PATH),
                      [&](dstd::StringView key, dstd::StringView body) {
                          if (auto stat = store::layout::decode_path_stat(body))
                              out.emplace_back(own(key.substr(1)), *stat);
                          return true;
                      });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

dstd::Result<dstd::Optional<dstd::String>> schema(const Snapshot& snapshot,
                                                  const FileRef& file) {
    if (!ready(snapshot, file, Ext::Profile))
        return dstd::Optional<dstd::String>{};
    return get_string(snapshot, file, Ext::Profile,
                      file_key(store::layout::profile_kind::ID));
}

dstd::Result<dstd::Optional<dstd::String>> rowset(const Snapshot& snapshot,
                                                  const FileRef& file,
                                                  dstd::StringView name) {
    if (!ready(snapshot, file, Ext::Rowset))
        return dstd::Optional<dstd::String>{};
    return get_string(snapshot, file, Ext::Rowset,
                      path_prefix(store::layout::rowset_kind::FRAME, name));
}

dstd::Result<dstd::Vector<dstd::String>> paths(const Snapshot& snapshot,
                                               const FileRef& file,
                                               store::IndexExtension ext) {
    dstd::Vector<dstd::String> out;
    if (ext == Ext::Plugin) return out;
    auto scanned = scan_keys(snapshot, file, read_ns(ext),
                             kind_key(store::layout::path_kind::PATHS),
                             [&](dstd::StringView key) {
                                 out.push_back(own(key.substr(1)));
                                 return true;
                             });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

dstd::Result<GranuleList> granules(const Snapshot& snapshot,
                                   const FileRef& file,
                                   store::IndexExtension ext,
                                   dstd::StringView path) {
    GranuleList out;
    if (ext == Ext::Plugin) return out;
    const dstd::String prefix =
        path_prefix(store::layout::path_kind::DATA, path);
    auto scanned = scan_payloads(
        snapshot, file, read_ns(ext), prefix,
        [&](dstd::StringView key, dstd::StringView body) {
            if (key.size() == prefix.size() + 4)
                out.emplace_back(tail_u32(key, prefix.size()), own(body));
            return true;
        });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

dstd::Result<dstd::Optional<dstd::String>> path_file(const Snapshot& snapshot,
                                                     const FileRef& file,
                                                     store::IndexExtension ext,
                                                     dstd::StringView path) {
    if (ext == Ext::Plugin) return dstd::Optional<dstd::String>{};
    return get_string(snapshot, file, ext,
                      file_key(store::layout::path_kind::FILE, path));
}

dstd::Result<bool> has_postings(const Snapshot& snapshot, const FileRef& file,
                                dstd::StringView path) {
    bool found = false;
    auto scanned =
        scan_keys(snapshot, file, read_ns(Ext::Postings),
                  path_prefix(store::layout::path_kind::IN_FILE, path),
                  [&](dstd::StringView) {
                      found = true;
                      return false;
                  });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return found;
}

dstd::Result<bool> has_posting(const Snapshot& snapshot, const FileRef& file,
                               dstd::StringView path,
                               std::uint64_t value_hash) {
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return false;
    dstd::String key = path_prefix(store::layout::path_kind::IN_FILE, path);
    dstd::append_u64_be(key, value_hash);
    auto found = snap->get(read_ns(Ext::Postings), IndexAccess::scope(file),
                           dstd::ByteView(key));
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    return found->has_value();
}

dstd::Result<dstd::Vector<std::uint64_t>> posting_granules(
    const Snapshot& snapshot, const FileRef& file, dstd::StringView path,
    std::uint64_t value_hash) {
    dstd::Vector<std::uint64_t> out;
    dstd::String prefix =
        path_prefix(store::layout::path_kind::IN_GRANULE, path);
    dstd::append_u64_be(prefix, value_hash);
    auto scanned =
        scan_keys(snapshot, file, read_ns(Ext::Postings), prefix,
                  [&](dstd::StringView key) {
                      if (key.size() == prefix.size() + 4)
                          out.push_back(tail_u32(key, prefix.size()));
                      return true;
                  });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

dstd::Result<dstd::Vector<PluginCopy>> plugins(const Snapshot& snapshot,
                                               const FileRef& file) {
    dstd::Vector<PluginCopy> out;
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return out;
    auto versions = plugin_versions(*snap);
    if (!versions) return dstd::Unexpected<dstd::Error>(versions.error());
    for (const auto& [name, version] : *versions) {
        const dftracer::utils::store::CopyInfo info = snap->copy_info(
            read_ns(plugin_spec(name, version)), IndexAccess::scope(file));
        if (info.state != dftracer::utils::store::ScopeState::Ready &&
            info.state != dftracer::utils::store::ScopeState::Failed)
            continue;
        out.push_back(
            {own(name), version, info.params_hash,
             info.state == dftracer::utils::store::ScopeState::Ready});
    }
    return out;
}

dstd::Result<dstd::Optional<PluginData>> plugin(const Snapshot& snapshot,
                                                const FileRef& file,
                                                dstd::StringView name) {
    auto granules = plugin_granules(snapshot, file, name);
    if (!granules) return dstd::Unexpected<dstd::Error>(granules.error());
    if (!*granules) return dstd::Optional<PluginData>{};
    auto whole = plugin_file(snapshot, file, name);
    if (!whole) return dstd::Unexpected<dstd::Error>(whole.error());
    return dstd::Optional<PluginData>(
        PluginData{std::move(**granules), std::move(*whole)});
}

dstd::Result<dstd::Optional<GranuleList>> plugin_granules(
    const Snapshot& snapshot, const FileRef& file, dstd::StringView name) {
    auto ns = plugin_namespace(snapshot, file, name);
    if (!ns) return dstd::Unexpected<dstd::Error>(ns.error());
    if (!*ns) return dstd::Optional<GranuleList>{};
    GranuleList out;
    const dstd::String prefix =
        path_prefix(store::layout::path_kind::DATA, name);
    auto scanned = scan_payloads(
        snapshot, file, **ns, prefix,
        [&](dstd::StringView key, dstd::StringView body) {
            if (key.size() == prefix.size() + 4)
                out.emplace_back(tail_u32(key, prefix.size()), own(body));
            return true;
        });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return dstd::Optional<GranuleList>(std::move(out));
}

dstd::Result<dstd::Optional<dstd::String>> plugin_file(const Snapshot& snapshot,
                                                       const FileRef& file,
                                                       dstd::StringView name) {
    auto ns = plugin_namespace(snapshot, file, name);
    if (!ns) return dstd::Unexpected<dstd::Error>(ns.error());
    if (!*ns) return dstd::Optional<dstd::String>{};
    return get_decoded(snapshot, file, **ns,
                       file_key(store::layout::path_kind::FILE, name),
                       [](dstd::StringView body) { return own(body); });
}

dstd::Result<void> for_each_agg_partial(const Snapshot& snapshot,
                                        const FileRef& file,
                                        const AggPartialFn& fn) {
    constexpr std::size_t RECORD_BYTES =
        5 * sizeof(std::uint32_t) + sizeof(std::uint8_t);
    dstd::Optional<dstd::Error> failed;
    const dstd::String prefix = kind_key(store::layout::agg_kind::PARTIAL);
    auto scanned = scan_payloads(
        snapshot, file, read_ns(Ext::Agg), prefix,
        [&](dstd::StringView key, dstd::StringView body) {
            const dstd::StringView tail = key.substr(prefix.size());
            if (tail.size() != RECORD_BYTES) {
                failed = dstd::Error(dstd::Condition::Parse,
                                     "aggregation partial: malformed record");
                return false;
            }
            const auto kind = static_cast<std::uint8_t>(tail[RECORD_BYTES - 1]);
            if (kind > static_cast<std::uint8_t>(
                           schemas::dft::agg::PartialKind::Aggregated)) {
                failed = dstd::Error(dstd::Condition::Parse,
                                     "aggregation partial: unknown kind");
                return false;
            }
            const dstd::Span<const std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(body.data()),
                body.size());
            return fn(tail_u32(tail, 0), tail_u32(tail, 4), tail_u32(tail, 8),
                      tail_u32(tail, 12), tail_u32(tail, 16),
                      static_cast<schemas::dft::agg::PartialKind>(kind), bytes);
        });
    if (failed) return dstd::Unexpected<dstd::Error>(*failed);
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return {};
}

dstd::Result<void> for_each_agg_chunk_columns(const Snapshot& snapshot,
                                              const FileRef& file,
                                              const AggChunkColumnsFn& fn) {
    constexpr std::size_t RECORD_BYTES = sizeof(std::uint32_t);
    dstd::Optional<dstd::Error> failed;
    const dstd::String prefix = kind_key(store::layout::agg_kind::COLUMNS);
    dstd::HashMap<std::uint32_t, dstd::String> texts;
    bool texts_read = false;
    auto scanned = scan_payloads(
        snapshot, file, read_ns(Ext::Agg), prefix,
        [&](dstd::StringView key, dstd::StringView body) {
            if (key.size() != prefix.size() + RECORD_BYTES) {
                failed = dstd::Error(dstd::Condition::Parse,
                                     "chunk columns: bad key");
                return false;
            }
            if (!texts_read) {
                auto loaded = agg_file_texts(snapshot, file, texts);
                if (!loaded) {
                    failed = loaded.error();
                    return false;
                }
                texts_read = true;
            }
            auto block = decode_block(body);
            if (!block) {
                failed = block.error();
                return false;
            }
            block->chunk = tail_u32(key, prefix.size());
            fill_texts(texts, *block);
            return fn(block->chunk, *block);
        });
    if (failed) return dstd::Unexpected<dstd::Error>(*failed);
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return {};
}

dstd::Result<dstd::Optional<schemas::dft::agg::ChunkColumns>> agg_chunk_columns(
    const Snapshot& snapshot, const FileRef& file, std::uint64_t chunk) {
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr)
        return dstd::Optional<schemas::dft::agg::ChunkColumns>{};
    const dstd::String key =
        granule_key(store::layout::agg_kind::COLUMNS, chunk);
    auto found = snap->get(read_ns(Ext::Agg), IndexAccess::scope(file),
                           dstd::ByteView(key));
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) return dstd::Optional<schemas::dft::agg::ChunkColumns>{};
    auto block = decode_block(as_text((*found)->bytes()));
    if (!block) return dstd::Unexpected<dstd::Error>(block.error());
    block->chunk = chunk;
    dstd::HashMap<std::uint32_t, dstd::String> texts;
    auto loaded = agg_file_texts(snapshot, file, texts);
    if (!loaded) return dstd::Unexpected<dstd::Error>(loaded.error());
    fill_texts(texts, *block);
    return dstd::Optional<schemas::dft::agg::ChunkColumns>(std::move(*block));
}

dstd::Result<void> agg_file_texts(
    const Snapshot& snapshot, const FileRef& file,
    dstd::HashMap<std::uint32_t, dstd::String>& texts) {
    dstd::Optional<dstd::Error> failed;
    const dstd::String prefix = kind_key(store::layout::agg_kind::DICT);
    auto scanned = scan_payloads(
        snapshot, file, read_ns(Ext::Agg), prefix,
        [&](dstd::StringView key, dstd::StringView text) {
            if (key.size() != prefix.size() + sizeof(std::uint32_t)) {
                failed = dstd::Error(dstd::Condition::Parse,
                                     "aggregation dictionary: malformed entry");
                return false;
            }
            texts.try_emplace(tail_u32(key, prefix.size()), own(text));
            return true;
        });
    if (failed) return dstd::Unexpected<dstd::Error>(*failed);
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return {};
}

dstd::Result<void> agg_dictionary(const Snapshot& snapshot,
                                  schemas::dft::agg::AggInternTable& table) {
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return {};
    const dstd::String begin = kind_key(store::layout::agg_kind::DICT);
    const dstd::String end = store::layout::prefix_end(begin);
    dftracer::utils::store::Cursor cursor = snap->scan_all(
        read_ns(Ext::Agg), dstd::ByteView(begin), dstd::ByteView(end));
    dstd::Optional<dstd::Error> failed;
    for (; !cursor.at_end(); cursor.next()) {
        auto value = cursor.value();
        if (!value) return dstd::Unexpected<dstd::Error>(value.error());
        const dstd::StringView key = as_text(cursor.key());
        if (key.size() != begin.size() + sizeof(std::uint32_t)) {
            failed = dstd::Error(dstd::Condition::Parse,
                                 "aggregation dictionary: malformed entry");
            break;
        }
        const auto text = as_text(*value);
        const auto status =
            table.intern.insert_at_id(tail_u32(key, begin.size()), text);
        if (status == dstd::StringIntern::InsertStatus::Ok) continue;
        failed =
            dstd::Error(dstd::Condition::InvalidArgument,
                        status == dstd::StringIntern::InsertStatus::Conflict
                            ? "string intern: the id holds another string"
                            : "string intern: the id is out of range");
        break;
    }
    if (failed) return dstd::Unexpected<dstd::Error>(*failed);
    if (cursor.error()) return dstd::Unexpected<dstd::Error>(*cursor.error());
    return {};
}

dstd::Result<dstd::Optional<dstd::String>> agg_meta(const Snapshot& snapshot,
                                                    dstd::StringView name) {
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return dstd::Optional<dstd::String>{};
    const dstd::String key =
        file_key(store::layout::agg_kind::META,
                 dstd::StringView(name.data(), name.size()));
    auto found = snap->get(read_ns(agg_meta_spec(name)),
                           dftracer::utils::store::ScopeKey::root(),
                           dstd::ByteView(key));
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) return dstd::Optional<dstd::String>{};
    return dstd::Optional<dstd::String>(own(as_text((*found)->bytes())));
}

dstd::Optional<std::uint64_t> file_hash(const Snapshot& snapshot,
                                        const FileRef& file) {
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return dstd::Optional<std::uint64_t>{};
    const dftracer::utils::store::CopyInfo info =
        snap->copy_info(read_ns(Ext::Members), IndexAccess::scope(file));
    if (info.sources.empty()) return dstd::Optional<std::uint64_t>{};
    return dstd::Optional<std::uint64_t>(info.sources[0].hash);
}

dstd::Optional<store::ExtensionState> extension_state(
    const Snapshot& snapshot, const FileRef& file, store::IndexExtension ext) {
    if (!builtin_ext(ext)) return dstd::Optional<store::ExtensionState>{};
    const dftracer::utils::store::Snapshot* snap =
        IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return dstd::Optional<store::ExtensionState>{};
    const dftracer::utils::store::CopyInfo info =
        snap->copy_info(read_ns(ext), IndexAccess::scope(file));
    if (info.state != dftracer::utils::store::ScopeState::Ready &&
        info.state != dftracer::utils::store::ScopeState::Failed)
        return dstd::Optional<store::ExtensionState>{};
    return dstd::Optional<store::ExtensionState>(store::ExtensionState{
        store::layout::ext_version(ext), info.params_hash,
        info.state == dftracer::utils::store::ScopeState::Ready});
}

bool extension_current(const Snapshot& snapshot, const FileRef& file,
                       store::IndexExtension ext) {
    if (!builtin_ext(ext)) return false;
    return ready(snapshot, file, ext);
}

dstd::Result<dstd::Vector<dstd::Pair<dstd::String, store::ColumnType>>>
file_column_types(const Snapshot& snapshot, const FileRef& file) {
    dstd::Vector<dstd::Pair<dstd::String, store::ColumnType>> out;
    auto listed = catalog(snapshot, file);
    if (!listed) return dstd::Unexpected<dstd::Error>(listed.error());
    for (auto& [path, stat] : *listed) {
        const store::ColumnType type = store::seen_column_type(stat.seen);
        if (type == store::ColumnType::Unknown) continue;
        out.emplace_back(std::move(path), type);
    }
    return out;
}

void put_file_metadata(FileBuild& build, std::uint64_t checkpoint_size,
                       std::uint64_t total_lines, std::uint64_t total_uc_size,
                       bool truncated, bool has_open_line,
                       std::uint64_t next_member_idx, bool has_mid_line_chunk) {
    put_value(
        build, Ext::Members, file_key(store::layout::members::METADATA),
        store::internal::encoding::encode_metadata_record(
            checkpoint_size, total_lines, total_uc_size,
            (truncated ? store::internal::encoding::METADATA_FLAG_TRUNCATED
                       : 0) |
                (has_open_line
                     ? store::internal::encoding::METADATA_FLAG_OPEN_LINE
                     : 0) |
                (has_mid_line_chunk
                     ? store::internal::encoding::METADATA_FLAG_MID_LINE_CHUNK
                     : 0),
            next_member_idx));
}

void put_gzip_member(FileBuild& build, const gzip::GzipMemberRecord& member) {
    put_value(build, Ext::Members,
              granule_key(store::layout::members::MEMBER, member.member_idx),
              store::internal::encoding::encode_gzip_member_value(member));
}

void put_restart_window(FileBuild& build, std::uint64_t member_idx,
                        dstd::StringView window) {
    put_value(build, Ext::Members,
              granule_key(store::layout::members::RESTART, member_idx), window);
}

void put_chunk_statistics(FileBuild& build, std::uint64_t checkpoint_idx,
                          const schemas::dft::ChunkStatistics& stats) {
    put_value(
        build, Ext::Stats,
        granule_key(store::layout::stats_kind::CHUNK_STATS, checkpoint_idx),
        store::internal::encoding::encode_chunk_statistics_value(stats));
}

void put_chunk_metadata(FileBuild& build, std::uint64_t checkpoint_idx,
                        const store::ChunkMetadata& metadata) {
    put_value(build, Ext::Metadata,
              granule_key(store::layout::metadata_kind::CHUNK, checkpoint_idx),
              store::layout::encode_chunk_metadata(metadata));
}

void put_file_scalar_stats(FileBuild& build,
                           const schemas::dft::ChunkStatistics& stats,
                           std::uint64_t chunk_count) {
    put_value(
        build, Ext::Stats,
        file_key(store::layout::stats_kind::FILE_SCALAR_STATS),
        store::internal::encode_file_scalar_stats_value(stats, chunk_count));
}

void put_file_pid_tid_counts(FileBuild& build,
                             const dstd::StringViewMap<std::uint64_t>& counts) {
    put_value(build, Ext::Stats,
              file_key(store::layout::stats_kind::FILE_PID_TID_COUNTS),
              store::internal::encoding::encode_count_map_value(counts));
}

void put_profile(FileBuild& build, dstd::StringView profile_id) {
    put_value(build, Ext::Profile, file_key(store::layout::profile_kind::ID),
              profile_id);
}

void put_catalog_path(FileBuild& build, dstd::StringView path,
                      const store::PathStat& stat) {
    put_value(build, Ext::Catalog,
              file_key(store::layout::catalog_kind::PATH, path),
              store::layout::encode_path_stat(stat));
}

void put_rowset(FileBuild& build, dstd::StringView name,
                dstd::StringView frame) {
    put_value(build, Ext::Rowset,
              path_prefix(store::layout::rowset_kind::FRAME, name), frame);
}

void put_path(FileBuild& build, store::IndexExtension ext,
              dstd::StringView path) {
    if (!builtin(build, ext)) return;
    put_bare(build, ext, file_key(store::layout::path_kind::PATHS, path));
}

void put_path_granule(FileBuild& build, store::IndexExtension ext,
                      dstd::StringView path, std::uint64_t granule,
                      dstd::StringView payload) {
    if (!builtin(build, ext)) return;
    put_value(build, ext, path_granule_key(path, granule), payload);
}

void erase_path(FileBuild& build, store::IndexExtension ext,
                dstd::StringView path) {
    if (!builtin(build, ext)) return;
    const dftracer::utils::store::NamespaceSpec& spec = ext_spec(ext);
    // A key is NUL free, so one NUL past it bounds exactly that key.
    const auto erase_key = [&](dstd::String key) {
        dstd::String end = key;
        end.push_back('\0');
        IndexAccess::erase(build, spec, dstd::ByteView(key),
                           dstd::ByteView(end));
    };
    erase_key(file_key(store::layout::path_kind::PATHS, path));
    erase_key(file_key(store::layout::path_kind::FILE, path));
    const dstd::String data = path_prefix(store::layout::path_kind::DATA, path);
    const dstd::String end = store::layout::prefix_end(data);
    IndexAccess::erase(build, spec, dstd::ByteView(data), dstd::ByteView(end));
}

void put_path_file(FileBuild& build, store::IndexExtension ext,
                   dstd::StringView path, dstd::StringView payload) {
    if (!builtin(build, ext)) return;
    put_value(build, ext, file_key(store::layout::path_kind::FILE, path),
              payload);
}

void put_posting(FileBuild& build, dstd::StringView path,
                 std::uint64_t value_hash) {
    dstd::String key = path_prefix(store::layout::path_kind::IN_FILE, path);
    dstd::append_u64_be(key, value_hash);
    put_bare(build, Ext::Postings, key);
}

void put_posting_granule(FileBuild& build, dstd::StringView path,
                         std::uint64_t value_hash, std::uint64_t granule) {
    dstd::String key = path_prefix(store::layout::path_kind::IN_GRANULE, path);
    dstd::append_u64_be(key, value_hash);
    dstd::append_u32_be(key, static_cast<std::uint32_t>(granule));
    put_bare(build, Ext::Postings, key);
}

void put_agg_partial(FileBuild& build, std::uint64_t chunk,
                     std::uint32_t cat_id, std::uint32_t name_id,
                     std::uint32_t host_id, std::uint32_t extra_id,
                     std::uint8_t kind, dstd::StringView payload) {
    put_value(build, Ext::Agg,
              agg_partial_key(chunk, cat_id, name_id, host_id, extra_id, kind),
              payload);
}

void put_agg_partial_value(FileBuild& build, std::uint64_t chunk,
                           std::uint32_t cat_id, std::uint32_t name_id,
                           std::uint32_t host_id, std::uint32_t extra_id,
                           std::uint8_t kind, dstd::StringView value) {
    const dstd::String key =
        agg_partial_key(chunk, cat_id, name_id, host_id, extra_id, kind);
    IndexAccess::put(build, Ext::Agg, dstd::ByteView(key),
                     dstd::ByteView(value.data(), value.size()));
}

void put_agg_chunk_columns_value(FileBuild& build, std::uint64_t chunk,
                                 dstd::StringView value) {
    const dstd::String key =
        granule_key(store::layout::agg_kind::COLUMNS, chunk);
    IndexAccess::put(build, Ext::Agg, dstd::ByteView(key),
                     dstd::ByteView(value.data(), value.size()));
}

void put_agg_chunk_columns(FileBuild& build, std::uint64_t chunk,
                           dstd::StringView payload) {
    put_value(build, Ext::Agg,
              granule_key(store::layout::agg_kind::COLUMNS, chunk), payload);
}

void put_agg_dictionary_entry(FileBuild& build, std::uint32_t id,
                              dstd::StringView text) {
    dstd::String key = file_key(store::layout::agg_kind::DICT);
    dstd::append_u32_be(key, id);
    put_value(build, Ext::Agg, key, text);
}

namespace {

template <class Granules, class File>
void write_plugin(FileBuild& build, dstd::StringView name,
                  std::uint32_t version, std::uint64_t params_hash,
                  const Granules& granules, const File& file) {
    const dftracer::utils::store::NamespaceSpec spec =
        plugin_spec(name, version);
    const dstd::String listed = file_key(store::layout::path_kind::PATHS, name);
    IndexAccess::put(build, spec, dstd::ByteView(listed), dstd::ByteView());
    for (const auto& [granule, payload] : granules) {
        const dstd::String key = path_granule_key(name, granule);
        IndexAccess::put(build, spec, dstd::ByteView(key),
                         dstd::ByteView(payload.data(), payload.size()));
    }
    if (file) {
        const dstd::String key = file_key(store::layout::path_kind::FILE, name);
        IndexAccess::put(build, spec, dstd::ByteView(key),
                         dstd::ByteView(file->data(), file->size()));
    }
    IndexAccess::finish(build, spec, params_hash);
}

}  // namespace

void put_plugin(FileBuild& build, dstd::StringView name, std::uint32_t version,
                std::uint64_t params_hash, const GranuleList& granules,
                const dstd::Optional<dstd::String>& file) {
    write_plugin(build, name, version, params_hash, granules, file);
}

void put_plugin_views(FileBuild& build, dstd::StringView name,
                      std::uint32_t version, std::uint64_t params_hash,
                      const GranuleViews& granules,
                      const dstd::Optional<dstd::StringView>& file) {
    write_plugin(build, name, version, params_hash, granules, file);
}

void fail_plugin(FileBuild& build, dstd::StringView name, std::uint32_t version,
                 std::uint64_t params_hash) {
    IndexAccess::fail(build, plugin_spec(name, version), params_hash);
}

void clear_unwritten(FileBuild& build,
                     dstd::Span<const store::IndexExtension> keep) {
    dstd::Vector<dftracer::utils::store::NamespaceSpec> specs;
    for (const store::IndexExtension ext : keep)
        if (builtin_ext(ext)) specs.push_back(ext_spec(ext));
    IndexAccess::drop_unwritten(
        build, dstd::Span<const dftracer::utils::store::NamespaceSpec>(
                   specs.data(), specs.size()));
}

void clear_chunk_copies(FileBuild& build) {
    constexpr store::IndexExtension PER_FILE[] = {
        store::IndexExtension::Catalog, store::IndexExtension::Profile,
        store::IndexExtension::Rowset};
    clear_unwritten(build, PER_FILE);
}

dstd::Result<FileBuild> rebuild(const Index& index, dstd::StringView path) {
    auto snapshot = index.snapshot();
    if (!snapshot) return dstd::Unexpected<dstd::Error>(snapshot.error());
    const auto file = snapshot->file(path);
    if (!file)
        return dstd::Unexpected<dstd::Error>(dstd::Error(
            dstd::Condition::NotFound, "index: the file is not indexed"));
    return index.build(path, file->stat());
}

void fail_extension(FileBuild& build, store::IndexExtension ext,
                    std::uint64_t params_hash) {
    if (!builtin(build, ext)) return;
    IndexAccess::fail(build, ext_spec(ext), params_hash);
}

coro::CoroTask<dstd::Result<void>> put_agg_meta(const Index& index,
                                                dstd::StringView name,
                                                dstd::StringView payload) {
    auto opened = IndexAccess::open_store(index);
    if (!opened) co_return dstd::Unexpected<dstd::Error>(opened.error());
    dftracer::utils::store::Store store = std::move(*opened);
    auto registered = store.ns(agg_meta_spec(name));
    if (!registered)
        co_return dstd::Unexpected<dstd::Error>(registered.error());
    const dftracer::utils::store::Namespace ns = *registered;
    const dftracer::utils::store::ScopeKey scope =
        dftracer::utils::store::ScopeKey::root();
    const dstd::String key =
        file_key(store::layout::agg_kind::META,
                 dstd::StringView(name.data(), name.size()));
    dftracer::utils::store::Batch batch = store.building_batch(ns, scope);
    batch.put(dstd::ByteView(key),
              dstd::ByteView(payload.data(), payload.size()));
    auto written = co_await store.commit(std::move(batch));
    if (!written) co_return dstd::Unexpected<dstd::Error>(written.error());
    const dftracer::utils::store::Publish target{&ns, &scope, 0, nullptr};
    auto published = co_await store.publish(
        dstd::Span<const dftracer::utils::store::Publish>(&target, 1));
    if (!published) co_return dstd::Unexpected<dstd::Error>(published.error());
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> delete_agg_meta(const Index& index,
                                                   dstd::StringView name) {
    auto found = IndexAccess::find_store(index);
    if (!found) co_return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) co_return dstd::Result<void>{};
    dftracer::utils::store::Store store = **found;
    auto registered = store.ns(agg_meta_spec(name));
    if (!registered)
        co_return dstd::Unexpected<dstd::Error>(registered.error());
    const dftracer::utils::store::Namespace ns = *registered;
    const dftracer::utils::store::ScopeKey scope =
        dftracer::utils::store::ScopeKey::root();
    auto taken = store.snapshot();
    if (!taken) co_return dstd::Unexpected<dstd::Error>(taken.error());
    if (taken->copy_info(ns, scope).state ==
        dftracer::utils::store::ScopeState::Absent)
        co_return dstd::Result<void>{};
    const dftracer::utils::store::Publish target{
        &ns, &scope, 0, nullptr,
        dftracer::utils::store::Publish::Change::Dropped};
    auto published = co_await store.publish(
        dstd::Span<const dftracer::utils::store::Publish>(&target, 1));
    if (!published) co_return dstd::Unexpected<dstd::Error>(published.error());
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> remove_extension(
    const Index& index, store::IndexExtension ext,
    dstd::Span<const dstd::StringView> paths) {
    if (!builtin_ext(ext))
        co_return invalid_result("index: not a built-in extension");
    auto found = IndexAccess::find_store(index);
    if (!found) co_return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) co_return dstd::Result<void>{};
    dftracer::utils::store::Store store = **found;
    auto registered = store.ns(ext_spec(ext));
    if (!registered)
        co_return dstd::Unexpected<dstd::Error>(registered.error());
    const dftracer::utils::store::Namespace ns = *registered;
    auto taken = store.snapshot();
    if (!taken) co_return dstd::Unexpected<dstd::Error>(taken.error());
    dstd::Vector<dftracer::utils::store::ScopeKey> scopes;
    scopes.reserve(paths.size());
    for (const dstd::StringView path : paths) {
        dftracer::utils::store::ScopeKey scope =
            dftracer::utils::store::ScopeKey::file(path);
        if (taken->copy_info(ns, scope).state ==
            dftracer::utils::store::ScopeState::Absent)
            continue;
        scopes.push_back(std::move(scope));
    }
    if (scopes.empty()) co_return dstd::Result<void>{};
    dstd::Vector<dftracer::utils::store::Publish> targets;
    targets.reserve(scopes.size());
    for (const dftracer::utils::store::ScopeKey& scope : scopes) {
        targets.push_back(dftracer::utils::store::Publish{
            &ns, &scope, 0, nullptr,
            dftracer::utils::store::Publish::Change::Dropped});
    }
    auto published = co_await store.publish(
        dstd::Span<const dftracer::utils::store::Publish>(targets.data(),
                                                          targets.size()));
    if (!published) co_return dstd::Unexpected<dstd::Error>(published.error());
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> remove_plugin(
    const Index& index, dstd::StringView name,
    dstd::Span<const dstd::StringView> paths) {
    auto found = IndexAccess::find_store(index);
    if (!found) co_return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) co_return dstd::Result<void>{};
    dftracer::utils::store::Store store = **found;
    auto taken = store.snapshot();
    if (!taken) co_return dstd::Unexpected<dstd::Error>(taken.error());
    auto versions = plugin_versions(*taken);
    if (!versions) co_return dstd::Unexpected<dstd::Error>(versions.error());
    const auto known = versions->find(dstd::String(name.data(), name.size()));
    if (known == versions->end()) co_return dstd::Result<void>{};
    auto registered = store.ns(plugin_spec(name, known->second));
    if (!registered)
        co_return dstd::Unexpected<dstd::Error>(registered.error());
    const dftracer::utils::store::Namespace ns = *registered;
    dstd::Vector<dftracer::utils::store::ScopeKey> scopes;
    scopes.reserve(paths.size());
    for (const dstd::StringView path : paths) {
        dftracer::utils::store::ScopeKey scope =
            dftracer::utils::store::ScopeKey::file(path);
        if (taken->copy_info(ns, scope).state ==
            dftracer::utils::store::ScopeState::Absent)
            continue;
        scopes.push_back(std::move(scope));
    }
    if (scopes.empty()) co_return dstd::Result<void>{};
    dstd::Vector<dftracer::utils::store::Publish> targets;
    targets.reserve(scopes.size());
    for (const dftracer::utils::store::ScopeKey& scope : scopes) {
        targets.push_back(dftracer::utils::store::Publish{
            &ns, &scope, 0, nullptr,
            dftracer::utils::store::Publish::Change::Dropped});
    }
    auto published = co_await store.publish(
        dstd::Span<const dftracer::utils::store::Publish>(targets.data(),
                                                          targets.size()));
    if (!published) co_return dstd::Unexpected<dstd::Error>(published.error());
    co_return dstd::Result<void>{};
}

}  // namespace dftracer::utils::index
