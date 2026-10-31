#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/index/store/error.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/agg_row_reader.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/internal/index_encoding.h>
#include <dftracer/utils/index/store/internal/payload_codec.h>
#include <dftracer/utils/index/store/internal/statistics_codec.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/store_backend.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/store/snapshot.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {

namespace kv = dftracer::utils::store;

using namespace dftracer::utils::index::gzip;
using namespace dftracer::utils::index::store::internal;
using layout::Ext;
namespace sk = layout::stats_kind;
namespace pk = layout::path_kind;

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

namespace {

std::uint32_t fid(int file_id) { return static_cast<std::uint32_t>(file_id); }

std::uint32_t tail_u32(std::string_view key, std::size_t offset) {
    return layout::read_u32(key.substr(offset));
}

index::schemas::dft::ChunkStatistics decode_chunk_statistics_value(
    std::string_view value) {
    Cursor cursor(value);
    index::schemas::dft::ChunkStatistics stats;
    stats.total_events = cursor.u64();
    stats.min_timestamp_us = cursor.u64();
    stats.max_timestamp_us = cursor.u64();
    stats.duration_sum_us = cursor.i64();
    stats.duration_min_us = cursor.u64();
    stats.duration_max_us = cursor.u64();
    stats.duration_count = cursor.u64();
    stats.duration_m2 = cursor.f64();

    auto duration_sketch = cursor.blob_view();
    if (!duration_sketch.empty()) {
        stats.duration_sketch = dataframe::DDSketch::deserialize(
            reinterpret_cast<const std::uint8_t*>(duration_sketch.data()),
            duration_sketch.size());
    }

    auto duration_histogram = cursor.str();
    if (!duration_histogram.empty()) {
        stats.duration_histogram =
            dataframe::Log2Histogram::from_json(duration_histogram);
    }

    auto name_sketches = cursor.blob_view();
    if (!name_sketches.empty()) {
        stats.name_duration_sketches = index::schemas::dft::ChunkStatistics::
            deserialize_name_duration_sketches(
                reinterpret_cast<const std::uint8_t*>(name_sketches.data()),
                name_sketches.size());
    }

    stats.name_duration_histograms =
        index::schemas::dft::ChunkStatistics::parse_histogram_map_json(
            cursor.str());
    stats.name_duration_sums =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());
    stats.name_duration_sum_sqs =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());
    stats.name_category =
        index::schemas::dft::ChunkStatistics::parse_string_map_json(
            cursor.str());

    auto cat_sketches = cursor.blob_view();
    if (!cat_sketches.empty()) {
        stats.cat_duration_sketches = index::schemas::dft::ChunkStatistics::
            deserialize_name_duration_sketches(
                reinterpret_cast<const std::uint8_t*>(cat_sketches.data()),
                cat_sketches.size());
    }
    stats.cat_duration_sums =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());

    auto pid_sketches = cursor.blob_view();
    if (!pid_sketches.empty()) {
        stats.pid_duration_sketches = index::schemas::dft::ChunkStatistics::
            deserialize_name_duration_sketches(
                reinterpret_cast<const std::uint8_t*>(pid_sketches.data()),
                pid_sketches.size());
    }
    stats.pid_duration_sums =
        index::schemas::dft::ChunkStatistics::parse_double_map_json(
            cursor.str());

    return stats;
}

index::gzip::GzipMemberRecord decode_gzip_member(std::string_view key,
                                                 std::string_view body) {
    index::gzip::GzipMemberRecord member;
    member.member_idx = tail_u32(key, layout::KEY_PREFIX_BYTES);
    Cursor cursor(body);
    member.c_offset = cursor.u64();
    member.c_size = cursor.u64();
    member.uc_offset = cursor.u64();
    member.uc_size = cursor.u64();
    member.first_line_num = cursor.u64();
    member.last_line_num = cursor.u64();
    if (body.size() >= 6 * sizeof(std::uint64_t) + 2) {
        const auto kind = cursor.u8();
        if (kind >
            static_cast<std::uint8_t>(index::gzip::GzipRecordKind::Restart))
            throw DFTUtilsException::cat(ErrorCode::Indexer,
                                         "unknown gzip record kind ", kind);
        member.kind = static_cast<index::gzip::GzipRecordKind>(kind);
        member.bits = cursor.u8();
    }
    return member;
}

StringViewMap<std::uint64_t> decode_count_map_value(std::string_view value) {
    Cursor cursor(value);
    StringViewMap<std::uint64_t> counts;
    auto num_entries = cursor.u32();
    counts.reserve(num_entries);
    for (std::uint32_t i = 0; i < num_entries; ++i) {
        auto key = cursor.str();
        counts.emplace(std::move(key), cursor.u64());
    }
    return counts;
}

}  // namespace

namespace {

struct StoreNamespace {
    std::string name;
    std::uint32_t version = 0;
    std::string owner;
};

}  // namespace

struct IndexDatabase::Impl {
    std::string db_path_;
    IndexOpenMode open_mode_ = IndexOpenMode::ReadWrite;
    std::shared_ptr<StoreRoot> store_;

    std::optional<std::string> raw_get(std::string_view key);
    /// Passes the full index key to fn, in ascending byte order.
    template <typename Fn>
    void raw_scan(std::string_view prefix, Fn&& fn);

    /// nullopt when absent or when the header does not match.
    std::optional<std::string> get(layout::Ext ext, std::uint8_t kind,
                                   std::string_view key);
    template <typename Fn>
    void scan(layout::Ext ext, std::uint8_t kind, std::string_view prefix,
              Fn&& fn);
    template <typename Fn>
    void scan_keys(std::string_view prefix, Fn&& fn);
    /// In key order.
    template <typename Fn>
    void scan_files(layout::Ext ext, std::uint8_t kind,
                    const std::vector<int>& file_ids, Fn&& fn);
    bool any_with_prefix(std::string_view prefix);
    std::optional<layout::FileRecord> file_record(std::string_view path);

    /// Null when the root holds no store yet, where every read answers nothing.
    std::shared_ptr<const kv::Snapshot> snapshot() const;
    std::optional<std::string> store_get(std::string_view key);
    template <typename Fn>
    void store_scan(std::string_view prefix, Fn&& fn);
    /// Highest version of each, in plugin name order.
    std::vector<StoreNamespace> plugin_namespaces() const;
    /// Highest version of each, in record name order.
    std::vector<StoreNamespace> meta_namespaces() const;
    std::vector<std::uint32_t> file_ids_of(const kv::NamespaceSpec& spec) const;
    std::optional<std::string> registered_path(std::uint32_t file_id) const;
    std::optional<std::string> store_manifest(int file_id, layout::Ext ext,
                                              std::string_view plugin);
    std::optional<layout::FileRecord> store_file_record(
        std::string_view path) const;
    template <typename Fn>
    void store_manifests(std::uint32_t file_id, std::string_view plugin,
                         Fn&& fn);
};

