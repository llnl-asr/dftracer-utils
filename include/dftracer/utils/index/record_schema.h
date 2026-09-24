#ifndef DFTRACER_UTILS_INDEX_RECORD_SCHEMA_H
#define DFTRACER_UTILS_INDEX_RECORD_SCHEMA_H

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

/// The trace role a field plays.
enum class Role : std::uint8_t { NONE, TIME, DURATION, ENTITY };

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
    /// An optional field is not needed for detection.
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
    std::string phase;
    TimeUnit time_unit = TimeUnit::US;
    TimeUnit duration_unit = TimeUnit::US;
};

/// Rows of the records named `rows`, each keyed by the value at `key` and
/// holding `fields` (field name -> path in the record). Data records hold
/// keys in the fields `keys_in`, and `resolved.<key field>.<field>` reads a
/// row's field through them.
struct Dictionary {
    std::string name;
    std::string rows;
    std::string key;
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<std::string> keys_in;

    bool has_field(std::string_view field) const;
};

/// Prefix of the columns a schema's dictionaries resolve.
inline constexpr std::string_view RESOLVED_PREFIX = "resolved.";

/// `resolved.<key_field>.<field>`: `field` of the `dictionary` row whose key
/// the data field `key_field` holds.
struct ResolvedColumn {
    std::string key_field;
    const Dictionary* dictionary = nullptr;
    std::string field;
};

/// What register_schema resolves into a RecordSchema: fields, dictionaries
/// (by name) and the path budget override the parent's.
struct SchemaSpec {
    std::string id;
    std::string extends = "generic";
    std::vector<FieldSpec> fields;
    std::optional<std::size_t> path_budget;
    std::vector<Dictionary> dictionaries;
};

/// A record format: how to detect it, decode it and read its roles.
struct RecordSchema {
    std::string id;
    Decoder decoder = Decoder::PATH;
    /// Resolved fields, the parent's first, each with its path set.
    std::vector<FieldSpec> fields;
    std::vector<Dictionary> dictionaries;
    /// The path budget for files of this schema; the build's when unset.
    std::optional<std::size_t> path_budget;
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

    /// The dictionary whose keys the data field `key_field` holds, or null.
    const Dictionary* dictionary_of(std::string_view key_field) const;

    /// Parses a `resolved.` column name. Throws DFTUtilsException
    /// INVALID_ARGUMENT, listing resolved_names(), when it names no field of a
    /// dictionary through one of its key fields.
    ResolvedColumn resolved_column(std::string_view name) const;
    /// Every `resolved.` column name the schema accepts.
    std::vector<std::string> resolved_names() const;

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
/// index.path_budget and dictionaries (name, rows, key, fields, keys_in).
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
/// dictionaries (their names), in registration order.
std::string schemas_json();

/// load_schemas on `<index_dir>/schemas` for the index at `index_path`
/// (`<index_dir>/.dftindex`), when that directory exists.
void load_index_schemas(const std::string& index_path);

/// Share of sampled records a schema must match to be chosen.
inline constexpr double SCHEMA_MATCH_SHARE = 0.9;
/// Records sampled from the start of a file.
inline constexpr std::size_t SCHEMA_SAMPLE_LINES = 1000;

/// One schema's detection score.
struct SchemaScore {
    const RecordSchema* schema = nullptr;
    /// Share of the sampled JSON objects holding every required path; 0 when
    /// no line is an object.
    double share = 0;
};

/// What detection saw: every registered schema's score, in registration
/// order, and the schema it chooses.
struct SchemaDetection {
    std::vector<SchemaScore> scores;
    const RecordSchema* chosen = nullptr;
    std::size_t objects = 0;
};

/// Scores every registered schema on `lines`. The chosen schema is the one
/// with the most required paths (inherited ones included) that at least
/// SCHEMA_MATCH_SHARE of the JSON objects satisfy, the higher share and then
/// the lower id breaking a tie; "generic" when none does or no line is an
/// object. A schema requiring nothing is never chosen.
SchemaDetection explain_schema(std::span<const std::string_view> lines);

/// explain_schema over up to SCHEMA_SAMPLE_LINES lines from the start of
/// `file_path` (gzip or plain). Throws DFTUtilsException IO when the file
/// cannot be read.
SchemaDetection explain_file_schema(const std::string& file_path);

/// `d` as a JSON object: chosen (id), objects, and scores, each with id,
/// required (the count of required paths) and share.
std::string to_json(const SchemaDetection& d);

/// explain_schema(lines).chosen.
const RecordSchema& detect_schema(std::span<const std::string_view> lines);

/// explain_file_schema(file_path).chosen.
const RecordSchema& detect_file_schema(const std::string& file_path);

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_RECORD_SCHEMA_H
