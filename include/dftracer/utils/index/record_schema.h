#ifndef DFTRACER_UTILS_INDEX_RECORD_SCHEMA_H
#define DFTRACER_UTILS_INDEX_RECORD_SCHEMA_H

#include <dftracer/utils/core/common/hash/fnv1a.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index {

/// How a schema's records are decoded: the dftracer event decoder, or every
/// scalar leaf by exact JSON path.
enum class Decoder : std::uint8_t { DFTRACER = 0, PATH = 1 };

/// The type a field declares; a View reads the field as this type. JSON holds
/// any value, an object or array included, as canonical JSON text.
enum class FieldType : std::uint8_t { BOOL, INT, FLOAT, STRING, JSON };

/// The trace role a field plays. ENTITY is the process a record belongs to
/// (dftracer pid), LANE the thread within it (tid) and NAME the event's name.
enum class Role : std::uint8_t { NONE, TIME, DURATION, ENTITY, LANE, NAME };

/// The unit of a time or duration field.
enum class TimeUnit : std::uint8_t { NS, US, MS, S };

/// Microseconds in one `unit`.
double micros_per(TimeUnit unit);

/// `d` as an int field reads it: an int64 when `d` is a whole number in range.
inline std::optional<std::int64_t> whole_int64(double d) {
    // -min is 2^63 exactly, the first double past int64.
    constexpr double LIMIT =
        -static_cast<double>(std::numeric_limits<std::int64_t>::min());
    if (std::trunc(d) != d || std::abs(d) >= LIMIT) return std::nullopt;
    return static_cast<std::int64_t>(d);
}

/// Microseconds since the Unix epoch of an ISO-8601 time such as
/// "2024-01-02T03:04:05.123456Z" or "2024-01-02 03:04:05+02:00"; nullopt when
/// `text` is not one.
std::optional<std::int64_t> iso8601_micros(std::string_view text);

/// The integer id a trace lane shows for an entity or lane value written as
/// `text`: the integer itself, else a stable non-negative 31-bit hash of the
/// text, exact as a JavaScript number.
inline std::int64_t entity_id(std::string_view text) {
    std::int64_t v = 0;
    const char* end = text.data() + text.size();
    if (auto [ptr, ec] = std::from_chars(text.data(), end, v);
        ec == std::errc() && ptr == end && !text.empty())
        return v;
    return static_cast<std::int64_t>(dftracer::utils::hash::fnv1a_hash(text) &
                                     0x7FFFFFFFULL);
}

/// "bool", "int", "float", "string" or "json".
std::string_view field_type_name(FieldType type);

/// One declared field of a record schema.
struct FieldSpec {
    /// Identifies the field; a child field of the same name replaces the
    /// parent's.
    std::string name;
    /// The JSON path, as the records write it; empty means `name`.
    std::string path;
    FieldType type = FieldType::STRING;
    /// An optional field is not needed for detection; a schema whose fields
    /// are all optional is detected by any of them.
    bool optional = false;
    Role role = Role::NONE;
    /// Only for the TIME and DURATION roles; microseconds when unset.
    std::optional<TimeUnit> unit;
    /// Indexed even past the path budget.
    bool always_index = false;

    bool operator==(const FieldSpec&) const = default;
};

/// The paths that play a trace role in a schema's records, derived from its
/// fields; empty when the schema has no such role. `phase` is set only by the
/// dftracer decoder.
struct Roles {
    std::string time;
    std::string duration;
    std::string entity;
    std::string lane;
    std::string name;
    std::string phase;
    TimeUnit time_unit = TimeUnit::US;
    TimeUnit duration_unit = TimeUnit::US;
};

/// What register_schema resolves into a RecordSchema: fields and source
/// members (by name), the path budget and the stats share override the
/// parent's.
struct SchemaSpec {
    std::string id;
    std::string extends = "generic";
    std::vector<FieldSpec> fields;
    std::optional<std::size_t> path_budget;
    std::optional<double> stats_share;
    /// duql source members (`name = pipeline`, `def ...`), `;` separated.
    std::string source;
};

