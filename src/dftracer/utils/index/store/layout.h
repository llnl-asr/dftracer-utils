#ifndef DFTRACER_UTILS_INDEX_STORE_LAYOUT_H
#define DFTRACER_UTILS_INDEX_STORE_LAYOUT_H

#include <dftracer/utils/index/store/file.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The on-disk layout of an index database. Index data keys are
// [ext_id u16][kind u8][file_id u32][tail], big-endian; every non-empty value
// starts with a VALUE_HEADER_BYTES header. Only index/store reads and writes
// these bytes.
namespace dftracer::utils::index::store::layout {

enum class Family : std::uint8_t {
    REGISTRY,
    MEMBERS,
    GRANULE,
    BLOB,
    POSTINGS,
    AGGREGATION,
    SYSTEM_METRICS,
};

inline constexpr std::size_t FAMILY_COUNT = 7;

std::string_view family_name(Family family);

using Ext = IndexExtension;

std::string_view ext_name(Ext ext);

namespace host {
inline constexpr std::uint8_t FORMAT = 0;
inline constexpr std::uint8_t EXT = 1;
inline constexpr std::uint8_t FILE_BY_PATH = 2;
inline constexpr std::uint8_t FILE_BY_ID = 3;
inline constexpr std::uint8_t NEXT_FILE_ID = 4;
inline constexpr std::uint8_t MANIFEST = 5;
}  // namespace host

namespace members {
inline constexpr std::uint8_t MEMBER = 0;
inline constexpr std::uint8_t METADATA = 1;
inline constexpr std::uint8_t RESTART = 2;
}  // namespace members

/// Kinds of the path-keyed pruning extensions (zonemap, bloom, counts,
/// postings). PATHS lists a file's indexed paths; DATA is per chunk and path,
/// keyed [path][0x00][granule]; FILE is per file and path (bloom); postings
/// use IN_FILE [path][0x00][value hash] and IN_GRANULE (that plus granule).
namespace path_kind {
inline constexpr std::uint8_t PATHS = 0;
inline constexpr std::uint8_t DATA = 1;
inline constexpr std::uint8_t FILE = 2;
inline constexpr std::uint8_t IN_FILE = 1;
inline constexpr std::uint8_t IN_GRANULE = 2;
}  // namespace path_kind

namespace stats_kind {
inline constexpr std::uint8_t CHUNK_STATS = 0;
inline constexpr std::uint8_t FILE_SCALAR_STATS = 1;
inline constexpr std::uint8_t FILE_PID_TID_COUNTS = 2;
}  // namespace stats_kind

/// The record schema: one ID record per file holding the schema id.
namespace profile_kind {
inline constexpr std::uint8_t ID = 0;
}  // namespace profile_kind

/// Metadata counts: one CHUNK record per chunk, {records u64, context u64}.
namespace metadata_kind {
inline constexpr std::uint8_t CHUNK = 0;
}  // namespace metadata_kind

/// The catalog: one PATH record per file and JSON path, keyed [path].
namespace catalog_kind {
inline constexpr std::uint8_t PATH = 0;
}  // namespace catalog_kind

/// Row sets: one FRAME record per file and row set, keyed [name], holding
/// the rows as an Arrow IPC frame.
namespace rowset_kind {
inline constexpr std::uint8_t FRAME = 0;
}  // namespace rowset_kind

/// file_id of data that belongs to the whole index rather than one file.
inline constexpr std::uint32_t INDEX_WIDE = 0xFFFFFFFFU;

struct KindSpec {
    Ext ext;
    std::uint8_t kind;
    Family family;
};

/// Every (ext, kind) of index data with its family.
std::span<const KindSpec> kinds();

Family family_of(Ext ext, std::uint8_t kind);

/// Code version of each extension's data; a manifest entry with another
/// version is stale.
std::uint32_t ext_version(Ext ext);

void append_u16(std::string& out, std::uint16_t value);
void append_u32(std::string& out, std::uint32_t value);
void append_u64(std::string& out, std::uint64_t value);
std::uint16_t read_u16(std::string_view bytes);
std::uint32_t read_u32(std::string_view bytes);
std::uint64_t read_u64(std::string_view bytes);

/// [ext][kind]
std::string kind_prefix(Ext ext, std::uint8_t kind);
/// [ext][kind][file_id]
std::string file_prefix(Ext ext, std::uint8_t kind, std::uint32_t file_id);
/// [ext][kind][file_id][granule]
std::string granule_key(Ext ext, std::uint8_t kind, std::uint32_t file_id,
                        std::uint32_t granule);
/// [ext][kind][file_id][path][0x00]: the start of one path's keys.
std::string path_prefix(Ext ext, std::uint8_t kind, std::uint32_t file_id,
                        std::string_view path);
/// The first key after every key starting with `prefix`.
std::string prefix_end(std::string_view prefix);

inline constexpr std::size_t KEY_PREFIX_BYTES = 7;
inline constexpr std::size_t VALUE_HEADER_BYTES = 9;

/// Prepends the value header for (ext, kind) to `payload`.
std::string with_header(Ext ext, std::uint8_t kind, std::string_view payload);

/// The payload of a value written for (ext, kind); nullopt when the header is
/// missing, truncated or names another type or version.
std::optional<std::string_view> payload(std::string_view value, Ext ext,
                                        std::uint8_t kind);

enum class ExtStatus : std::uint8_t { READY = 1, FAILED = 2 };

struct ManifestEntry {
    std::uint32_t version = 0;
    std::uint64_t params_hash = 0;
    ExtStatus status = ExtStatus::READY;
};

std::string encode_chunk_metadata(const ChunkMetadata& metadata);
std::optional<ChunkMetadata> decode_chunk_metadata(std::string_view body);

/// A catalog record: {type u8, seen u8, count u64}.
std::string encode_path_stat(const PathStat& stat);
std::optional<PathStat> decode_path_stat(std::string_view body);

std::string manifest_key(std::uint32_t file_id, Ext ext);
/// The manifest entry of plugin extension `name`.
std::string plugin_manifest_key(std::uint32_t file_id, std::string_view name);
std::string manifest_prefix(std::uint32_t file_id);
std::string encode_manifest(const ManifestEntry& entry);
std::optional<ManifestEntry> decode_manifest(std::string_view value);

std::string format_key();
std::string next_file_id_key();
std::string ext_registry_key(Ext ext);
std::string file_by_path_key(std::string_view logical_path);
std::string file_by_id_key(std::uint32_t file_id);

struct FileRecord {
    std::uint32_t file_id = 0;
    std::uint64_t mtime = 0;
    std::uint64_t hash = 0;
    std::uint64_t size = 0;
};

std::string encode_file_record(const FileRecord& record);
std::optional<FileRecord> decode_file_record(std::string_view value);

}  // namespace dftracer::utils::index::store::layout

#endif  // DFTRACER_UTILS_INDEX_STORE_LAYOUT_H
