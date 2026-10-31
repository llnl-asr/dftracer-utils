#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/index/index_internal.h>
#include <dftracer/utils/index/store/internal/index_decoding.h>
#include <dftracer/utils/index/store/internal/index_encoding.h>
#include <dftracer/utils/index/store/internal/statistics_codec.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/index/tables.h>

#include <algorithm>
#include <exception>
#include <map>
#include <string>
#include <string_view>

namespace dftracer::utils::index {

namespace decoding = store::internal::decoding;
namespace encoding = store::internal::encoding;
namespace layout = store::layout;
namespace store_layout = store::store_layout;

using layout::Ext;

namespace {

namespace sk = layout::stats_kind;
namespace pk = layout::path_kind;

// The keys below are built with the legacy encoders and a file id of zero,
// then cut down to the key the store holds: [kind][tail].
constexpr std::uint32_t NO_FILE = 0;

std::string inner_of(const std::string& legacy) {
    std::string inner(1, legacy[2]);
    inner.append(legacy, layout::KEY_PREFIX_BYTES, std::string::npos);
    return inner;
}

std::string kind_key(std::uint8_t kind) {
    return std::string(1, static_cast<char>(kind));
}

std::string file_key(Ext ext, std::uint8_t kind) {
    return inner_of(layout::file_prefix(ext, kind, NO_FILE));
}

std::string file_key(Ext ext, std::uint8_t kind, std::string_view tail) {
    return inner_of(layout::file_prefix(ext, kind, NO_FILE).append(tail));
}

std::string granule_key(Ext ext, std::uint8_t kind, std::uint64_t granule) {
    return inner_of(layout::granule_key(ext, kind, NO_FILE,
                                        static_cast<std::uint32_t>(granule)));
}

std::string path_prefix(Ext ext, std::uint8_t kind, std::string_view path) {
    return inner_of(layout::path_prefix(ext, kind, NO_FILE, path));
}

std::string path_granule_key(Ext ext, std::string_view path,
                             std::uint64_t granule) {
    std::string key = path_prefix(ext, pk::DATA, path);
    dstd::append_u32_be(key, static_cast<std::uint32_t>(granule));
    return key;
}

std::string agg_partial_key(std::uint64_t chunk, std::uint32_t cat_id,
                            std::uint32_t name_id, std::uint32_t host_id,
                            std::uint32_t extra_id, std::uint8_t kind) {
    std::string key = granule_key(Ext::Agg, layout::agg_kind::PARTIAL, chunk);
    dstd::append_u32_be(key, cat_id);
    dstd::append_u32_be(key, name_id);
    dstd::append_u32_be(key, host_id);
    dstd::append_u32_be(key, extra_id);
    key.push_back(static_cast<char>(kind));
    return key;
}

std::string_view as_text(dstd::ByteView bytes) {
    return std::string_view(bytes.as<char>(), bytes.size());
}

dstd::String own(std::string_view bytes) {
    return dstd::String(bytes.data(), bytes.size());
}

std::uint32_t tail_u32(std::string_view key, std::size_t offset) {
    return layout::read_u32(key.substr(offset));
}

dstd::Unexpected<dstd::Error> parse_error(const std::exception& error) {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::Parse, error.what()));
}

kv::Namespace read_ns(const kv::NamespaceSpec& spec) {
    kv::Namespace ns;
    ns.spec = spec;
    return ns;
}

kv::Namespace read_ns(Ext ext) { return read_ns(ext_spec(ext)); }

bool ready(const Snapshot& snapshot, const FileRef& file, Ext ext) {
    const kv::Snapshot* snap = IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return false;
    return snap->copy_info(read_ns(ext), IndexAccess::scope(file)).state ==
           kv::ScopeState::Ready;
}

