#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/index/store/layout.h>

#include <algorithm>
#include <array>

namespace dftracer::utils::index::store::layout {

namespace {

constexpr std::array<KindSpec, 23> KINDS = {{
    {Ext::MEMBERS, members::MEMBER, Family::MEMBERS},
    {Ext::MEMBERS, members::METADATA, Family::REGISTRY},
    {Ext::MEMBERS, members::RESTART, Family::BLOB},
    {Ext::ZONEMAP, path_kind::PATHS, Family::REGISTRY},
    {Ext::ZONEMAP, path_kind::DATA, Family::GRANULE},
    {Ext::BLOOM, path_kind::PATHS, Family::REGISTRY},
    {Ext::BLOOM, path_kind::DATA, Family::BLOB},
    {Ext::BLOOM, path_kind::FILE, Family::BLOB},
    {Ext::COUNTS, path_kind::PATHS, Family::REGISTRY},
    {Ext::COUNTS, path_kind::DATA, Family::GRANULE},
    {Ext::POSTINGS, path_kind::PATHS, Family::REGISTRY},
    {Ext::POSTINGS, path_kind::IN_FILE, Family::POSTINGS},
    {Ext::POSTINGS, path_kind::IN_GRANULE, Family::POSTINGS},
    {Ext::STATS, stats_kind::CHUNK_STATS, Family::BLOB},
    {Ext::STATS, stats_kind::FILE_SCALAR_STATS, Family::BLOB},
    {Ext::STATS, stats_kind::FILE_PID_TID_COUNTS, Family::BLOB},
    {Ext::CATALOG, catalog_kind::PATH, Family::REGISTRY},
    {Ext::PROFILE, profile_kind::ID, Family::REGISTRY},
    {Ext::METADATA, metadata_kind::CHUNK, Family::GRANULE},
    {Ext::PLUGIN, path_kind::PATHS, Family::REGISTRY},
    {Ext::PLUGIN, path_kind::DATA, Family::BLOB},
    {Ext::PLUGIN, path_kind::FILE, Family::BLOB},
    {Ext::ROWSET, rowset_kind::FRAME, Family::BLOB},
}};

constexpr std::uint8_t PAYLOAD_VERSION = 1;
constexpr std::uint8_t CODEC_RAW = 0;

std::uint16_t type_id(Ext ext, std::uint8_t kind) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(ext) << 8U) |
                                      kind);
}

}  // namespace

std::string_view family_name(Family family) {
    switch (family) {
        case Family::REGISTRY:
            return "default";
        case Family::MEMBERS:
            return "members";
        case Family::GRANULE:
            return "granule";
        case Family::BLOB:
            return "blob";
        case Family::POSTINGS:
            return "postings";
        case Family::AGGREGATION:
            return "aggregation";
        case Family::SYSTEM_METRICS:
            return "system_metrics";
    }
    return "default";
}

std::string_view ext_name(Ext ext) {
    switch (ext) {
        case Ext::HOST:
            return "host";
        case Ext::MEMBERS:
            return "core.members";
        case Ext::ZONEMAP:
            return "zonemap";
        case Ext::BLOOM:
            return "bloom";
        case Ext::COUNTS:
            return "counts";
        case Ext::POSTINGS:
            return "postings";
        case Ext::STATS:
            return "dft.stats";
        case Ext::CATALOG:
            return "core.catalog";
        case Ext::PROFILE:
            return "core.profile";
        case Ext::METADATA:
            return "dft.metadata";
        case Ext::PLUGIN:
            return "plugin";
        case Ext::AGG:
            return "dftracer.agg";
        case Ext::ROWSET:
            return "core.rowset";
    }
    return "";
}

}  // namespace dftracer::utils::index::store::layout

namespace dftracer::utils::index::store {

std::string_view extension_name(IndexExtension ext) {
    return layout::ext_name(ext);
}

std::optional<IndexExtension> parse_extension(std::string_view name) {
    for (auto ext : ALL_EXTENSIONS)
        if (layout::ext_name(ext) == name) return ext;
    return std::nullopt;
}

}  // namespace dftracer::utils::index::store

namespace dftracer::utils::index::store::layout {

std::span<const KindSpec> kinds() { return KINDS; }

Family family_of(Ext ext, std::uint8_t kind) {
    for (const auto& k : KINDS)
        if (k.ext == ext && k.kind == kind) return k.family;
    return Family::REGISTRY;
}

// Every extension is at version 1 until the first release: stored formats
// change freely before then, and a development index is deleted, not migrated.
std::uint32_t ext_version(Ext /*ext*/) { return 1; }

void append_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value >> 8U));
    out.push_back(static_cast<char>(value & 0xFFU));
}

