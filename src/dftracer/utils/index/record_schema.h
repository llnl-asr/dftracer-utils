// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_RECORD_SCHEMA_H
#define DFTRACER_UTILS_INDEX_RECORD_SCHEMA_H

#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/hash/xxh3.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

namespace dftracer::utils::index {

/// How a schema's records are decoded: the dftracer event decoder, or every
/// scalar leaf by exact JSON path.
enum class Decoder : std::uint8_t { Dftracer = 0, Path = 1 };

/// The type a field declares; a View reads the field as this type. JSON holds
/// any value, an object or array included, as canonical JSON text.
enum class FieldType : std::uint8_t { Bool, Int, Float, String, Json };

/// The trace role a field plays. ENTITY is the process a record belongs to
/// (dftracer pid), LANE the thread within it (tid) and NAME the event's name.
enum class Role : std::uint8_t { None, Time, Duration, Entity, Lane, Name };

/// The unit of a time or duration field.
enum class TimeUnit : std::uint8_t { Ns, Us, Ms, S };

/// Microseconds in one `unit`.
double micros_per(TimeUnit unit);

/// `d` as an int field reads it: an int64 when `d` is a whole number in range.
inline dstd::Optional<std::int64_t> whole_int64(double d) {
    // -min is 2^63 exactly, the first double past int64.
    constexpr double LIMIT =
        -static_cast<double>(std::numeric_limits<std::int64_t>::min());
    if (std::trunc(d) != d || std::abs(d) >= LIMIT) return dstd::nullopt;
    return static_cast<std::int64_t>(d);
}

/// Microseconds since the Unix epoch of an ISO-8601 time such as
/// "2024-01-02T03:04:05.123456Z" or "2024-01-02 03:04:05+02:00"; nullopt when
/// `text` is not one.
dstd::Optional<std::int64_t> iso8601_micros(dstd::StringView text);

/// The integer id a trace lane shows for an entity or lane value written as
/// `text`: the integer itself, else a stable non-negative 31-bit hash of the
/// text, exact as a JavaScript number.
inline std::int64_t entity_id(dstd::StringView text) {
    std::int64_t v = 0;
    const char* end = text.data() + text.size();
    if (auto [ptr, ec] = dstd::from_chars(text.data(), end, v);
        ec == std::errc() && ptr == end && !text.empty())
        return v;
    return static_cast<std::int64_t>(
        dftracer::utils::dstd::hash::xxh3_64(text) & 0x7FFFFFFFULL);
}

/// "bool", "int", "float", "string" or "json".
dstd::StringView field_type_name(FieldType type);

/// One declared field of a record schema.
struct FieldSpec {
    /// Identifies the field; a child field of the same name replaces the
    /// parent's.
    dstd::String name;
    /// The JSON path, as the records write it; empty means `name`.
    dstd::String path;
    FieldType type = FieldType::String;
    /// An optional field is not needed for detection; a schema whose fields
    /// are all optional is detected by any of them.
    bool optional = false;
    Role role = Role::None;
    /// Only for the TIME and DURATION roles; microseconds when unset.
    dstd::Optional<TimeUnit> unit;
    /// Indexed even past the path budget.
    bool always_index = false;

