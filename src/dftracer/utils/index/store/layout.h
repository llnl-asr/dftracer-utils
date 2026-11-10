// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_LAYOUT_H
#define DFTRACER_UTILS_INDEX_STORE_LAYOUT_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/file.h>

#include <cstdint>

// The on-disk layout of an index database. A key is [kind u8][tail], its tail
// big-endian, inside the namespace of its extension and the scope of its file.
// A value is the payload alone. Only index/store reads and writes these bytes.
namespace dftracer::utils::index::store::layout {

using Ext = IndexExtension;

dstd::StringView ext_name(Ext ext);

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

/// Aggregation kinds. Every tail is written after the kind byte, so a file's
/// records are one range in key order.
///   PARTIAL  [chunk u32][category id u32][name id u32][host id u32]
///            [extra id u32][kind u8]
///   DICT     [id u32]
///   META     [name], a namespace per name at the root scope
///   COLUMNS  [chunk u32], a column block of the system counter rows (phase
///            Counter, category sys) of the chunk (see
///            schemas/dft/agg/chunk_columns.h)
///   FILE_PARTIAL  [chunk u32][file id u32][kind u8], the events of the chunk
///            that carry one file hash, whatever their category, name or
///            host, as a PARTIAL value. The file id is the dictionary id of
///            the hash text, and NO_FILE for the events with no hash.
/// The ids are the index root's dictionary ids. The extra id is that of the
/// extra group key values joined by 0x1F, the empty text when there are none.
namespace agg_kind {
inline constexpr std::uint8_t PARTIAL = 0;
inline constexpr std::uint8_t META = 1;
inline constexpr std::uint8_t DICT = 2;
/// Retired kind. The value stays unused so a later kind does not take the id
/// of an older store's namespace.
inline constexpr std::uint8_t UNUSED_SYSTEM = 3;
inline constexpr std::uint8_t COLUMNS = 4;
inline constexpr std::uint8_t FILE_PARTIAL = 5;
/// The file id of the events with no file hash.
inline constexpr std::uint32_t NO_FILE = 0xFFFFFFFFu;
}  // namespace agg_kind

/// Code version of each extension's data; a manifest entry with another
/// version is stale.
std::uint32_t ext_version(Ext ext);

/// The big-endian integer at the start of `bytes`, which must hold at least
/// its width; a shorter input ends the process.
std::uint16_t read_u16(dstd::StringView bytes);
std::uint32_t read_u32(dstd::StringView bytes);
std::uint64_t read_u64(dstd::StringView bytes);

/// The first key after every key starting with `prefix`.
dstd::String prefix_end(dstd::StringView prefix);

dstd::String encode_chunk_metadata(const ChunkMetadata& metadata);
dstd::Optional<ChunkMetadata> decode_chunk_metadata(dstd::StringView body);

/// A catalog record: {type u8, seen u8, count u64}.
dstd::String encode_path_stat(const PathStat& stat);
dstd::Optional<PathStat> decode_path_stat(dstd::StringView body);

}  // namespace dftracer::utils::index::store::layout

#endif  // DFTRACER_UTILS_INDEX_STORE_LAYOUT_H