void append_u32(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
}

void append_u64(std::string& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
}

namespace {
template <typename T>
T read_be(std::string_view bytes) {
    if (bytes.size() < sizeof(T))
        throw DFTUtilsException(ErrorCode::PARSE,
                                "index layout: truncated integer");
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value = static_cast<T>((value << 8U) |
                               static_cast<unsigned char>(bytes[i]));
    return value;
}
}  // namespace

std::uint16_t read_u16(std::string_view bytes) {
    return read_be<std::uint16_t>(bytes);
}
std::uint32_t read_u32(std::string_view bytes) {
    return read_be<std::uint32_t>(bytes);
}
std::uint64_t read_u64(std::string_view bytes) {
    return read_be<std::uint64_t>(bytes);
}

std::string kind_prefix(Ext ext, std::uint8_t kind) {
    std::string key;
    key.reserve(KEY_PREFIX_BYTES + 16);
    append_u16(key, static_cast<std::uint16_t>(ext));
    key.push_back(static_cast<char>(kind));
    return key;
}

std::string file_prefix(Ext ext, std::uint8_t kind, std::uint32_t file_id) {
    auto key = kind_prefix(ext, kind);
    append_u32(key, file_id);
    return key;
}

std::string granule_key(Ext ext, std::uint8_t kind, std::uint32_t file_id,
                        std::uint32_t granule) {
    auto key = file_prefix(ext, kind, file_id);
    append_u32(key, granule);
    return key;
}

std::string path_prefix(Ext ext, std::uint8_t kind, std::uint32_t file_id,
                        std::string_view path) {
    auto key = file_prefix(ext, kind, file_id);
    key.append(path);
    key.push_back('\0');
    return key;
}

std::string prefix_end(std::string_view prefix) {
    std::string end(prefix);
    while (!end.empty()) {
        auto& last = end.back();
        if (static_cast<unsigned char>(last) != 0xFFU) {
            last = static_cast<char>(static_cast<unsigned char>(last) + 1);
            return end;
        }
        end.pop_back();
    }
    return std::string(prefix) + std::string(8, '\xFF');
}

std::string with_header(Ext ext, std::uint8_t kind, std::string_view payload) {
    std::string value;
    value.reserve(VALUE_HEADER_BYTES + payload.size());
    append_u16(value, type_id(ext, kind));
    value.push_back(static_cast<char>(PAYLOAD_VERSION));
    value.push_back(static_cast<char>(CODEC_RAW));
    value.push_back(0);
    append_u32(value, static_cast<std::uint32_t>(payload.size()));
    value.append(payload);
    return value;
}

std::optional<std::string_view> payload(std::string_view value, Ext ext,
                                        std::uint8_t kind) {
    if (value.size() < VALUE_HEADER_BYTES) return std::nullopt;
    if (read_u16(value) != type_id(ext, kind) ||
        static_cast<std::uint8_t>(value[2]) != PAYLOAD_VERSION ||
        static_cast<std::uint8_t>(value[3]) != CODEC_RAW)
        return std::nullopt;
    const auto len = read_u32(value.substr(5));
    if (len != value.size() - VALUE_HEADER_BYTES) return std::nullopt;
    return value.substr(VALUE_HEADER_BYTES);
}

std::string manifest_prefix(std::uint32_t file_id) {
    return file_prefix(Ext::HOST, host::MANIFEST, file_id);
}

std::string manifest_key(std::uint32_t file_id, Ext ext) {
    auto key = manifest_prefix(file_id);
    append_u16(key, static_cast<std::uint16_t>(ext));
    return key;
}

std::string plugin_manifest_key(std::uint32_t file_id, std::string_view name) {
    auto key = manifest_key(file_id, Ext::PLUGIN);
    key.append(name);
    key.push_back('\0');
    return key;
}

std::string encode_path_stat(const PathStat& stat) {
    std::string out;
    out.push_back(static_cast<char>(stat.type));
    out.push_back(static_cast<char>(stat.seen));
    append_u64(out, stat.count);
    return out;
}

std::optional<PathStat> decode_path_stat(std::string_view body) {
    if (body.size() != 10) return std::nullopt;
    const auto type = static_cast<std::uint8_t>(body[0]);
    if (type > static_cast<std::uint8_t>(PathType::ARRAY)) return std::nullopt;
    return PathStat{static_cast<PathType>(type),
                    static_cast<std::uint8_t>(body[1]),
                    read_u64(body.substr(2))};
}