/// Calls fn(inner key, payload) for each record under `prefix` whose value
/// carries the header of (ext, kind). fn returns false to stop. A decoder that
/// throws becomes a Parse error.
template <class Fn>
Result<void> scan_payloads(const Snapshot& snapshot, const FileRef& file,
                           const kv::Namespace& ns, Ext ext, std::uint8_t kind,
                           std::string_view prefix, Fn&& fn) {
    const kv::Snapshot* snap = IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return {};
    kv::Cursor cursor =
        snap->scan(ns, IndexAccess::scope(file), dstd::ByteView(prefix));
    for (; !cursor.at_end(); cursor.next()) {
        auto value = cursor.value();
        if (!value) return dstd::Unexpected<dstd::Error>(value.error());
        const auto body = layout::payload(as_text(*value), ext, kind);
        if (!body) continue;
        bool more = true;
        try {
            more = fn(as_text(cursor.key()), *body);
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
Result<void> scan_keys(const Snapshot& snapshot, const FileRef& file,
                       const kv::Namespace& ns, std::string_view prefix,
                       Fn&& fn) {
    const kv::Snapshot* snap = IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return {};
    kv::Cursor cursor =
        snap->scan(ns, IndexAccess::scope(file), dstd::ByteView(prefix));
    for (; !cursor.at_end(); cursor.next()) {
        if (!fn(as_text(cursor.key()))) break;
    }
    if (cursor.error()) return dstd::Unexpected<dstd::Error>(*cursor.error());
    return {};
}

/// fn(payload) of the record at `key`; nullopt when it is absent or its header
/// is another type or version.
template <class Fn>
auto get_decoded(const Snapshot& snapshot, const FileRef& file,
                 const kv::Namespace& ns, Ext ext, std::uint8_t kind,
                 const std::string& key, Fn&& fn)
    -> Result<dstd::Optional<decltype(fn(std::string_view{}))>> {
    using Out = decltype(fn(std::string_view{}));
    const kv::Snapshot* snap = IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return dstd::Optional<Out>{};
    auto found = snap->get(ns, IndexAccess::scope(file), dstd::ByteView(key));
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) return dstd::Optional<Out>{};
    const auto body = layout::payload(as_text((*found)->bytes()), ext, kind);
    if (!body) return dstd::Optional<Out>{};
    try {
        return dstd::Optional<Out>(fn(*body));
    } catch (const std::exception& error) {
        return parse_error(error);
    }
}

Result<dstd::Optional<dstd::String>> get_string(const Snapshot& snapshot,
                                                const FileRef& file, Ext ext,
                                                std::uint8_t kind,
                                                const std::string& key) {
    return get_decoded(snapshot, file, read_ns(ext), ext, kind, key,
                       [](std::string_view body) { return own(body); });
}

/// The highest format version of each plugin namespace the store declares.
Result<std::map<std::string, std::uint32_t>> plugin_versions(
    const kv::Snapshot& snap) {
    const std::string_view prefix(store_layout::PLUGIN_NAMESPACE_PREFIX);
    auto declared = snap.namespaces();
    if (!declared) return dstd::Unexpected<dstd::Error>(declared.error());
    std::map<std::string, std::uint32_t> versions;
    for (const kv::NamespaceDecl& entry : *declared) {
        const std::string_view name(entry.name.data(), entry.name.size());
        if (!name.starts_with(prefix)) continue;
        std::uint32_t& known =
            versions[std::string(name.substr(prefix.size()))];
        known = std::max(known, entry.format_version);
    }
    return versions;
}

void put_value(FileBuild& build, Ext ext, std::uint8_t kind,
               const std::string& key, std::string_view body) {
    const std::string value = layout::with_header(ext, kind, body);
    IndexAccess::put(build, ext_spec(ext), dstd::ByteView(key),
                     dstd::ByteView(value));
}

void put_bare(FileBuild& build, Ext ext, const std::string& key) {
    IndexAccess::put(build, ext_spec(ext), dstd::ByteView(key),
                     dstd::ByteView());
}

bool builtin(FileBuild& build, store::IndexExtension ext) {
    if (ext != Ext::Plugin && ext != Ext::Host) return true;
    IndexAccess::record_error(build,
                              dstd::Error(dstd::Condition::InvalidArgument,
                                          "index: not a built-in extension"));
    return false;
}

constexpr std::uint32_t AGG_META_VERSION = 0;

bool builtin_ext(Ext ext) { return ext != Ext::Plugin && ext != Ext::Host; }

kv::NamespaceSpec agg_meta_spec(dstd::StringView name) {
    kv::NamespaceSpec spec;
    const std::string full = store_layout::agg_meta_namespace_name(
        std::string_view(name.data(), name.size()));
    spec.name.assign(full.data(), full.size());
    spec.format_version = AGG_META_VERSION;
    return spec;
}

Result<void> invalid_result(std::string_view message) {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument, message));
}

}  // namespace

