#ifndef DFTRACER_UTILS_INDEX_STORE_SHARD_MANIFEST_H
#define DFTRACER_UTILS_INDEX_STORE_SHARD_MANIFEST_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::store {

/// One immutable index shard in a shard set. `path` is relative to the
/// directory holding the manifest and names a self-contained, read-only
/// `.dftindex` store. A shard owns a closed file-id range
/// [file_id_min, file_id_max] that is disjoint from every other shard's range,
/// so a merge reader opens all shards without key collisions. An empty shard
/// (num_files == 0) carries file_id_min > file_id_max.
struct IndexShardEntry {
    std::string path;
    std::int64_t file_id_min = 0;
    std::int64_t file_id_max = -1;
    std::uint64_t num_files = 0;
    std::uint64_t num_events = 0;

    bool empty() const noexcept { return num_files == 0; }
};

/// Catalog of the immutable shards that make up one logical index. Persisted as
/// a JSON file (SHARD_MANIFEST_FILENAME) at the shard-set root and published
/// atomically, so a reader sees either the whole previous manifest or the whole
/// new one, never a torn file. `format_version` is the index FORMAT_VERSION the
/// shards were built with; a reader rejects a manifest whose version it cannot
/// read.
struct IndexShardManifest {
    std::uint32_t format_version = 0;
    std::vector<IndexShardEntry> shards;
};

inline constexpr std::string_view SHARD_MANIFEST_FILENAME = "shards.json";

/// The hidden subdirectory a producer nests a shard set under by default, so
/// the shards and manifest do not clutter (or get scanned as inputs from) the
/// trace directory. Its `.dftindex` prefix makes the directory scanner skip it.
inline constexpr std::string_view SHARD_SET_DIRNAME = ".dftindex-shards";

/// Serialize to the on-disk JSON representation.
std::string to_json(const IndexShardManifest& manifest);

/// Parse the on-disk JSON representation. Throws DFTUtilsException on malformed
/// input.
IndexShardManifest parse_manifest(std::string_view json);

/// Write `manifest` to `<dir>/shards.json`, creating `dir` if needed. Writes a
/// temporary file in `dir` and renames it into place so a concurrent reader
/// never observes a partial manifest. Throws DFTUtilsException on I/O failure.
void write_shard_manifest(const std::string& dir,
                          const IndexShardManifest& manifest);

/// Read `<dir>/shards.json`. Returns std::nullopt when the file is absent;
/// throws DFTUtilsException when it exists but cannot be read or parsed.
std::optional<IndexShardManifest> read_shard_manifest(const std::string& dir);

/// True when `<dir>/shards.json` exists: `dir` is the root of a shard set. A
/// cheap existence check for tools that autodetect and route to a merge reader.
bool has_shard_manifest(const std::string& dir);

/// Resolve the shard-set root under `target`: `target` itself if it holds a
/// manifest, else `target/.dftindex-shards` (the default nested location),
/// else empty. Lets a tool autodetect a set by pointing at the trace directory.
std::string resolve_shard_set_root(const std::string& target);

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_SHARD_MANIFEST_H