namespace {

std::string_view view_of(const dstd::String& value) {
    return std::string_view(value.data(), value.size());
}

constexpr std::string_view PLUGIN_NAMESPACE_PREFIX =
    store_layout::PLUGIN_NAMESPACE_PREFIX;

std::string next_file_id_value(std::uint32_t next) {
    std::string body;
    append_u32_be(body, next);
    return layout::with_header(layout::Ext::Host, layout::host::NEXT_FILE_ID,
                               body);
}

kv::NamespaceSpec namespace_spec(std::string_view name, std::uint32_t version) {
    kv::NamespaceSpec spec;
    spec.name.assign(name.data(), name.size());
    spec.format_version = version;
    return spec;
}

kv::NamespaceSpec ext_spec(layout::Ext ext) {
    return namespace_spec(store_layout::namespace_name(ext),
                          layout::ext_version(ext));
}

std::optional<kv::ScopeKey> file_scope(const std::string& path) {
    kv::ScopeKey scope;
    if (path.empty()) return std::nullopt;
    scope.kind = kv::ScopeKind::File;
    scope.paths.push_back(dstd::String(path.data(), path.size()));
    return scope;
}

kv::ScopeKey root_scope() {
    kv::ScopeKey scope;
    scope.kind = kv::ScopeKind::Root;
    return scope;
}

void throw_store_error(const char* what, const dstd::Error& error) {
    const dstd::String text = error.format();
    throw std::runtime_error(std::string(what) + ": " +
                             std::string(text.data(), text.size()));
}

}  // namespace

template <typename Fn>
void IndexDatabase::Impl::raw_scan(std::string_view prefix, Fn&& fn) {
    // The snapshot lives in the root's pool. The caller's work on each record
    // stays in the caller's pool.
    memory::Pool& caller = memory::current_pool();
    memory::Scope in_store(store_->pool());
    store_scan(prefix, [&](std::string_view key, std::string_view value) {
        memory::Scope back(caller);
        return fn(key, value);
    });
}

std::optional<std::string> IndexDatabase::Impl::raw_get(std::string_view key) {
    memory::Scope in_store(store_->pool());
    return store_get(key);
}

std::optional<std::string> IndexDatabase::Impl::get(layout::Ext ext,
                                                    std::uint8_t kind,
                                                    std::string_view key) {
    auto value = raw_get(key);
    if (!value) return std::nullopt;
    auto body = layout::payload(*value, ext, kind);
    if (!body) return std::nullopt;
    return std::string(*body);
}

template <typename Fn>
void IndexDatabase::Impl::scan(layout::Ext ext, std::uint8_t kind,
                               std::string_view prefix, Fn&& fn) {
    raw_scan(prefix, [&](std::string_view key, std::string_view value) {
        if (auto body = layout::payload(value, ext, kind)) fn(key, *body);
        return true;
    });
}

template <typename Fn>
void IndexDatabase::Impl::scan_keys(std::string_view prefix, Fn&& fn) {
    raw_scan(prefix, [&](std::string_view key, std::string_view) {
        fn(key);
        return true;
    });
}

template <typename Fn>
void IndexDatabase::Impl::scan_files(layout::Ext ext, std::uint8_t kind,
                                     const std::vector<int>& file_ids,
                                     Fn&& fn) {
    std::vector<std::uint32_t> ids;
    ids.reserve(file_ids.size());
    for (int file_id : file_ids)
        if (file_id >= 0) ids.push_back(fid(file_id));
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.empty()) return;
    for (std::uint32_t file_id : ids) {
        scan(ext, kind, layout::file_prefix(ext, kind, file_id),
             [&](std::string_view key, std::string_view body) {
                 fn(static_cast<int>(file_id), key, body);
             });
    }
}

bool IndexDatabase::Impl::any_with_prefix(std::string_view prefix) {
    bool found = false;
    raw_scan(prefix, [&](std::string_view, std::string_view) {
        found = true;
        return false;
    });
    return found;
}

std::optional<layout::FileRecord> IndexDatabase::Impl::file_record(
    std::string_view path) {
    // A file record carries its own header, so it skips get().
    auto value = raw_get(layout::file_by_path_key(path));
    if (!value) return std::nullopt;
    return layout::decode_file_record(*value);
}

std::shared_ptr<const kv::Snapshot> IndexDatabase::Impl::snapshot() const {
    return store_->snapshot();
}

std::vector<StoreNamespace> IndexDatabase::Impl::plugin_namespaces() const {
    const std::string_view prefix(PLUGIN_NAMESPACE_PREFIX);
    std::vector<StoreNamespace> out;
    const auto held = snapshot();
    if (!held) return out;
    const auto declared = held->namespaces();
    if (!declared)
        throw_store_error("store index: list namespaces", declared.error());
    for (const kv::NamespaceDecl& entry : *declared) {
        const std::string_view name = view_of(entry.name);
        if (!name.starts_with(prefix)) continue;
        const std::string_view plugin = name.substr(prefix.size());
        auto at = std::find_if(out.begin(), out.end(), [&](const auto& known) {
            return known.owner == plugin;
        });
        if (at == out.end()) {
            out.push_back(StoreNamespace{
                std::string(name), entry.format_version, std::string(plugin)});
        } else if (entry.format_version > at->version) {
            at->version = entry.format_version;
        }
    }
    std::sort(out.begin(), out.end(),
              [](const StoreNamespace& a, const StoreNamespace& b) {
                  return a.owner < b.owner;
              });
    return out;
}

std::vector<StoreNamespace> IndexDatabase::Impl::meta_namespaces() const {
    const std::string_view prefix(store_layout::AGG_META_NAMESPACE_PREFIX);
    std::vector<StoreNamespace> out;
    const auto held = snapshot();
    if (!held) return out;
    const auto declared = held->namespaces();
    if (!declared)
        throw_store_error("store index: list namespaces", declared.error());
    for (const kv::NamespaceDecl& entry : *declared) {
        const std::string_view name = view_of(entry.name);
        if (!name.starts_with(prefix)) continue;
        const std::string_view record = name.substr(prefix.size());
        auto at = std::find_if(out.begin(), out.end(), [&](const auto& known) {
            return known.owner == record;
        });
        if (at == out.end()) {
            out.push_back(StoreNamespace{
                std::string(name), entry.format_version, std::string(record)});
        } else if (entry.format_version > at->version) {
            at->version = entry.format_version;
        }
    }
    std::sort(out.begin(), out.end(),
              [](const StoreNamespace& a, const StoreNamespace& b) {
                  return a.owner < b.owner;
              });
    return out;
}