dftracer::utils::store::NamespaceSpec ext_spec(store::IndexExtension ext) {
    kv::NamespaceSpec spec;
    const std::string name = store_layout::namespace_name(ext);
    spec.name.assign(name.data(), name.size());
    spec.format_version = layout::ext_version(ext);
    return spec;
}

dftracer::utils::store::NamespaceSpec plugin_spec(dstd::StringView name,
                                                  std::uint32_t version) {
    kv::NamespaceSpec spec;
    const std::string full = store_layout::plugin_namespace_name(name);
    spec.name.assign(full.data(), full.size());
    spec.format_version = version;
    return spec;
}

Result<dstd::Vector<gzip::GzipMemberRecord>> members(const Snapshot& snapshot,
                                                     const FileRef& file) {
    dstd::Vector<gzip::GzipMemberRecord> out;
    if (!ready(snapshot, file, Ext::Members)) return out;
    auto scanned = scan_payloads(
        snapshot, file, read_ns(Ext::Members), Ext::Members,
        layout::members::MEMBER, kind_key(layout::members::MEMBER),
        [&](std::string_view key, std::string_view body) {
            out.push_back(decoding::decode_gzip_member(tail_u32(key, 1), body));
            return true;
        });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

Result<dstd::Vector<gzip::ChunkSpan>> spans(const Snapshot& snapshot,
                                            const FileRef& file) {
    auto found = members(snapshot, file);
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    dstd::Vector<gzip::ChunkSpan> out;
    out.reserve(found->size());
    for (const auto& member : *found) {
        out.push_back({member.uc_offset, member.uc_size, member.first_line_num,
                       member.last_line_num});
    }
    return out;
}

Result<dstd::Optional<dstd::String>> restart_window(const Snapshot& snapshot,
                                                    const FileRef& file,
                                                    std::uint64_t member_idx) {
    return get_string(
        snapshot, file, Ext::Members, layout::members::RESTART,
        granule_key(Ext::Members, layout::members::RESTART, member_idx));
}

Result<dstd::Optional<store::FileMetadataResult>> file_metadata(
    const Snapshot& snapshot, const FileRef& file) {
    return get_decoded(snapshot, file, read_ns(Ext::Members), Ext::Members,
                       layout::members::METADATA,
                       file_key(Ext::Members, layout::members::METADATA),
                       [](std::string_view body) {
                           return decoding::decode_metadata_record(body);
                       });
}

Result<dstd::Vector<schemas::dft::ChunkStatisticsResult>> chunk_stats(
    const Snapshot& snapshot, const FileRef& file) {
    dstd::Vector<schemas::dft::ChunkStatisticsResult> out;
    auto scanned = scan_payloads(
        snapshot, file, read_ns(Ext::Stats), Ext::Stats, sk::CHUNK_STATS,
        kind_key(sk::CHUNK_STATS),
        [&](std::string_view key, std::string_view body) {
            out.push_back({tail_u32(key, 1),
                           decoding::decode_chunk_statistics_value(body)});
            return true;
        });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

Result<dstd::Optional<schemas::dft::MergedStatisticsResult>> scalar_stats(
    const Snapshot& snapshot, const FileRef& file) {
    return get_decoded(
        snapshot, file, read_ns(Ext::Stats), Ext::Stats, sk::FILE_SCALAR_STATS,
        file_key(Ext::Stats, sk::FILE_SCALAR_STATS), [](std::string_view body) {
            return store::internal::decode_file_scalar_stats_value(body);
        });
}

Result<dstd::Optional<dstd::StringViewMap<std::uint64_t>>> pid_tid_counts(
    const Snapshot& snapshot, const FileRef& file) {
    return get_decoded(snapshot, file, read_ns(Ext::Stats), Ext::Stats,
                       sk::FILE_PID_TID_COUNTS,
                       file_key(Ext::Stats, sk::FILE_PID_TID_COUNTS),
                       [](std::string_view body) {
                           return decoding::decode_count_map_value(body);
                       });
}

Result<dstd::Optional<dstd::HashMap<std::uint64_t, store::ChunkMetadata>>>
chunk_meta(const Snapshot& snapshot, const FileRef& file) {
    using Map = dstd::HashMap<std::uint64_t, store::ChunkMetadata>;
    if (!ready(snapshot, file, Ext::Metadata)) return dstd::Optional<Map>{};
    Map out;
    auto scanned = scan_payloads(
        snapshot, file, read_ns(Ext::Metadata), Ext::Metadata,
        layout::metadata_kind::CHUNK, kind_key(layout::metadata_kind::CHUNK),
        [&](std::string_view key, std::string_view body) {
            if (auto metadata = layout::decode_chunk_metadata(body))
                out.emplace(tail_u32(key, 1), std::move(*metadata));
            return true;
        });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return dstd::Optional<Map>(std::move(out));
}

Result<dstd::Vector<std::pair<dstd::String, store::PathStat>>> catalog(
    const Snapshot& snapshot, const FileRef& file) {
    dstd::Vector<std::pair<dstd::String, store::PathStat>> out;
    auto scanned = scan_payloads(
        snapshot, file, read_ns(Ext::Catalog), Ext::Catalog,
        layout::catalog_kind::PATH, kind_key(layout::catalog_kind::PATH),
        [&](std::string_view key, std::string_view body) {
            if (auto stat = layout::decode_path_stat(body))
                out.emplace_back(own(key.substr(1)), *stat);
            return true;
        });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

Result<dstd::Optional<dstd::String>> schema(const Snapshot& snapshot,
                                            const FileRef& file) {
    if (!ready(snapshot, file, Ext::Profile))
        return dstd::Optional<dstd::String>{};
    return get_string(snapshot, file, Ext::Profile, layout::profile_kind::ID,
                      file_key(Ext::Profile, layout::profile_kind::ID));
}

Result<dstd::Optional<dstd::String>> rowset(const Snapshot& snapshot,
                                            const FileRef& file,
                                            dstd::StringView name) {
    if (!ready(snapshot, file, Ext::Rowset))
        return dstd::Optional<dstd::String>{};
    return get_string(
        snapshot, file, Ext::Rowset, layout::rowset_kind::FRAME,
        path_prefix(Ext::Rowset, layout::rowset_kind::FRAME, name));
}

Result<dstd::Vector<dstd::String>> paths(const Snapshot& snapshot,
                                         const FileRef& file,
                                         store::IndexExtension ext) {
    dstd::Vector<dstd::String> out;
    if (ext == Ext::Plugin || ext == Ext::Host) return out;
    auto scanned = scan_keys(snapshot, file, read_ns(ext), kind_key(pk::PATHS),
                             [&](std::string_view key) {
                                 out.push_back(own(key.substr(1)));
                                 return true;
                             });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

Result<GranuleList> granules(const Snapshot& snapshot, const FileRef& file,
                             store::IndexExtension ext, dstd::StringView path) {
    GranuleList out;
    if (ext == Ext::Plugin || ext == Ext::Host) return out;
    const std::string prefix = path_prefix(ext, pk::DATA, path);
    auto scanned = scan_payloads(
        snapshot, file, read_ns(ext), ext, pk::DATA, prefix,
        [&](std::string_view key, std::string_view body) {
            if (key.size() == prefix.size() + 4)
                out.emplace_back(tail_u32(key, prefix.size()), own(body));
            return true;
        });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

Result<dstd::Optional<dstd::String>> path_file(const Snapshot& snapshot,
                                               const FileRef& file,
                                               store::IndexExtension ext,
                                               dstd::StringView path) {
    if (ext == Ext::Plugin || ext == Ext::Host)
        return dstd::Optional<dstd::String>{};
    return get_string(snapshot, file, ext, pk::FILE,
                      file_key(ext, pk::FILE, path));
}

Result<bool> has_postings(const Snapshot& snapshot, const FileRef& file,
                          dstd::StringView path) {
    bool found = false;
    auto scanned = scan_keys(snapshot, file, read_ns(Ext::Postings),
                             path_prefix(Ext::Postings, pk::IN_FILE, path),
                             [&](std::string_view) {
                                 found = true;
                                 return false;
                             });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return found;
}

Result<bool> has_posting(const Snapshot& snapshot, const FileRef& file,
                         dstd::StringView path, std::uint64_t value_hash) {
    const kv::Snapshot* snap = IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return false;
    std::string key = path_prefix(Ext::Postings, pk::IN_FILE, path);
    dstd::append_u64_be(key, value_hash);
    auto found = snap->get(read_ns(Ext::Postings), IndexAccess::scope(file),
                           dstd::ByteView(key));
    if (!found) return dstd::Unexpected<dstd::Error>(found.error());
    return found->has_value();
}

Result<dstd::Vector<std::uint64_t>> posting_granules(const Snapshot& snapshot,
                                                     const FileRef& file,
                                                     dstd::StringView path,
                                                     std::uint64_t value_hash) {
    dstd::Vector<std::uint64_t> out;
    std::string prefix = path_prefix(Ext::Postings, pk::IN_GRANULE, path);
    dstd::append_u64_be(prefix, value_hash);
    auto scanned =
        scan_keys(snapshot, file, read_ns(Ext::Postings), prefix,
                  [&](std::string_view key) {
                      if (key.size() == prefix.size() + 4)
                          out.push_back(tail_u32(key, prefix.size()));
                      return true;
                  });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    return out;
}

Result<dstd::Vector<PluginCopy>> plugins(const Snapshot& snapshot,
                                         const FileRef& file) {
    dstd::Vector<PluginCopy> out;
    const kv::Snapshot* snap = IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return out;
    auto versions = plugin_versions(*snap);
    if (!versions) return dstd::Unexpected<dstd::Error>(versions.error());
    for (const auto& [name, version] : *versions) {
        const kv::CopyInfo info = snap->copy_info(
            read_ns(plugin_spec(name, version)), IndexAccess::scope(file));
        if (info.state != kv::ScopeState::Ready &&
            info.state != kv::ScopeState::Failed)
            continue;
        out.push_back({own(name), version, info.params_hash,
                       info.state == kv::ScopeState::Ready});
    }
    return out;
}

Result<dstd::Optional<PluginData>> plugin(const Snapshot& snapshot,
                                          const FileRef& file,
                                          dstd::StringView name) {
    const kv::Snapshot* snap = IndexAccess::store_snapshot(snapshot);
    if (snap == nullptr) return dstd::Optional<PluginData>{};
    auto versions = plugin_versions(*snap);
    if (!versions) return dstd::Unexpected<dstd::Error>(versions.error());
    const auto known = versions->find(std::string(name));
    if (known == versions->end()) return dstd::Optional<PluginData>{};
    const kv::Namespace ns = read_ns(plugin_spec(name, known->second));
    const kv::CopyInfo info = snap->copy_info(ns, IndexAccess::scope(file));
    if (info.state != kv::ScopeState::Ready &&
        info.state != kv::ScopeState::Failed)
        return dstd::Optional<PluginData>{};
    PluginData data;
    const std::string prefix = path_prefix(Ext::Plugin, pk::DATA, name);
    auto scanned =
        scan_payloads(snapshot, file, ns, Ext::Plugin, pk::DATA, prefix,
                      [&](std::string_view key, std::string_view body) {
                          if (key.size() == prefix.size() + 4)
                              data.granules.emplace_back(
                                  tail_u32(key, prefix.size()), own(body));
                          return true;
                      });
    if (!scanned) return dstd::Unexpected<dstd::Error>(scanned.error());
    auto whole = get_decoded(snapshot, file, ns, Ext::Plugin, pk::FILE,
                             file_key(Ext::Plugin, pk::FILE, name),
                             [](std::string_view body) { return own(body); });
    if (!whole) return dstd::Unexpected<dstd::Error>(whole.error());
    data.file = std::move(*whole);
    return dstd::Optional<PluginData>(std::move(data));
}

void put_file_metadata(FileBuild& build, std::uint64_t checkpoint_size,
                       std::uint64_t total_lines, std::uint64_t total_uc_size,
                       bool truncated) {
    put_value(build, Ext::Members, layout::members::METADATA,
              file_key(Ext::Members, layout::members::METADATA),
              encoding::encode_metadata_record(
                  checkpoint_size, total_lines, total_uc_size,
                  truncated ? encoding::METADATA_FLAG_TRUNCATED : 0));
}

void put_gzip_member(FileBuild& build, const gzip::GzipMemberRecord& member) {
    put_value(
        build, Ext::Members, layout::members::MEMBER,
        granule_key(Ext::Members, layout::members::MEMBER, member.member_idx),
        encoding::encode_gzip_member_value(member));
}

void put_restart_window(FileBuild& build, std::uint64_t member_idx,
                        dstd::StringView window) {
    put_value(build, Ext::Members, layout::members::RESTART,
              granule_key(Ext::Members, layout::members::RESTART, member_idx),
              window);
}

void put_chunk_statistics(FileBuild& build, std::uint64_t checkpoint_idx,
                          const schemas::dft::ChunkStatistics& stats) {
    put_value(build, Ext::Stats, sk::CHUNK_STATS,
              granule_key(Ext::Stats, sk::CHUNK_STATS, checkpoint_idx),
              encoding::encode_chunk_statistics_value(stats));
}

void put_chunk_metadata(FileBuild& build, std::uint64_t checkpoint_idx,
                        const store::ChunkMetadata& metadata) {
    put_value(build, Ext::Metadata, layout::metadata_kind::CHUNK,
              granule_key(Ext::Metadata, layout::metadata_kind::CHUNK,
                          checkpoint_idx),
              layout::encode_chunk_metadata(metadata));
}

void put_file_scalar_stats(FileBuild& build,
                           const schemas::dft::ChunkStatistics& stats,
                           std::uint64_t num_chunks) {
    put_value(
        build, Ext::Stats, sk::FILE_SCALAR_STATS,
        file_key(Ext::Stats, sk::FILE_SCALAR_STATS),
        store::internal::encode_file_scalar_stats_value(stats, num_chunks));
}

void put_file_pid_tid_counts(FileBuild& build,
                             const dstd::StringViewMap<std::uint64_t>& counts) {
    put_value(build, Ext::Stats, sk::FILE_PID_TID_COUNTS,
              file_key(Ext::Stats, sk::FILE_PID_TID_COUNTS),
              encoding::encode_count_map_value(counts));
}

void put_profile(FileBuild& build, dstd::StringView profile_id) {
    put_value(build, Ext::Profile, layout::profile_kind::ID,
              file_key(Ext::Profile, layout::profile_kind::ID), profile_id);
}

void put_catalog_path(FileBuild& build, dstd::StringView path,
                      const store::PathStat& stat) {
    put_value(build, Ext::Catalog, layout::catalog_kind::PATH,
              file_key(Ext::Catalog, layout::catalog_kind::PATH, path),
              layout::encode_path_stat(stat));
}

void put_rowset(FileBuild& build, dstd::StringView name,
                dstd::StringView frame) {
    put_value(build, Ext::Rowset, layout::rowset_kind::FRAME,
              path_prefix(Ext::Rowset, layout::rowset_kind::FRAME, name),
              frame);
}

void put_path(FileBuild& build, store::IndexExtension ext,
              dstd::StringView path) {
    if (!builtin(build, ext)) return;
    put_bare(build, ext, file_key(ext, pk::PATHS, path));
}

void put_path_granule(FileBuild& build, store::IndexExtension ext,
                      dstd::StringView path, std::uint64_t granule,
                      dstd::StringView payload) {
    if (!builtin(build, ext)) return;
    put_value(build, ext, pk::DATA, path_granule_key(ext, path, granule),
              payload);
}

void erase_path(FileBuild& build, store::IndexExtension ext,
                dstd::StringView path) {
    if (!builtin(build, ext)) return;
    const kv::NamespaceSpec& spec = ext_spec(ext);
    // A key is NUL free, so one NUL past it bounds exactly that key.
    const auto erase_key = [&](std::string key) {
        std::string end = key;
        end.push_back('\0');
        IndexAccess::erase(build, spec, dstd::ByteView(key),
                           dstd::ByteView(end));
    };
    erase_key(file_key(ext, pk::PATHS, path));
    erase_key(file_key(ext, pk::FILE, path));
    const std::string data = path_prefix(ext, pk::DATA, path);
    const std::string end = layout::prefix_end(data);
    IndexAccess::erase(build, spec, dstd::ByteView(data), dstd::ByteView(end));
}

void put_path_file(FileBuild& build, store::IndexExtension ext,
                   dstd::StringView path, dstd::StringView payload) {
    if (!builtin(build, ext)) return;
    put_value(build, ext, pk::FILE, file_key(ext, pk::FILE, path), payload);
}

void put_posting(FileBuild& build, dstd::StringView path,
                 std::uint64_t value_hash) {
    std::string key = path_prefix(Ext::Postings, pk::IN_FILE, path);
    dstd::append_u64_be(key, value_hash);
    put_bare(build, Ext::Postings, key);
}

void put_posting_granule(FileBuild& build, dstd::StringView path,
                         std::uint64_t value_hash, std::uint64_t granule) {
    std::string key = path_prefix(Ext::Postings, pk::IN_GRANULE, path);
    dstd::append_u64_be(key, value_hash);
    dstd::append_u32_be(key, static_cast<std::uint32_t>(granule));
    put_bare(build, Ext::Postings, key);
}

void put_agg_partial(FileBuild& build, std::uint64_t chunk,
                     std::uint32_t cat_id, std::uint32_t name_id,
                     std::uint32_t host_id, std::uint32_t extra_id,
                     std::uint8_t kind, dstd::StringView payload) {
    put_value(build, Ext::Agg, layout::agg_kind::PARTIAL,
              agg_partial_key(chunk, cat_id, name_id, host_id, extra_id, kind),
              payload);
}

void put_agg_partial_value(FileBuild& build, std::uint64_t chunk,
                           std::uint32_t cat_id, std::uint32_t name_id,
                           std::uint32_t host_id, std::uint32_t extra_id,
                           std::uint8_t kind, dstd::StringView value) {
    const std::string key =
        agg_partial_key(chunk, cat_id, name_id, host_id, extra_id, kind);
    IndexAccess::put(build, ext_spec(Ext::Agg), dstd::ByteView(key),
                     dstd::ByteView(value.data(), value.size()));
}

void put_agg_chunk_columns_value(FileBuild& build, std::uint64_t chunk,
                                 dstd::StringView value) {
    const std::string key =
        granule_key(Ext::Agg, layout::agg_kind::COLUMNS, chunk);
    IndexAccess::put(build, ext_spec(Ext::Agg), dstd::ByteView(key),
                     dstd::ByteView(value.data(), value.size()));
}

void put_agg_chunk_columns(FileBuild& build, std::uint64_t chunk,
                           dstd::StringView payload) {
    put_value(build, Ext::Agg, layout::agg_kind::COLUMNS,
              granule_key(Ext::Agg, layout::agg_kind::COLUMNS, chunk), payload);
}

void put_agg_dictionary_entry(FileBuild& build, std::uint32_t id,
                              dstd::StringView text) {
    std::string key = file_key(Ext::Agg, layout::agg_kind::DICT);
    dstd::append_u32_be(key, id);
    put_value(build, Ext::Agg, layout::agg_kind::DICT, key, text);
}

namespace {

template <class Granules, class File>
void write_plugin(FileBuild& build, dstd::StringView name,
                  std::uint32_t version, std::uint64_t params_hash,
                  const Granules& granules, const File& file) {
    const kv::NamespaceSpec spec = plugin_spec(name, version);
    const std::string listed = file_key(Ext::Plugin, pk::PATHS, name);
    IndexAccess::put(build, spec, dstd::ByteView(listed), dstd::ByteView());
    for (const auto& [granule, payload] : granules) {
        const std::string key = path_granule_key(Ext::Plugin, name, granule);
        const std::string value = layout::with_header(
            Ext::Plugin, pk::DATA,
            std::string_view(payload.data(), payload.size()));
        IndexAccess::put(build, spec, dstd::ByteView(key),
                         dstd::ByteView(value));
    }
    if (file) {
        const std::string key = file_key(Ext::Plugin, pk::FILE, name);
        const std::string value =
            layout::with_header(Ext::Plugin, pk::FILE,
                                std::string_view(file->data(), file->size()));
        IndexAccess::put(build, spec, dstd::ByteView(key),
                         dstd::ByteView(value));
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
    dstd::Vector<kv::NamespaceSpec> specs;
    for (const store::IndexExtension ext : keep)
        if (builtin_ext(ext)) specs.push_back(ext_spec(ext));
    IndexAccess::drop_unwritten(
        build, dstd::Span<const kv::NamespaceSpec>(specs.data(), specs.size()));
}

Result<FileBuild> rebuild(const Index& index, dstd::StringView path) {
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

coro::CoroTask<Result<void>> put_agg_meta(const Index& index,
                                          dstd::StringView name,
                                          dstd::StringView payload) {
    auto opened = IndexAccess::open_store(index);
    if (!opened) co_return dstd::Unexpected<dstd::Error>(opened.error());
    kv::Store store = std::move(*opened);
    auto registered = store.ns(agg_meta_spec(name));
    if (!registered)
        co_return dstd::Unexpected<dstd::Error>(registered.error());
    const kv::Namespace ns = *registered;
    const kv::ScopeKey scope = kv::ScopeKey::root();
    const std::string key =
        file_key(Ext::Agg, layout::agg_kind::META,
                 std::string_view(name.data(), name.size()));
    const std::string value =
        layout::with_header(Ext::Agg, layout::agg_kind::META,
                            std::string_view(payload.data(), payload.size()));
    kv::Batch batch = store.building_batch(ns, scope);
    batch.put(dstd::ByteView(key), dstd::ByteView(value));
    auto written = co_await store.commit(std::move(batch));
    if (!written) co_return dstd::Unexpected<dstd::Error>(written.error());
    const kv::Publish target{&ns, &scope, 0, nullptr};
    auto published =
        co_await store.publish(dstd::Span<const kv::Publish>(&target, 1));
    if (!published) co_return dstd::Unexpected<dstd::Error>(published.error());
    note_applied(IndexAccess::root(index),
                 dstd::Span<const kv::Publish>(&target, 1));
    co_return Result<void>{};
}

coro::CoroTask<Result<void>> delete_agg_meta(const Index& index,
                                             dstd::StringView name) {
    auto found = IndexAccess::find_store(index);
    if (!found) co_return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) co_return Result<void>{};
    kv::Store store = **found;
    auto registered = store.ns(agg_meta_spec(name));
    if (!registered)
        co_return dstd::Unexpected<dstd::Error>(registered.error());
    const kv::Namespace ns = *registered;
    const kv::ScopeKey scope = kv::ScopeKey::root();
    auto taken = store.snapshot();
    if (!taken) co_return dstd::Unexpected<dstd::Error>(taken.error());
    if (taken->copy_info(ns, scope).state == kv::ScopeState::Absent)
        co_return Result<void>{};
    const kv::Publish target{&ns, &scope, 0, nullptr,
                             kv::Publish::Change::Dropped};
    auto published =
        co_await store.publish(dstd::Span<const kv::Publish>(&target, 1));
    if (!published) co_return dstd::Unexpected<dstd::Error>(published.error());
    note_applied(IndexAccess::root(index),
                 dstd::Span<const kv::Publish>(&target, 1));
    co_return Result<void>{};
}

coro::CoroTask<Result<void>> remove_extension(
    const Index& index, store::IndexExtension ext,
    dstd::Span<const dstd::StringView> paths) {
    if (!builtin_ext(ext))
        co_return invalid_result("index: not a built-in extension");
    auto found = IndexAccess::find_store(index);
    if (!found) co_return dstd::Unexpected<dstd::Error>(found.error());
    if (!*found) co_return Result<void>{};
    kv::Store store = **found;
    auto registered = store.ns(ext_spec(ext));
    if (!registered)
        co_return dstd::Unexpected<dstd::Error>(registered.error());
    const kv::Namespace ns = *registered;
    auto taken = store.snapshot();
    if (!taken) co_return dstd::Unexpected<dstd::Error>(taken.error());
    dstd::Vector<kv::ScopeKey> scopes;
    scopes.reserve(paths.size());
    for (const dstd::StringView path : paths) {
        kv::ScopeKey scope = kv::ScopeKey::file(path);
        if (taken->copy_info(ns, scope).state == kv::ScopeState::Absent)
            continue;
        scopes.push_back(std::move(scope));
    }
    if (scopes.empty()) co_return Result<void>{};
    dstd::Vector<kv::Publish> targets;
    targets.reserve(scopes.size());
    for (const kv::ScopeKey& scope : scopes) {
        targets.push_back(
            kv::Publish{&ns, &scope, 0, nullptr, kv::Publish::Change::Dropped});
    }
    auto published = co_await store.publish(
        dstd::Span<const kv::Publish>(targets.data(), targets.size()));
    if (!published) co_return dstd::Unexpected<dstd::Error>(published.error());
    note_applied(IndexAccess::root(index),
                 dstd::Span<const kv::Publish>(targets.data(), targets.size()));
    co_return Result<void>{};
}

}  // namespace dftracer::utils::index