    bool operator==(const FieldSpec&) const = default;
};

/// The paths that play a trace role in a schema's records, derived from its
/// fields; empty when the schema has no such role. `phase` is set only by the
/// dftracer decoder.
struct Roles {
    dstd::String time;
    dstd::String duration;
    dstd::String entity;
    dstd::String lane;
    dstd::String name;
    dstd::String phase;
    TimeUnit time_unit = TimeUnit::Us;
    TimeUnit duration_unit = TimeUnit::Us;
};

/// What register_schema resolves into a @ref
/// dftracer::utils::index::RecordSchema "RecordSchema": fields and source
/// members (by name), the path budget and the stats share override the
/// parent's.
struct SchemaSpec {
    dstd::String id;
    dstd::String extends = "generic";
    dstd::Vector<FieldSpec> fields;
    dstd::Optional<std::size_t> path_budget;
    dstd::Optional<double> stats_share;
    /// Added to the parent's `never_index` paths.
    dstd::Vector<dstd::String> never_index;
    /// duql source members (`name = pipeline`, `def ...`), `;` separated.
    dstd::String source;
};

/// A record format: how to detect it, decode it and read its roles.
struct RecordSchema {
    dstd::String id;
    Decoder decoder = Decoder::Path;
    /// Resolved fields, the parent's first, each with its path set.
    dstd::Vector<FieldSpec> fields;
    /// The duql source: row sets (`name = pipeline`) and macros, as canonical
    /// text with the parent's members first. `data` is what a query with no
    /// `from` reads; empty reads every record.
    dstd::String source;
    /// The condition of the source's `data` row set, as canonical text;
    /// empty when `data` reads every record.
    dstd::String data;
    /// The source defines `args_fallback = true`: a bare name its records
    /// lack reads the field of that name under `args`.
    bool args_fallback = false;
    /// The path budget for files of this schema; the build's when unset.
    dstd::Optional<std::size_t> path_budget;
    /// The share of a file's size its evidence may use, in (0, 1]; the
    /// build's when unset.
    dstd::Optional<double> stats_share;
    /// Paths of the required fields; more paths make a more specific schema.
    dstd::Vector<dstd::String> require;
    Roles roles;
    /// Paths indexed past the path budget: `always_index` fields and, for
    /// path-decoded records, the time field.
    dstd::Vector<dstd::String> always_index;
    /// Paths the index build lists in the path catalog but keeps no evidence
    /// for and interns no value of (`index.never_index`); sorted and unique.
    /// Queries still read them.
    dstd::Vector<dstd::String> never_index;
    /// The library's own definition (dftracer, generic, genesis).
    bool builtin = false;
    /// Whether a field has the JSON type.
    bool has_json = false;
    /// (path, index into `fields`), sorted by path, for field_at.
    dstd::Vector<dstd::Pair<dstd::String, std::size_t>> fields_by_path;

    /// The field at `path`, or null.
    const FieldSpec* field_at(dstd::StringView path) const;