std::string encode_manifest(const ManifestEntry& entry) {
    std::string body;
    append_u32(body, entry.version);
    append_u64(body, entry.params_hash);
    body.push_back(static_cast<char>(entry.status));
    return with_header(Ext::HOST, host::MANIFEST, body);
}

std::optional<ManifestEntry> decode_manifest(std::string_view value) {
    auto body = payload(value, Ext::HOST, host::MANIFEST);
    if (!body || body->size() != 13) return std::nullopt;
    ManifestEntry entry;
    entry.version = read_u32(*body);
    entry.params_hash = read_u64(body->substr(4));
    entry.status =
        static_cast<ExtStatus>(static_cast<std::uint8_t>((*body)[12]));
    if (entry.status != ExtStatus::READY && entry.status != ExtStatus::FAILED)
        return std::nullopt;
    return entry;
}

std::string format_key() { return kind_prefix(Ext::HOST, host::FORMAT); }

std::string next_file_id_key() {
    return kind_prefix(Ext::HOST, host::NEXT_FILE_ID);
}

std::string ext_registry_key(Ext ext) {
    auto key = kind_prefix(Ext::HOST, host::EXT);
    key.append(ext_name(ext));
    return key;
}

std::string file_by_path_key(std::string_view logical_path) {
    auto key = kind_prefix(Ext::HOST, host::FILE_BY_PATH);
    key.append(logical_path);
    return key;
}

std::string file_by_id_key(std::uint32_t file_id) {
    return file_prefix(Ext::HOST, host::FILE_BY_ID, file_id);
}

std::string encode_file_record(const FileRecord& record) {
    std::string body;
    append_u32(body, record.file_id);
    append_u64(body, record.mtime);
    append_u64(body, record.hash);
    append_u64(body, record.size);
    return with_header(Ext::HOST, host::FILE_BY_PATH, body);
}

std::optional<FileRecord> decode_file_record(std::string_view value) {
    auto body = payload(value, Ext::HOST, host::FILE_BY_PATH);
    if (!body || body->size() != 28) return std::nullopt;
    FileRecord record;
    record.file_id = read_u32(*body);
    record.mtime = read_u64(body->substr(4));
    record.hash = read_u64(body->substr(12));
    record.size = read_u64(body->substr(20));
    return record;
}

namespace {

// A list as {count u32, then per value {length u32, bytes}}; an over-cap
// (nullopt) list is the count OVER_CAP.
constexpr std::uint32_t OVER_CAP = 0xFFFFFFFFU;

void append_list(std::string& out,
                 const std::optional<std::vector<std::string>>& list) {
    if (!list) {
        append_u32(out, OVER_CAP);
        return;
    }
    append_u32(out, static_cast<std::uint32_t>(list->size()));
    for (const auto& v : *list) {
        append_u32(out, static_cast<std::uint32_t>(v.size()));
        out.append(v);
    }
}

bool read_list(std::string_view& in,
               std::optional<std::vector<std::string>>& list) {
    if (in.size() < 4) return false;
    const std::uint32_t n = read_u32(in);
    in.remove_prefix(4);
    if (n == OVER_CAP) {
        list.reset();
        return true;
    }
    list.emplace();
    for (std::uint32_t i = 0; i < n; ++i) {
        if (in.size() < 4) return false;
        const std::uint32_t len = read_u32(in);
        in.remove_prefix(4);
        if (in.size() < len) return false;
        list->emplace_back(in.substr(0, len));
        in.remove_prefix(len);
    }
    return true;
}

}  // namespace

std::string encode_chunk_metadata(const ChunkMetadata& metadata) {
    std::string body;
    append_u64(body, metadata.records);
    append_list(body, metadata.names);
    append_list(body, metadata.paths);
    return body;
}

std::optional<ChunkMetadata> decode_chunk_metadata(std::string_view body) {
    if (body.size() < 8) return std::nullopt;
    ChunkMetadata m;
    m.records = read_u64(body);
    body.remove_prefix(8);
    if (!read_list(body, m.names) || !read_list(body, m.paths) || !body.empty())
        return std::nullopt;
    return m;
}

}  // namespace dftracer::utils::index::store::layout

namespace dftracer::utils::index::store {

void add_metadata_value(std::optional<std::vector<std::string>>& list,
                        std::string_view value) {
    if (!list) return;
    auto it = std::lower_bound(list->begin(), list->end(), value);
    if (it != list->end() && *it == value) return;
    if (list->size() == METADATA_VALUES_CAP) {
        list.reset();
        return;
    }
    list->emplace(it, value);
}

}  // namespace dftracer::utils::index::store
