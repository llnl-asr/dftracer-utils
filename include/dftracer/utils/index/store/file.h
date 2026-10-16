#ifndef DFTRACER_UTILS_INDEX_STORE_FILE_H
#define DFTRACER_UTILS_INDEX_STORE_FILE_H

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::store {

struct FileMetadataResult {
    std::uint64_t checkpoint_size = 0;
    std::uint64_t num_lines = 0;
    std::uint64_t max_bytes = 0;
    /// The trace's last gzip member was cut short when it was indexed.
    bool truncated = false;
};

struct FileRegistryEntry {
    int file_id = -1;
};

/// Owners of index data. The manifest records, per file, which of them are
/// built and with which configuration.
enum class IndexExtension : std::uint16_t {
    Host = 0,
    /// Gzip members and file metadata.
    Members = 1,
    /// Per chunk and path: value range, type, record counts.
    Zonemap = 4,
    /// Per chunk and path, and per file: a bloom of the path's values.
    Bloom = 5,
    /// Per chunk and path: value counts up to a cap.
    Counts = 6,
    /// Per file and path: value to chunks.
    Postings = 7,
    /// dftracer statistics no pruning source reads: chunk and file summaries
    /// and pid:tid counts.
    Stats = 8,
    /// Per file: every JSON path, its type and its non-null record count.
    Catalog = 9,
    /// Per file: the id of the schema its records were decoded with.
    Profile = 10,
    /// Per chunk: how many metadata records, with their names and paths.
    Metadata = 11,
    /// Plugin extensions (plugins/abi/index.h), each keyed by its name, with
    /// one manifest entry per name.
    Plugin = 12,
    /// The dftracer pre-aggregation tier; its rows live in the aggregation
    /// and system_metrics families without a file id, and this extension's
    /// manifest entry marks a file's rows as merged into them.
    Agg = 13,
    /// Per file and row set: the rows of each row set of the schema's source
    /// that the build evaluates, as an Arrow IPC frame keyed by its name.
    Rowset = 14,
};

/// The type of a JSON path. A number is INT when it fits in int64 and UINT
/// only when it does not. MIXED is a join of incompatible types. OBJECT and
/// ARRAY mark an empty object or array, recorded so a path that holds one is
/// known to exist.
enum class PathType : std::uint8_t {
    NullValue = 0,
    Bool = 1,
    Int = 2,
    Uint = 3,
    Double = 4,
    String = 5,
    Mixed = 6,
    Object = 7,
    Array = 8,
};

/// The least type both `a` and `b` promote to: NULL_VALUE yields to the
/// other, INT and UINT join to UINT, numbers join with DOUBLE to DOUBLE, and
/// anything else is MIXED. Commutative and associative.
constexpr PathType join(PathType a, PathType b) {
    if (a == b || b == PathType::NullValue) return a;
    if (a == PathType::NullValue) return b;
    auto number = [](PathType t) {
        return t == PathType::Int || t == PathType::Uint ||
               t == PathType::Double;
    };
    if (!number(a) || !number(b)) return PathType::Mixed;
    if (a == PathType::Double || b == PathType::Double) return PathType::Double;
    return PathType::Uint;
}

/// One path of a file's catalog.
struct PathStat {
    PathType type = PathType::NullValue;
    /// Bit (1 << PathType) for every scalar type seen, MIXED included; an
    /// empty object or array sets none.
    std::uint8_t seen = 0;
    /// Records in which the path is present and not null.
    std::uint64_t count = 0;
};

/// One chunk's metadata (`ph="M"`) records. Context records are those other
/// than the FH, HH and SH hash definitions: thread and process names, PR, CM.
struct ChunkMetadata {
    std::uint64_t records = 0;
    /// The records' distinct names and field paths ("pid", "args.name"),
    /// sorted; nullopt when there were more than METADATA_VALUES_CAP.
    std::optional<std::vector<std::string>> names = std::vector<std::string>{};
    std::optional<std::vector<std::string>> paths = std::vector<std::string>{};
};

inline constexpr std::size_t METADATA_VALUES_CAP = 64;

/// Adds `value` to a sorted ChunkMetadata list, which becomes nullopt past
/// METADATA_VALUES_CAP.
void add_metadata_value(std::optional<std::vector<std::string>>& list,
                        std::string_view value);

/// A set of extensions.
class ExtensionMask {
   public:
    constexpr ExtensionMask() = default;
    constexpr ExtensionMask(std::initializer_list<IndexExtension> exts) {
        for (auto ext : exts) add(ext);
    }
    constexpr bool has(IndexExtension ext) const {
        return (bits_ >> static_cast<std::uint16_t>(ext)) & 1U;
    }
    constexpr void add(IndexExtension ext) {
        bits_ |= static_cast<std::uint32_t>(1U << static_cast<unsigned>(ext));
    }
    constexpr void add(ExtensionMask other) { bits_ |= other.bits_; }
    constexpr bool empty() const { return bits_ == 0; }
    constexpr bool operator==(const ExtensionMask&) const = default;

   private:
    std::uint32_t bits_ = 0;
};

/// The pruning extensions; a build makes these unless told otherwise.
inline constexpr ExtensionMask PRUNING_EXTENSIONS = {
    IndexExtension::Zonemap, IndexExtension::Bloom, IndexExtension::Counts,
    IndexExtension::Postings};

/// Every extension a build writes, in manifest order.
inline constexpr IndexExtension ALL_EXTENSIONS[] = {
    IndexExtension::Members, IndexExtension::Zonemap,  IndexExtension::Bloom,
    IndexExtension::Counts,  IndexExtension::Postings, IndexExtension::Stats,
    IndexExtension::Catalog, IndexExtension::Profile,  IndexExtension::Metadata,
    IndexExtension::Agg,     IndexExtension::Rowset};

/// The extension's name, such as "zonemap" or "core.members".
std::string_view extension_name(IndexExtension ext);
/// The extension named `name`; nullopt for an unknown name.
std::optional<IndexExtension> parse_extension(std::string_view name);

/// A manifest entry: the extension was built for the file at `version` with
/// configuration `params_hash`; `ready` is false when the build failed.
struct ExtensionState {
    std::uint32_t version = 0;
    std::uint64_t params_hash = 0;
    bool ready = false;
};

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_FILE_H