    /// Hash of the definition, recorded with each file indexed under it.
    std::uint64_t params_hash() const;
};

/// Every registered schema: the built-ins "dftracer", "generic" and
/// "genesis" first, then user schemas in registration order. Schemas stay
/// registered, and the pointers valid, until the process exits. The first
/// registry access loads $DFTRACER_UTILS_SCHEMA_PATH. Thread-safe.
dstd::Vector<const RecordSchema*> registered_schemas();

/// The registered schema `id`, or null.
const RecordSchema* find_schema(dstd::StringView id);

/// The registered schema `id`. Throws @ref
/// dftracer::utils::common::DFTUtilsException "DFTUtilsException"
/// INVALID_ARGUMENT, listing the registered ids and where schemas load from,
/// for any other.
const RecordSchema& get_schema(dstd::StringView id);

/// Resolves `spec` against its parent and registers it; `source` names it in
/// errors. Throws @ref dftracer::utils::common::DFTUtilsException
/// "DFTUtilsException" INVALID_ARGUMENT for an empty id or field name, a
/// built-in id, an unknown or self `extends`, a unit without a time or duration
/// role, two fields with one role or one path, or an id registered with another
/// definition; registering the same definition again returns the registered
/// schema.
const RecordSchema& register_schema(const SchemaSpec& spec,
                                    dstd::StringView source);

/// Registers the schema the YAML or JSON spec `text` describes. Keys: id,
/// extends, fields (name -> type, path, optional, role, unit, always_index),
/// index.path_budget, index.never_index (a list of paths) and source (duql
/// source members). Throws as the @ref SchemaSpec overload, and for an unknown
/// key or a value of the wrong type.
const RecordSchema& register_schema(dstd::StringView text,
                                    dstd::StringView source);

/// Registers every spec at `path`: a file, or the *.yaml, *.yml and *.json
/// files of a directory in name order. A path is loaded once per process.
/// Throws as register_schema, or IO when a file cannot be read.
void load_schemas(dstd::StringView path);

/// The registered schemas as a JSON array of objects with id, decoder
/// ("dftracer" or "path"), fields, require, path_budget (null when unset) and
/// source (its text), in registration order.
dstd::String schemas_json();

/// load_schemas on `<index_dir>/schemas` for the index at `index_path`
/// (`<index_dir>/.dftindex`), when that directory exists.
void load_index_schemas(dstd::StringView index_path);

/// Share of the records a schema judges that it must match to be chosen.
inline constexpr double SCHEMA_MATCH_SHARE = 0.9;
/// Records (objects no schema's `data` row set leaves out) sampled from the
/// start of a file; metadata lines before them are read past, up to 16 MiB of
/// text.
inline constexpr std::size_t SCHEMA_SAMPLE_LINES = 1000;

/// One schema's detection score.
struct SchemaScore {
    const RecordSchema* schema = nullptr;
    /// Share of the objects the schema judges (those its `data` row set does
    /// not leave out) that match it; 0 when it judges none.
    double share = 0;
};

/// What detection saw: every registered schema's score, in registration
/// order, and the schema it chooses.
struct SchemaDetection {
    dstd::Vector<SchemaScore> scores;
    const RecordSchema* chosen = nullptr;
    /// JSON objects sampled.
    std::size_t objects = 0;
    /// Objects no schema's `data` row set leaves out. A file with none (empty,
    /// or only metadata) takes no vote on the schema of a View of many files.
    std::size_t records = 0;
};

/// Scores every registered schema on `lines`; whitespace and a trailing comma
/// (JSON array traces) around a line are ignored. A schema judges the objects
/// its `data` row set does not leave out; an object matches when it holds
/// every required path or, for a schema whose fields are all optional, any
/// declared path. Paths resolve as json::JsonValue::at. The chosen schema is
/// one that at least SCHEMA_MATCH_SHARE of the objects it judges match,
/// preferring more required paths, then a user schema over a built-in, then
/// the higher share, then the lower id. With only metadata sampled it is the
/// schema with the fewest required paths whose `data` leaves out every
/// object. Otherwise "generic"; a schema declaring no field is never chosen.
SchemaDetection explain_schema(dstd::Span<const dstd::StringView> lines);

/// explain_schema over the lines at the start of `file_path` (gzip or
/// plain), up to SCHEMA_SAMPLE_LINES records; the last line needs no
/// newline. Cached per process by path, modification time, size and the
/// registered schemas. Throws @ref dftracer::utils::common::DFTUtilsException
/// "DFTUtilsException" IO when the file cannot be read.
SchemaDetection explain_file_schema(dstd::CStringView file_path);

/// The same over an open file of `size` bytes modified at `mtime_ns`
/// nanoseconds since the epoch, read at offsets. `file` stays open and owned
/// by the caller.
SchemaDetection explain_file_schema(dstd::CStringView file_path,
                                    const dstd::File& file, std::uint64_t size,
                                    std::int64_t mtime_ns);

/// `d` as a JSON object: chosen (id), objects, records, and scores, each with
/// id, required (the count of required paths) and share.
dstd::String to_json(const SchemaDetection& d);

/// explain_schema(lines).chosen.
const RecordSchema& detect_schema(dstd::Span<const dstd::StringView> lines);

/// explain_file_schema(file_path).chosen.
const RecordSchema& detect_file_schema(dstd::CStringView file_path);

/// explain_file_schema(file_path, file, size, mtime_ns).chosen.
const RecordSchema& detect_file_schema(dstd::CStringView file_path,
                                       const dstd::File& file,
                                       std::uint64_t size,
                                       std::int64_t mtime_ns);

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_RECORD_SCHEMA_H