/// A record format: how to detect it, decode it and read its roles.
struct RecordSchema {
    std::string id;
    Decoder decoder = Decoder::PATH;
    /// Resolved fields, the parent's first, each with its path set.
    std::vector<FieldSpec> fields;
    /// The duql source: row sets (`name = pipeline`) and macros, as canonical
    /// text with the parent's members first. `data` is what a query with no
    /// `from` reads; empty reads every record.
    std::string source;
    /// The condition of the source's `data` row set, as canonical text;
    /// empty when `data` reads every record.
    std::string data;
    /// The source defines `args_fallback = true`: a bare name its records
    /// lack reads the field of that name under `args`.
    bool args_fallback = false;
    /// The path budget for files of this schema; the build's when unset.
    std::optional<std::size_t> path_budget;
    /// The share of a file's size its evidence may use, in (0, 1]; the
    /// build's when unset.
    std::optional<double> stats_share;
    /// Paths of the required fields; more paths make a more specific schema.
    std::vector<std::string> require;
    Roles roles;
    /// Paths indexed past the path budget: `always_index` fields and, for
    /// path-decoded records, the time field.
    std::vector<std::string> always_index;
    /// The library's own definition (dftracer, generic, genesis).
    bool builtin = false;
    /// Whether a field has the JSON type.
    bool has_json = false;
    /// (path, index into `fields`), sorted by path, for field_at.
    std::vector<std::pair<std::string, std::size_t>> fields_by_path;

    /// The field at `path`, or null.
    const FieldSpec* field_at(std::string_view path) const;

    /// Hash of the definition, recorded with each file indexed under it.
    std::uint64_t params_hash() const;
};

/// Every registered schema: the built-ins "dftracer", "generic" and
/// "genesis" first, then user schemas in registration order. Schemas stay
/// registered, and the pointers valid, until the process exits. The first
/// registry access loads $DFTRACER_SCHEMA_PATH. Thread-safe.
std::vector<const RecordSchema*> registered_schemas();

/// The registered schema `id`, or null.
const RecordSchema* find_schema(std::string_view id);

/// The registered schema `id`. Throws DFTUtilsException INVALID_ARGUMENT,
/// listing the registered ids and where schemas load from, for any other.
const RecordSchema& get_schema(std::string_view id);

/// Resolves `spec` against its parent and registers it; `source` names it in
/// errors. Throws DFTUtilsException INVALID_ARGUMENT for an empty id or field
/// name, a built-in id, an unknown or self `extends`, a unit without a time
/// or duration role, two fields with one role or one path, or an id
/// registered with another definition; registering the same definition
/// again returns the registered schema.
const RecordSchema& register_schema(const SchemaSpec& spec,
                                    std::string_view source);

/// Registers the schema the YAML or JSON spec `text` describes. Keys: id,
/// extends, fields (name -> type, path, optional, role, unit, always_index),
/// index.path_budget and source (duql source members).
/// Throws as the SchemaSpec overload, and for an unknown key or a value of
/// the wrong type.
const RecordSchema& register_schema(std::string_view text,
                                    std::string_view source);

/// Registers every spec at `path`: a file, or the *.yaml, *.yml and *.json
/// files of a directory in name order. A path is loaded once per process.
/// Throws as register_schema, or IO when a file cannot be read.
void load_schemas(const std::string& path);

/// The registered schemas as a JSON array of objects with id, decoder
/// ("dftracer" or "path"), fields, require, path_budget (null when unset) and
/// source (its text), in registration order.
std::string schemas_json();

/// load_schemas on `<index_dir>/schemas` for the index at `index_path`
/// (`<index_dir>/.dftindex`), when that directory exists.
void load_index_schemas(const std::string& index_path);

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
    std::vector<SchemaScore> scores;
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
SchemaDetection explain_schema(std::span<const std::string_view> lines);

/// explain_schema over the lines at the start of `file_path` (gzip or
/// plain), up to SCHEMA_SAMPLE_LINES records; the last line needs no
/// newline. Cached per process by path, modification time, size and the
/// registered schemas. Throws DFTUtilsException IO when the file cannot be
/// read.
SchemaDetection explain_file_schema(const std::string& file_path);

/// `d` as a JSON object: chosen (id), objects, records, and scores, each with
/// id, required (the count of required paths) and share.
std::string to_json(const SchemaDetection& d);

/// explain_schema(lines).chosen.
const RecordSchema& detect_schema(std::span<const std::string_view> lines);

/// explain_file_schema(file_path).chosen.
const RecordSchema& detect_file_schema(const std::string& file_path);

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_RECORD_SCHEMA_H