std::vector<std::uint32_t> IndexDatabase::Impl::file_ids_of(
    const kv::NamespaceSpec& spec) const {
    std::vector<std::uint32_t> ids;
    const auto held = snapshot();
    if (!held) return ids;
    const auto listed = held->scopes(spec);
    if (!listed) throw_store_error("store index: list scopes", listed.error());
    for (const kv::ScopeEntry& entry : *listed) {
        if (entry.scope.kind != kv::ScopeKind::File ||
            entry.scope.paths.empty())
            continue;
        const auto id = store_->file_id_for(view_of(entry.scope.paths[0]));
        ids.push_back(fid(id));
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::optional<std::string> IndexDatabase::Impl::registered_path(
    std::uint32_t file_id) const {
    if (file_id == layout::INDEX_WIDE) return std::nullopt;
    return store_->path_of(static_cast<int>(file_id));
}

std::optional<std::string> IndexDatabase::Impl::store_manifest(
    int file_id, layout::Ext ext, std::string_view plugin) {
    const auto path = registered_path(fid(file_id));
    if (!path) return std::nullopt;
    kv::NamespaceSpec spec;
    if (ext == layout::Ext::Plugin) {
        for (const auto& known : plugin_namespaces()) {
            if (known.owner == plugin) {
                spec = namespace_spec(known.name, known.version);
                break;
            }
        }
        if (spec.name.empty()) return std::nullopt;
    } else {
        spec = ext_spec(ext);
    }
    const auto scope = file_scope(*path);
    if (!scope) return std::nullopt;
    const auto held = snapshot();
    if (!held) return std::nullopt;
    const kv::CopyInfo info = held->copy_info(spec, *scope);
    layout::ManifestEntry entry;
    entry.version = spec.format_version;
    entry.params_hash = info.params_hash;
    switch (info.state) {
        case kv::ScopeState::Ready:
            entry.status = layout::ExtStatus::Ready;
            break;
        case kv::ScopeState::Failed:
            entry.status = layout::ExtStatus::Failed;
            break;
        default:
            return std::nullopt;
    }
    return layout::encode_manifest(entry);
}

std::optional<layout::FileRecord> IndexDatabase::Impl::store_file_record(
    std::string_view path) const {
    auto id = store_->find_file_id(path);
    if (!id) return std::nullopt;
    layout::FileRecord record;
    record.file_id = fid(*id);
    const auto scope = file_scope(std::string(path));
    if (!scope) return std::nullopt;
    const auto held = snapshot();
    if (!held) return std::nullopt;
    const kv::CopyInfo info =
        held->copy_info(ext_spec(layout::Ext::Members), *scope);
    if (info.state != kv::ScopeState::Ready &&
        info.state != kv::ScopeState::Failed)
        return std::nullopt;
    if (!info.sources.empty()) {
        record.mtime = info.sources[0].mtime;
        record.size = info.sources[0].size;
        record.hash = info.sources[0].hash;
    }
    return record;
}

template <typename Fn>
void IndexDatabase::Impl::store_manifests(std::uint32_t file_id,
                                          std::string_view plugin, Fn&& fn) {
    const auto path = registered_path(file_id);
    if (!path) return;
    const auto scope = file_scope(*path);
    if (!scope) return;
    const auto held = snapshot();
    if (!held) return;
    for (const auto& known : plugin_namespaces()) {
        if (!std::string_view(known.owner).starts_with(plugin)) continue;
        const auto spec = namespace_spec(known.name, known.version);
        const kv::CopyInfo info = held->copy_info(spec, *scope);
        if (info.state != kv::ScopeState::Ready &&
            info.state != kv::ScopeState::Failed)
            continue;
        layout::ManifestEntry entry;
        entry.version = known.version;
        entry.params_hash = info.params_hash;
        entry.status = info.state == kv::ScopeState::Ready
                           ? layout::ExtStatus::Ready
                           : layout::ExtStatus::Failed;
        fn(layout::plugin_manifest_key(file_id, known.owner),
           layout::encode_manifest(entry));
    }
}

std::optional<std::string> IndexDatabase::Impl::store_get(
    std::string_view key) {
    if (auto host = store_layout::host_target_of(key)) {
        switch (host->kind) {
            case store_layout::HostKind::Format: {
                std::string body;
                append_u32_be(body, IndexDatabase::FORMAT_VERSION);
                return layout::with_header(layout::Ext::Host,
                                           layout::host::FORMAT, body);
            }
            case store_layout::HostKind::FileByPath: {
                const auto record = store_file_record(host->path);
                if (!record) return std::nullopt;
                return layout::encode_file_record(*record);
            }
            case store_layout::HostKind::FileById: {
                const auto path = registered_path(host->file_id);
                if (!path) return std::nullopt;
                return layout::with_header(layout::Ext::Host,
                                           layout::host::FILE_BY_ID, *path);
            }
            case store_layout::HostKind::NextFileId:
                return next_file_id_value(
                    static_cast<std::uint32_t>(store_->next_file_id()));
            case store_layout::HostKind::Manifest: {
                if (!host->ext) return std::nullopt;
                return store_manifest(static_cast<int>(host->file_id),
                                      *host->ext, {});
            }
            case store_layout::HostKind::PluginManifest:
                return store_manifest(static_cast<int>(host->file_id),
                                      layout::Ext::Plugin, host->plugin);
            case store_layout::HostKind::ExtRegistry:
                return std::nullopt;
        }
        return std::nullopt;
    }
    const auto target = store_layout::target_of(key);
    if (!target) return std::nullopt;
    kv::NamespaceSpec spec;
    kv::ScopeKey scope;
    if (target->file_id == layout::INDEX_WIDE) {
        scope = root_scope();
    } else {
        const auto path = registered_path(target->file_id);
        if (!path) return std::nullopt;
        const auto file = file_scope(*path);
        if (!file) return std::nullopt;
        scope = *file;
    }
    if (!target->meta.empty()) {
        for (const auto& known : meta_namespaces())
            if (known.owner == target->meta)
                spec = namespace_spec(known.name, known.version);
        if (spec.name.empty()) return std::nullopt;
    } else if (target->plugin.empty()) {
        spec = ext_spec(target->ext);
    } else {
        for (const auto& known : plugin_namespaces())
            if (known.owner == target->plugin)
                spec = namespace_spec(known.name, known.version);
        if (spec.name.empty()) return std::nullopt;
    }
    const auto held = snapshot();
    if (!held) return std::nullopt;
    auto found = held->get(spec, scope, ByteView(target->inner_key));
    if (!found) throw_store_error("store index: read a value", found.error());
    if (!*found) return std::nullopt;
    const auto bytes = (*found)->bytes();
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
}

template <typename Fn>
void IndexDatabase::Impl::store_scan(std::string_view prefix, Fn&& fn) {
    if (prefix.size() < 3) return;
    const auto manifest_kind =
        layout::kind_prefix(Ext::Host, layout::host::MANIFEST);
    if (prefix.size() == manifest_kind.size() + 4 &&
        prefix.starts_with(manifest_kind)) {
        const auto file_id =
            layout::read_u32(prefix.substr(manifest_kind.size()));
        std::vector<layout::Ext> exts;
        bool placed = false;
        for (layout::Ext ext : ALL_EXTENSIONS) {
            if (!placed &&
                static_cast<std::uint16_t>(ext) >
                    static_cast<std::uint16_t>(layout::Ext::Plugin)) {
                exts.push_back(layout::Ext::Plugin);
                placed = true;
            }
            exts.push_back(ext);
        }
        if (!placed) exts.push_back(layout::Ext::Plugin);
        for (layout::Ext ext : exts) {
            if (ext == layout::Ext::Plugin) {
                store_manifests(file_id, {}, fn);
                continue;
            }
            auto entry = store_manifest(static_cast<int>(file_id), ext, {});
            if (!entry) continue;
            if (!fn(layout::manifest_key(file_id, ext), *entry)) return;
        }
        return;
    }
    if (auto host = store_layout::host_target_of(prefix)) {
        switch (host->kind) {
            case store_layout::HostKind::PluginManifest:
                store_manifests(host->file_id, host->plugin, fn);
                return;
            case store_layout::HostKind::FileByPath: {
                const kv::NamespaceSpec spec = ext_spec(layout::Ext::Members);
                const auto held = snapshot();
                if (!held) return;
                const auto listed = held->scopes(spec);
                if (!listed)
                    throw_store_error("store index: list scopes",
                                      listed.error());
                for (const kv::ScopeEntry& entry : *listed) {
                    if (entry.scope.kind != kv::ScopeKind::File ||
                        entry.scope.paths.empty())
                        continue;
                    const std::string_view walked =
                        view_of(entry.scope.paths[0]);
                    if (!walked.starts_with(host->path)) continue;
                    if (entry.copy.state != kv::ScopeState::Ready &&
                        entry.copy.state != kv::ScopeState::Failed)
                        continue;
                    const auto record = store_file_record(walked);
                    if (!record) continue;
                    if (!fn(layout::file_by_path_key(walked),
                            layout::encode_file_record(*record)))
                        return;
                }
                return;
            }
            default:
                return;
        }
    }
    const auto ext =
        static_cast<layout::Ext>(layout::read_u16(prefix.substr(0, 2)));
    if (ext == layout::Ext::Agg &&
        static_cast<std::uint8_t>(prefix[2]) == layout::agg_kind::META) {
        const std::string_view tail =
            prefix.size() > layout::KEY_PREFIX_BYTES
                ? std::string_view(prefix).substr(layout::KEY_PREFIX_BYTES)
                : std::string_view();
        for (const auto& known : meta_namespaces()) {
            const std::string_view record(known.owner);
            if (!record.starts_with(tail)) continue;
            std::string inner(1, static_cast<char>(layout::agg_kind::META));
            inner.append(record.data() + tail.size(),
                         record.size() - tail.size());
            const kv::NamespaceSpec spec =
                namespace_spec(known.name, known.version);
            const auto held = snapshot();
            if (!held) return;
            auto cursor = held->scan(spec, root_scope(), ByteView(inner));
            for (; !cursor.at_end(); cursor.next()) {
                const auto inner_key = cursor.key();
                auto value = cursor.value();
                if (!value)
                    throw_store_error("store index: read a value",
                                      value.error());
                std::string key = layout::file_prefix(
                    ext, layout::agg_kind::META, layout::INDEX_WIDE);
                key.append(reinterpret_cast<const char*>(inner_key.data()) + 1,
                           inner_key.size() - 1);
                if (!fn(std::string_view(key),
                        std::string_view(
                            reinterpret_cast<const char*>(value->data()),
                            value->size())))
                    return;
            }
            if (cursor.error())
                throw_store_error("store index: scan index data",
                                  *cursor.error());
        }
        return;
    }
    std::string probe(prefix.substr(0, 3));
    probe.append(layout::KEY_PREFIX_BYTES - 3, '\0');
    if (!store_layout::target_of(probe)) return;
    std::string plugin_prefix;
    if (ext == layout::Ext::Plugin &&
        prefix.size() > layout::KEY_PREFIX_BYTES) {
        const std::string_view tail = prefix.substr(layout::KEY_PREFIX_BYTES);
        const auto end = tail.find('\0');
        plugin_prefix = std::string(
            tail.substr(0, end == std::string_view::npos ? tail.size() : end));
    }
    std::vector<StoreNamespace> namespaces;
    if (ext == layout::Ext::Plugin) {
        for (auto& known : plugin_namespaces())
            if (std::string_view(known.owner).starts_with(plugin_prefix))
                namespaces.push_back(std::move(known));
    } else {
        namespaces.push_back(
            StoreNamespace{store_layout::namespace_name(ext),
                           store_layout::namespace_version(ext),
                           {}});
    }
    std::string inner_prefix(1, static_cast<char>(prefix[2]));
    if (prefix.size() > layout::KEY_PREFIX_BYTES)
        inner_prefix.append(prefix.substr(layout::KEY_PREFIX_BYTES));
    auto scan_copy = [&](const StoreNamespace& ns, const kv::ScopeKey& scope,
                         std::uint32_t file_id) {
        const auto spec = namespace_spec(ns.name, ns.version);
        // A commit on another thread may replace the root's snapshot.
        const auto held = snapshot();
        if (!held) return false;
        auto cursor = held->scan(spec, scope, ByteView(inner_prefix));
        for (; !cursor.at_end(); cursor.next()) {
            const auto inner = cursor.key();
            auto value = cursor.value();
            if (!value)
                throw_store_error("store index: read a value", value.error());
            std::string key = layout::file_prefix(
                ext, static_cast<std::uint8_t>(inner.data()[0]), file_id);
            key.append(reinterpret_cast<const char*>(inner.data()) + 1,
                       inner.size() - 1);
            if (!std::string_view(key).starts_with(prefix)) continue;
            if (!fn(std::string_view(key),
                    std::string_view(
                        reinterpret_cast<const char*>(value->data()),
                        value->size())))
                return false;
        }
        if (cursor.error())
            throw_store_error("store index: scan index data", *cursor.error());
        return true;
    };
    if (prefix.size() >= layout::KEY_PREFIX_BYTES) {
        const std::uint32_t file_id = layout::read_u32(prefix.substr(3, 4));
        if (file_id == layout::INDEX_WIDE) {
            for (const auto& ns : namespaces)
                if (!scan_copy(ns, root_scope(), file_id)) return;
            return;
        }
        const auto path = registered_path(file_id);
        if (!path) return;
        const auto scope = file_scope(*path);
        if (!scope) return;
        for (const auto& ns : namespaces)
            if (!scan_copy(ns, *scope, file_id)) return;
        return;
    }
    std::vector<std::uint32_t> ids;
    for (const auto& ns : namespaces) {
        const auto spec = namespace_spec(ns.name, ns.version);
        for (std::uint32_t id : file_ids_of(spec)) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (std::uint32_t id : ids) {
        const auto path = registered_path(id);
        if (!path) continue;
        const auto scope = file_scope(*path);
        if (!scope) continue;
        for (const auto& ns : namespaces)
            if (!scan_copy(ns, *scope, id)) return;
    }
    for (const auto& ns : namespaces)
        if (!scan_copy(ns, root_scope(), layout::INDEX_WIDE)) return;
}

namespace {
std::atomic<std::uint64_t> read_write_open_count{0};

/// A StoreRoot lives only while something holds it, and building one numbers
/// every file of its store, so retaining the last read-only opens spares a
/// reader that opens the index once per file.
constexpr std::size_t STORE_ROOTS_RETAINED = 64;

class RetainedStoreRoots {
   public:
    static RetainedStoreRoots& instance() {
        static RetainedStoreRoots* roots = [] {
            auto* made = new RetainedStoreRoots();
            // Each data file needs its footer and summary entry.
            std::atexit([] { RetainedStoreRoots::instance().clear(); });
            return made;
        }();
        return *roots;
    }

    void retain(const std::string& path, std::shared_ptr<StoreRoot> root) {
        memory::Scope general(memory::Manager::get().general());
        std::lock_guard<std::mutex> lock(mutex_);
        const auto known =
            std::find_if(roots_.begin(), roots_.end(),
                         [&](const auto& held) { return held.first == path; });
        if (known != roots_.end()) {
            roots_.splice(roots_.begin(), roots_, known);
            roots_.front().second = std::move(root);
            return;
        }
        roots_.emplace_front(path, std::move(root));
        if (roots_.size() > STORE_ROOTS_RETAINED) roots_.pop_back();
    }

    void clear() {
        std::list<std::pair<std::string, std::shared_ptr<StoreRoot>>> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dropped.swap(roots_);
        }
    }

    void forget(const std::string& path) {
        std::list<std::pair<std::string, std::shared_ptr<StoreRoot>>> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto known = std::find_if(
                roots_.begin(), roots_.end(),
                [&](const auto& held) { return held.first == path; });
            if (known != roots_.end())
                dropped.splice(dropped.begin(), roots_, known);
        }
    }

   private:
    std::mutex mutex_;
    std::list<std::pair<std::string, std::shared_ptr<StoreRoot>>> roots_;
};
}  // namespace

void close_index_writers() {
    if (const auto error = StoreRoot::close_all()) {
        DFTRACER_UTILS_LOG_ERROR("store index: close at exit: %s",
                                 error->c_str());
    }
}

void release_index(const std::string& index_path) {
    RetainedStoreRoots::instance().forget(
        index::store::internal::normalize_index_root(index_path));
}

std::uint64_t IndexDatabase::read_write_opens() noexcept {
    return read_write_open_count.load(std::memory_order_relaxed);
}

IndexDatabase::IndexDatabase(const std::string& index_path,
                             IndexOpenMode open_mode)
    : impl_(std::make_unique<Impl>()) {
    impl_->db_path_ = index::store::internal::normalize_index_root(index_path);
    impl_->open_mode_ = open_mode;
    impl_->store_ = StoreRoot::get_or_open(impl_->db_path_);
    if (open_mode == IndexOpenMode::ReadOnly) {
        RetainedStoreRoots::instance().retain(impl_->db_path_, impl_->store_);
        return;
    }
    // Only a write creates the store inside the root.
    std::error_code error;
    fs::create_directories(impl_->db_path_, error);
    if (error) {
        throw std::runtime_error("store index: cannot create " +
                                 impl_->db_path_ + ": " + error.message());
    }
    read_write_open_count.fetch_add(1, std::memory_order_relaxed);
}

IndexDatabase::IndexDatabase(IndexDatabase&&) noexcept = default;
IndexDatabase& IndexDatabase::operator=(IndexDatabase&&) noexcept = default;
IndexDatabase::~IndexDatabase() = default;

void IndexDatabase::for_each_record(
    const std::function<void(std::string_view, std::string_view)>& fn) const {
    const auto held = impl_->store_->snapshot();
    if (!held) return;
    const auto declared = held->namespaces();
    if (!declared)
        throw_store_error("store index: list the namespaces", declared.error());
    for (const kv::NamespaceDecl& decl : *declared) {
        kv::NamespaceSpec spec;
        spec.name = decl.name;
        spec.format_version = decl.format_version;
        const auto listed = held->scopes(spec);
        if (!listed)
            throw_store_error("store index: list the scopes", listed.error());
        const std::string name(decl.name.data(), decl.name.size());
        for (const kv::ScopeEntry& entry : *listed) {
            std::string scope =
                std::to_string(decl.format_version) + "|" +
                std::to_string(static_cast<int>(entry.scope.kind));
            for (const dstd::String& path : entry.scope.paths) {
                scope += "|";
                scope.append(path.data(), path.size());
            }
            const std::string copy =
                std::to_string(static_cast<int>(entry.copy.state)) + "|" +
                std::to_string(entry.copy.params_hash);
            fn("#copy|" + name + "|" + scope, copy);
            auto cursor = held->scan(spec, entry.scope, ByteView{}, ByteView{});
            for (; !cursor.at_end(); cursor.next()) {
                auto value = cursor.value();
                if (!value)
                    throw_store_error("store index: read a value",
                                      value.error());
                const ByteView key = cursor.key();
                fn(name + "|" + scope + "|" +
                       std::string(reinterpret_cast<const char*>(key.data()),
                                   key.size()),
                   std::string_view(
                       reinterpret_cast<const char*>(value->data()),
                       value->size()));
            }
            if (cursor.error())
                throw_store_error("store index: scan a copy", *cursor.error());
        }
    }
}

std::unique_ptr<IndexDatabaseWrite> IndexDatabase::begin_staged_write() {
    return impl_->store_->begin_write(true);
}

std::unique_ptr<IndexDatabaseWrite> IndexDatabase::begin_write() {
    return impl_->store_->begin_write();
}

std::vector<int> IndexDatabase::assign_file_ids(
    const std::vector<std::string>& file_paths) {
    std::vector<int> ids(file_paths.size(), -1);
    std::size_t fresh = 0;
    for (std::size_t i = 0; i < file_paths.size(); ++i) {
        ids[i] = get_file_info_id(get_logical_path(file_paths[i]));
        if (ids[i] < 0) ++fresh;
    }
    if (fresh == 0) return ids;
    int next = reserve_file_id_range(fresh);
    for (auto& id : ids)
        if (id < 0) id = next++;
    return ids;
}

int IndexDatabase::reserve_file_id_range(std::size_t count) {
    return count == 0 ? impl_->store_->next_file_id()
                      : impl_->store_->reserve_file_id_range(count);
}

void IndexDatabase::close_writer() { impl_->store_->close(); }

void IndexDatabase::refresh() { impl_->store_->refresh(); }

std::optional<ExtensionState> IndexDatabase::extension_state(
    int file_id, IndexExtension ext) const {
    auto value = impl_->raw_get(layout::manifest_key(fid(file_id), ext));
    if (!value) return std::nullopt;
    auto entry = layout::decode_manifest(*value);
    if (!entry) return std::nullopt;
    return ExtensionState{entry->version, entry->params_hash,
                          entry->status == layout::ExtStatus::Ready};
}

std::optional<ExtensionState> IndexDatabase::plugin_extension_state(
    int file_id, std::string_view name) const {
    auto value =
        impl_->raw_get(layout::plugin_manifest_key(fid(file_id), name));
    if (!value) return std::nullopt;
    auto entry = layout::decode_manifest(*value);
    if (!entry) return std::nullopt;
    return ExtensionState{entry->version, entry->params_hash,
                          entry->status == layout::ExtStatus::Ready};
}

std::vector<std::pair<std::string, ExtensionState>>
IndexDatabase::plugin_extension_states(int file_id) const {
    std::vector<std::pair<std::string, ExtensionState>> out;
    const auto prefix = layout::manifest_key(fid(file_id), Ext::Plugin);
    impl_->raw_scan(prefix, [&](std::string_view key, std::string_view value) {
        if (key.size() <= prefix.size() || key.back() != '\0') return true;
        auto entry = layout::decode_manifest(value);
        if (!entry) return true;
        out.emplace_back(
            std::string(
                key.substr(prefix.size(), key.size() - prefix.size() - 1)),
            ExtensionState{entry->version, entry->params_hash,
                           entry->status == layout::ExtStatus::Ready});
        return true;
    });
    return out;
}

bool IndexDatabase::extension_current(int file_id, IndexExtension ext) const {
    auto state = extension_state(file_id, ext);
    return state && state->ready && state->version == layout::ext_version(ext);
}

bool IndexDatabase::pruning_tier_current(int file_id) const {
    return extension_current(file_id, Ext::Stats);
}

std::vector<std::string> IndexDatabase::extension_paths(
    int file_id, IndexExtension ext) const {
    std::vector<std::string> paths;
    const auto prefix = layout::file_prefix(ext, pk::PATHS, fid(file_id));
    impl_->scan_keys(prefix, [&](std::string_view key) {
        paths.emplace_back(key.substr(prefix.size()));
    });
    return paths;
}

std::vector<std::pair<std::uint64_t, std::string>> IndexDatabase::path_granules(
    int file_id, IndexExtension ext, std::string_view path) const {
    std::vector<std::pair<std::uint64_t, std::string>> out;
    const auto prefix = layout::path_prefix(ext, pk::DATA, fid(file_id), path);
    impl_->scan(ext, pk::DATA, prefix,
                [&](std::string_view key, std::string_view body) {
                    if (key.size() == prefix.size() + 4)
                        out.emplace_back(tail_u32(key, prefix.size()),
                                         std::string(body));
                });
    return out;
}

std::optional<std::string> IndexDatabase::path_file_value(
    int file_id, IndexExtension ext, std::string_view path) const {
    auto key = layout::file_prefix(ext, pk::FILE, fid(file_id));
    key.append(path);
    return impl_->get(ext, pk::FILE, key);
}

bool IndexDatabase::has_postings(int file_id, std::string_view path) const {
    const auto prefix =
        layout::path_prefix(Ext::Postings, pk::IN_FILE, fid(file_id), path);
    return impl_->any_with_prefix(prefix);
}

bool IndexDatabase::has_posting(int file_id, std::string_view path,
                                std::uint64_t value_hash) const {
    auto key =
        layout::path_prefix(Ext::Postings, pk::IN_FILE, fid(file_id), path);
    append_u64_be(key, value_hash);
    return impl_->raw_get(key).has_value();
}

std::vector<std::uint64_t> IndexDatabase::posting_granules(
    int file_id, std::string_view path, std::uint64_t value_hash) const {
    std::vector<std::uint64_t> out;
    auto prefix =
        layout::path_prefix(Ext::Postings, pk::IN_GRANULE, fid(file_id), path);
    append_u64_be(prefix, value_hash);
    impl_->scan_keys(prefix, [&](std::string_view key) {
        if (key.size() == prefix.size() + 4)
            out.push_back(tail_u32(key, prefix.size()));
    });
    return out;
}

int IndexDatabase::get_file_info_id(std::string_view path) const {
    auto record = impl_->file_record(path);
    return record ? static_cast<int>(record->file_id) : -1;
}

std::optional<std::uint64_t> IndexDatabase::get_file_hash(
    std::string_view path) const {
    auto record = impl_->file_record(path);
    if (!record) return std::nullopt;
    return record->hash;
}

std::optional<IndexDatabase::FileStat> IndexDatabase::get_file_stat(
    std::string_view path) const {
    auto record = impl_->file_record(path);
    if (!record) return std::nullopt;
    return FileStat{record->mtime, record->size};
}

std::optional<std::uint32_t> IndexDatabase::get_format_version() const {
    auto body =
        impl_->get(Ext::Host, layout::host::FORMAT, layout::format_key());
    if (!body) return std::nullopt;
    return layout::read_u32(*body);
}

bool IndexDatabase::format_outdated() const {
    const auto stored = get_format_version();
    return !stored || *stored != FORMAT_VERSION;
}

IndexDatabase::Freshness IndexDatabase::check_freshness(
    const std::string& file_path) const {
    dftracer::utils::InputFileStat st;
    if (!dftracer::utils::stat_input(file_path, st))
        return check_freshness(file_path, 0, 0);
    return check_freshness(file_path, st.size, st.mtime);
}

IndexDatabase::Freshness IndexDatabase::check_freshness(
    const std::string& file_path, std::uint64_t size,
    std::int64_t mtime) const {
    // Stat-only by design: mtime + size are the only O(1) freshness signals,
    // and this runs on every index open and every stale scan, so it must not
    // read the file body. A same-size edit with a deliberately restored mtime
    // is the one change this cannot see; that is accepted rather than pay a
    // whole-file read on the hot path.
    if (format_outdated()) return Freshness::FormatOutdated;

    const auto logical = index::store::internal::get_logical_path(file_path);
    if (get_file_info_id(logical) < 0) return Freshness::Stale;

    auto stored = get_file_stat(logical);
    if (!stored) return Freshness::Stale;  // record predates stat tracking

    if (stored->mtime != static_cast<std::uint64_t>(mtime) ||
        stored->size != size) {
        return Freshness::Stale;
    }
    return Freshness::Fresh;
}

IndexDatabase::StaleCheckResult IndexDatabase::find_stale_files(
    const std::vector<std::string>& current_paths) const {
    StaleCheckResult result;
    result.format_outdated = format_outdated();

    StringViewSet seen_logical;
    seen_logical.reserve(current_paths.size());
    for (const auto& path : current_paths) {
        const auto logical = index::store::internal::get_logical_path(path);
        seen_logical.insert(logical);
        if (get_file_info_id(logical) < 0) {
            result.added.push_back(path);
            continue;
        }
        if (check_freshness(path) != Freshness::Fresh) {
            result.changed.push_back(path);
        }
    }

    for (const auto& [logical, _] : query_all_file_info_ids()) {
        if (seen_logical.find(logical) == seen_logical.end()) {
            result.removed.push_back(logical);
        }
    }
    return result;
}

StringViewMap<int> IndexDatabase::query_all_file_info_ids() const {
    StringViewMap<int> results;
    for (const auto& [path, entry] : query_all_file_registry())
        results.emplace(path, entry.file_id);
    return results;
}

StringViewMap<FileRegistryEntry> IndexDatabase::query_all_file_registry()
    const {
    StringViewMap<FileRegistryEntry> results;
    const auto prefix =
        layout::kind_prefix(Ext::Host, layout::host::FILE_BY_PATH);
    impl_->raw_scan(prefix, [&](std::string_view key, std::string_view value) {
        if (auto record = layout::decode_file_record(value))
            results.emplace(
                key.substr(prefix.size()),
                FileRegistryEntry{static_cast<int>(record->file_id)});
        return true;
    });
    return results;
}

int IndexDatabase::find_file(std::string_view file_path) const {
    return get_file_info_id(get_logical_path(file_path));
}

namespace {

// The schema type of a catalog path from the base types seen for it; nullopt
// when only nulls were seen.
std::optional<ColumnType> schema_type(std::uint8_t seen) {
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
    return std::nullopt;
}

}  // namespace

std::vector<std::string> IndexDatabase::query_all_columns() const {
    std::vector<std::string> out;
    for (auto& [name, _] : query_all_column_types())
        out.push_back(std::move(name));
    return out;
}

std::vector<std::pair<std::string, ColumnType>>
IndexDatabase::query_all_column_types() const {
    constexpr std::string_view ARGS = "args.";
    ankerl::unordered_dense::map<std::string, ColumnType> cols;
    impl_->scan(Ext::Catalog, layout::catalog_kind::PATH,
                layout::kind_prefix(Ext::Catalog, layout::catalog_kind::PATH),
                [&](std::string_view key, std::string_view body) {
                    if (key.size() <= layout::KEY_PREFIX_BYTES) return;
                    auto stat = layout::decode_path_stat(body);
                    if (!stat) return;
                    auto type = schema_type(stat->seen);
                    if (!type) return;
                    auto path = key.substr(layout::KEY_PREFIX_BYTES);
                    if (path.starts_with(ARGS)) path.remove_prefix(ARGS.size());
                    auto [pos, inserted] =
                        cols.emplace(std::string(path), *type);
                    if (!inserted)
                        pos->second = merge_column_type(pos->second, *type);
                });
    std::vector<std::pair<std::string, ColumnType>> out(cols.begin(),
                                                        cols.end());
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

std::vector<std::pair<std::string, ColumnType>>
IndexDatabase::file_column_types(int file_id) const {
    std::vector<std::pair<std::string, ColumnType>> out;
    for (auto& [path, stat] : catalog(file_id))
        if (auto type = schema_type(stat.seen))
            out.emplace_back(std::move(path), *type);
    return out;
}

std::optional<std::string> IndexDatabase::file_schema(int file_id) const {
    if (!extension_current(file_id, Ext::Profile)) return std::nullopt;
    auto body =
        impl_->get(Ext::Profile, layout::profile_kind::ID,
                   layout::file_prefix(Ext::Profile, layout::profile_kind::ID,
                                       fid(file_id)));
    if (!body) return std::nullopt;
    return std::string(*body);
}

std::vector<std::pair<std::string, PathStat>> IndexDatabase::catalog(
    int file_id) const {
    std::vector<std::pair<std::string, PathStat>> out;
    const auto prefix = layout::file_prefix(
        Ext::Catalog, layout::catalog_kind::PATH, fid(file_id));
    impl_->scan(Ext::Catalog, layout::catalog_kind::PATH, prefix,
                [&](std::string_view key, std::string_view body) {
                    if (auto stat = layout::decode_path_stat(body))
                        out.emplace_back(std::string(key.substr(prefix.size())),
                                         *stat);
                });
    return out;
}

std::vector<index::schemas::dft::ChunkStatisticsResult>
IndexDatabase::query_chunk_statistics(int file_id) const {
    std::vector<index::schemas::dft::ChunkStatisticsResult> results;
    impl_->scan(Ext::Stats, sk::CHUNK_STATS,
                layout::file_prefix(Ext::Stats, sk::CHUNK_STATS, fid(file_id)),
                [&](std::string_view key, std::string_view body) {
                    results.push_back({tail_u32(key, layout::KEY_PREFIX_BYTES),
                                       decode_chunk_statistics_value(body)});
                });
    return results;
}

std::optional<ankerl::unordered_dense::map<std::uint64_t, ChunkMetadata>>
IndexDatabase::chunk_metadata(int file_id) const {
    if (!extension_current(file_id, Ext::Metadata)) return std::nullopt;
    ankerl::unordered_dense::map<std::uint64_t, ChunkMetadata> out;
    impl_->scan(Ext::Metadata, layout::metadata_kind::CHUNK,
                layout::file_prefix(Ext::Metadata, layout::metadata_kind::CHUNK,
                                    fid(file_id)),
                [&](std::string_view key, std::string_view body) {
                    if (auto m = layout::decode_chunk_metadata(body))
                        out.emplace(tail_u32(key, layout::KEY_PREFIX_BYTES),
                                    std::move(*m));
                });
    return out;
}

ankerl::unordered_dense::map<
    int, std::vector<index::schemas::dft::ChunkStatisticsResult>>
IndexDatabase::query_chunk_statistics_batch(
    const std::vector<int>& file_ids) const {
    ankerl::unordered_dense::map<
        int, std::vector<index::schemas::dft::ChunkStatisticsResult>>
        results;
    impl_->scan_files(
        Ext::Stats, sk::CHUNK_STATS, file_ids,
        [&](int file_id, std::string_view key, std::string_view body) {
            results[file_id].push_back({tail_u32(key, layout::KEY_PREFIX_BYTES),
                                        decode_chunk_statistics_value(body)});
        });
    return results;
}

ankerl::unordered_dense::map<int, index::schemas::dft::MergedStatisticsResult>
IndexDatabase::query_file_scalar_stats_batch(
    const std::vector<int>& file_ids) const {
    ankerl::unordered_dense::map<int,
                                 index::schemas::dft::MergedStatisticsResult>
        results;
    impl_->scan_files(
        Ext::Stats, sk::FILE_SCALAR_STATS, file_ids,
        [&](int file_id, std::string_view, std::string_view body) {
            results.emplace(file_id, decode_file_scalar_stats_value(body));
        });
    return results;
}

ankerl::unordered_dense::map<int, FileMetadataResult>
IndexDatabase::query_file_metadata_batch(
    const std::vector<int>& file_ids) const {
    ankerl::unordered_dense::map<int, FileMetadataResult> results;
    impl_->scan_files(
        Ext::Members, layout::members::METADATA, file_ids,
        [&](int file_id, std::string_view, std::string_view body) {
            Cursor cursor(body);
            auto& meta = results[file_id];
            meta.checkpoint_size = cursor.u64();
            meta.num_lines = cursor.u64();
            meta.max_bytes = cursor.u64();
            meta.truncated =
                (cursor.u64() & encoding::METADATA_FLAG_TRUNCATED) != 0;
        });
    return results;
}

ankerl::unordered_dense::map<int, StringViewMap<std::uint64_t>>
IndexDatabase::query_file_pid_tid_counts_batch(
    const std::vector<int>& file_ids) const {
    ankerl::unordered_dense::map<int, StringViewMap<std::uint64_t>> results;
    impl_->scan_files(
        Ext::Stats, sk::FILE_PID_TID_COUNTS, file_ids,
        [&](int file_id, std::string_view, std::string_view body) {
            results.emplace(file_id, decode_count_map_value(body));
        });
    return results;
}

std::vector<index::gzip::GzipMemberRecord> IndexDatabase::query_gzip_members(
    int file_id) const {
    std::vector<index::gzip::GzipMemberRecord> members;
    if (!extension_current(file_id, Ext::Members)) return members;
    impl_->scan(Ext::Members, layout::members::MEMBER,
                layout::file_prefix(Ext::Members, layout::members::MEMBER,
                                    fid(file_id)),
                [&](std::string_view key, std::string_view body) {
                    members.push_back(decode_gzip_member(key, body));
                });
    return members;
}

std::optional<std::string> IndexDatabase::query_restart_window(
    int file_id, std::uint64_t member_idx) const {
    return impl_->get(Ext::Members, layout::members::RESTART,
                      layout::granule_key(
                          Ext::Members, layout::members::RESTART, fid(file_id),
                          static_cast<std::uint32_t>(member_idx)));
}

std::vector<index::gzip::ChunkSpan> IndexDatabase::query_chunk_spans(
    int file_id) const {
    auto members = query_gzip_members(file_id);
    std::vector<index::gzip::ChunkSpan> spans;
    spans.reserve(members.size());
    for (const auto& m : members)
        spans.push_back(
            {m.uc_offset, m.uc_size, m.first_line_num, m.last_line_num});
    return spans;
}

namespace {
// The distinct PIDs a file touched, projected from its "pid:tid" -> count map
// (FILE_PID_TID_COUNTS, written by the bloom/stats pass). This is the same set
// the retired manifest P| keys used to store, now derived so PID collection
// needs no separate index tier.
ankerl::unordered_dense::set<std::uint64_t> pids_from_pid_tid_counts(
    const StringViewMap<std::uint64_t>& counts) {
    ankerl::unordered_dense::set<std::uint64_t> pids;
    for (const auto& [key, _] : counts) {
        std::string_view sv(key);
        const auto colon = sv.find(':');
        const std::string_view pid_sv =
            colon == std::string_view::npos ? sv : sv.substr(0, colon);
        std::uint64_t pid = 0;
        std::from_chars(pid_sv.data(), pid_sv.data() + pid_sv.size(), pid);
        pids.insert(pid);
    }
    return pids;
}
}  // namespace

ankerl::unordered_dense::set<std::uint64_t> IndexDatabase::query_file_pids(
    int file_id) const {
    auto body = impl_->get(
        Ext::Stats, sk::FILE_PID_TID_COUNTS,
        layout::file_prefix(Ext::Stats, sk::FILE_PID_TID_COUNTS, fid(file_id)));
    if (!body) return {};
    return pids_from_pid_tid_counts(decode_count_map_value(*body));
}

ankerl::unordered_dense::map<int, ankerl::unordered_dense::set<std::uint64_t>>
IndexDatabase::query_all_file_pids() const {
    ankerl::unordered_dense::map<int,
                                 ankerl::unordered_dense::set<std::uint64_t>>
        result;
    impl_->scan(Ext::Stats, sk::FILE_PID_TID_COUNTS,
                layout::kind_prefix(Ext::Stats, sk::FILE_PID_TID_COUNTS),
                [&](std::string_view key, std::string_view body) {
                    result[static_cast<int>(tail_u32(key, 3))] =
                        pids_from_pid_tid_counts(decode_count_map_value(body));
                });
    return result;
}

namespace {

// Metadata record field `idx` for `file_id`, 0 when the record is absent.
template <typename Db>
std::uint64_t get_metadata_field(Db& db, int file_id, std::size_t idx) {
    auto body =
        db.get(Ext::Members, layout::members::METADATA,
               layout::file_prefix(Ext::Members, layout::members::METADATA,
                                   fid(file_id)));
    if (!body || body->size() < (idx + 1) * 8) return 0;
    return layout::read_u64(std::string_view(*body).substr(idx * 8));
}

}  // namespace

std::uint64_t IndexDatabase::get_checkpoint_size(int file_id) const {
    return get_metadata_field(*impl_, file_id, 0);
}

std::uint64_t IndexDatabase::get_num_lines(int file_id) const {
    return get_metadata_field(*impl_, file_id, 1);
}

std::uint64_t IndexDatabase::get_max_bytes(int file_id) const {
    return get_metadata_field(*impl_, file_id, 2);
}

std::optional<std::string> IndexDatabase::rowset(int file_id,
                                                 std::string_view name) const {
    if (!extension_current(file_id, Ext::Rowset)) return std::nullopt;
    auto body =
        impl_->get(Ext::Rowset, layout::rowset_kind::FRAME,
                   layout::path_prefix(Ext::Rowset, layout::rowset_kind::FRAME,
                                       fid(file_id), name));
    if (!body) return std::nullopt;
    return std::string(*body);
}

}  // namespace dftracer::utils::index::store

namespace dftracer::utils::index::store {

void AggRowReader::for_each_row(const IndexDatabase& db, std::uint8_t kind,
                                std::string_view prefix, const RowFn& fn) {
    db.impl_->raw_scan(
        prefix, [&](std::string_view key, std::string_view value) {
            if (auto body = layout::payload(value, layout::Ext::Agg, kind))
                return fn(key, *body);
            return true;
        });
}

std::optional<std::string> AggRowReader::get_row(const IndexDatabase& db,
                                                 std::uint8_t kind,
                                                 std::string_view key) {
    return db.impl_->get(layout::Ext::Agg, kind, key);
}

std::vector<std::uint32_t> AggRowReader::file_ids(const IndexDatabase& db) {
    return db.impl_->file_ids_of(ext_spec(layout::Ext::Agg));
}

}  // namespace dftracer::utils::index::store
