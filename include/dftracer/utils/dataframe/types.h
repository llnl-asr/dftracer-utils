#ifndef DFTRACER_UTILS_DATAFRAME_TYPES_H
#define DFTRACER_UTILS_DATAFRAME_TYPES_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

enum class TypeId : std::int32_t {
    /// Schema-only marker: a type not knowable without scanning, or an
    /// unset/zero-initialized TypeId. Never the type of a real Series;
    /// dispatch over actual column data must reject it like any other
    /// out-of-range value, not handle it as data.
    Unknown,
    Bool,
    Int8,
    Int16,
    Int32,
    Int64,
    Uint8,
    Uint16,
    Uint32,
    Uint64,
    Float32,
    Float64,
    String,
    Binary,
    List,
    Struct,
    /// 2-byte IEEE 754 half-precision float. No kernel operates on the raw
    /// half bits: arithmetic/reduction promote it to Float32 first (exact -
    /// every half value has a precise float32 representation), so the result
    /// of e.g. adding two Float16 columns is a Float32 column, not Float16.
    Float16,
    /// Days since the Unix epoch, stored as Int32 (physical_type()).
    /// Compare/sort/min/max/group_by/hash dispatch on that Int32 value and
    /// keep the Date32 tag on the result. Arithmetic is refused (see
    /// arithmetic.cpp): Date32/Date64 carry no `DataType::time_unit`, so a
    /// day offset or a date difference has no Duration granularity to land
    /// on without inventing one.
    Date32,
    /// Milliseconds since the Unix epoch, stored as Int64 (physical_type()).
    /// Same compute and arithmetic behavior as Date32.
    Date64,
    /// Time of day since midnight in `DataType::time_unit` (Second or Milli),
    /// stored as Int32 (physical_type()). Same compute behavior as Date32.
    /// Arithmetic is refused: Time64 (see below) covers Time arithmetic at
    /// Micro/Nano granularity; extending it to Time32's Int32 physical
    /// storage needs cross-width widening this engine does not build yet.
    Time32,
    /// Time of day since midnight in `DataType::time_unit` (Micro or Nano),
    /// stored as Int64 (physical_type()). Same compute behavior as Date32.
    /// Arithmetic (see arithmetic.cpp, temporal_arith.h): `Time64 - Time64`
    /// -> Duration, `Time64 +/- Duration` -> Time64, at the finer of the two
    /// operand units; `Time64 + Time64` stays refused (meaningless).
    Time64,
    /// Instant since the Unix epoch in `DataType::time_unit`, with an
    /// optional `DataType::timezone`, stored as Int64 (physical_type()).
    /// Same compute behavior as Date32. Arithmetic (see arithmetic.cpp,
    /// temporal_arith.h): `Timestamp - Timestamp` -> Duration (the operand
    /// timezones must match exactly, including naive-vs-aware, or the op is
    /// refused rather than guessed); `Timestamp +/- Duration` -> Timestamp,
    /// keeping the Timestamp operand's timezone. The result unit is the
    /// finer of the two operand units. `Timestamp + Timestamp` and any
    /// `Timestamp * or /` op stay refused: meaningless.
    Timestamp,
    /// Elapsed time in `DataType::time_unit`, stored as Int64
    /// (physical_type()). Same compute behavior as Timestamp. Arithmetic:
    /// `Duration +/- Duration` -> Duration (finer unit), `Duration *
    /// scalar`/`Duration / scalar` -> Duration (unit unchanged, the scalar is
    /// a dimensionless multiplier). `SUM` reduces a Duration column to a
    /// Duration total in its native unit; `Duration * or / Duration` and
    /// `PRODUCT` stay refused (not in the defined rule table).
    Duration,
    /// 128-bit fixed-point decimal; `DataType::decimal_precision` and
    /// `decimal_scale` give its meaning. Compare/sort/group_by/hash work on
    /// the exact scaled integer. Arithmetic against another Decimal128 (see
    /// arithmetic.cpp, decimal_arith.h) is EXACT int128 math: add/sub
    /// rescale the smaller-scale operand up to match, multiply adds the two
    /// scales, divide extends the dividend's scale by a fixed increment
    /// (DECIMAL_DIVIDE_SCALE_INCREMENT) truncating toward zero. Overflow past
    /// the declared precision, or of the 128-bit container, is a loud
    /// refusal, never a wrapped value. Decimal-against-a-plain-scalar, or
    /// against a Decimal256/other numeric column, still promotes to Float64
    /// and LOSES PRECISION (a double has 53 mantissa bits; a precision-38
    /// decimal needs up to 127) - a bare scalar has no scale of its own to
    /// compute an exact target scale from.
    Decimal128,
    /// 256-bit fixed-point decimal; see Decimal128. Add/sub against another
    /// Decimal256 are exact (256-bit carry-chain arithmetic). Multiply and
    /// divide have no exact path here - a correct result needs a 512-bit
    /// intermediate this pass does not build - so those two ops stay on the
    /// lossy Float64 path, same as any other Decimal256 arithmetic.
    Decimal256,
    /// Fixed-width byte string; `DataType::fixed_size` is the width in bytes.
    /// No offsets buffer. Equality/comparison/sort/group_by/hash are exact
    /// byte comparisons; it carries no numeric or text interpretation, so no
    /// arithmetic or string kernel applies.
    FixedSizeBinary,
    /// String with 64-bit offsets, for payloads too large for String's
    /// 32-bit offsets. Its own physical layout, not String's: every string,
    /// compare, sort, group_by, and Arrow import/export kernel reads and
    /// writes its offsets at their native 64-bit width, zero-copy, with no
    /// size ceiling. A column constructed elsewhere (not Arrow import) is
    /// always String, never LargeString; only Arrow import produces one, and
    /// only from a genuine large_utf8 array.
    LargeString,
    /// Binary with 64-bit offsets. Its own physical layout, not Binary's.
    /// Same kernel and construction coverage as LargeString.
    LargeBinary,
    /// List with 64-bit offsets. Its own physical layout, not List's. Same
    /// kernel and construction coverage as LargeString; a derived column
    /// built by a kernel (e.g. group_by's key column, dictionary_encode's
    /// dictionary) may still narrow to the 32-bit-offset type when its own
    /// output is bounded - that is a fresh sizing choice for new data, not a
    /// loss of the source column's width.
    LargeList,
    /// Fixed-count list: `DataType::fixed_size` elements of the single entry
    /// in `DataType::fields` per row. No offsets buffer.
    FixedSizeList,
    /// Sorted key/value pairs per row, stored as List<Struct{key, value}>
    /// (DataType::physical_type()): `DataType::fields` holds one "entries"
    /// field whose type is that Struct.
    Map,
};

struct Field;

/// Second/Milli/Micro/Nano granularity for Timestamp, Time32, Time64, and
/// Duration.
enum class TimeUnit : std::int32_t {
    Second,
    Milli,
    Micro,
    Nano,
};

/// A column's full type. `id` is the tag; `fields` carries the nested
/// structure - exactly one entry for List/LargeList/FixedSizeList (the
/// element) and for Map (the Struct{key,value} entry type), one per field for
/// Struct, and none for a scalar. The remaining members are parameters that
/// are meaningful only for the `id` values named on each: default-valued and
/// ignored otherwise.
struct DataType {
    TypeId id = TypeId::Unknown;
    std::vector<Field> fields;
    /// Timestamp, Time32, Time64, Duration.
    TimeUnit time_unit = TimeUnit::Micro;
    /// Timestamp only; empty means no timezone (a naive timestamp).
    std::string timezone;
    /// Decimal128, Decimal256.
    std::int32_t decimal_precision = 0;
    std::int32_t decimal_scale = 0;
    /// FixedSizeBinary (byte width), FixedSizeList (element count).
    std::int32_t fixed_size = 0;

    bool operator==(const DataType& other) const;
    bool operator!=(const DataType& other) const { return !(*this == other); }
};

/// A named member of a nested type: a Struct field, or a List's element.
struct Field {
    std::string name;
    DataType type;
    bool nullable = true;

    bool operator==(const Field& other) const {
        return name == other.name && nullable == other.nullable &&
               type == other.type;
    }
    bool operator!=(const Field& other) const { return !(*this == other); }
};

inline bool DataType::operator==(const DataType& other) const {
    return id == other.id && fields == other.fields &&
           time_unit == other.time_unit && timezone == other.timezone &&
           decimal_precision == other.decimal_precision &&
           decimal_scale == other.decimal_scale &&
           fixed_size == other.fixed_size;
}

/// A scalar DataType with no nested fields.
inline DataType scalar(TypeId id) noexcept {
    DataType dt;
    dt.id = id;
    return dt;
}

/// A List<item_type> DataType, with the element named `item_name`.
inline DataType list_of(DataType item_type, std::string item_name = "item") {
    DataType dt;
    dt.id = TypeId::List;
    dt.fields.push_back(
        Field{std::move(item_name), std::move(item_type), true});
    return dt;
}

/// A LargeList<item_type> DataType, with the element named `item_name`.
inline DataType large_list_of(DataType item_type,
                              std::string item_name = "item") {
    DataType dt;
    dt.id = TypeId::LargeList;
    dt.fields.push_back(
        Field{std::move(item_name), std::move(item_type), true});
    return dt;
}

/// A FixedSizeList<item_type> DataType of `size` elements per row.
inline DataType fixed_size_list_of(DataType item_type, std::int32_t size,
                                   std::string item_name = "item") {
    DataType dt;
    dt.id = TypeId::FixedSizeList;
    dt.fixed_size = size;
    dt.fields.push_back(
        Field{std::move(item_name), std::move(item_type), true});
    return dt;
}

/// A Struct DataType with the given fields, in order.
inline DataType struct_of(std::vector<Field> fields) {
    DataType dt;
    dt.id = TypeId::Struct;
    dt.fields = std::move(fields);
    return dt;
}

/// A Map<key_type, value_type> DataType, physically List<Struct{key, value}>.
/// The single field in `fields` (named "item", matching List's convention) is
/// that Struct type. Every field built here is marked nullable: Series::
/// data_type() does not track per-field nullability through the C ABI (it
/// always reports true), so a non-nullable field could never round-trip equal.
inline DataType map_of(DataType key_type, DataType value_type) {
    DataType entry = struct_of({Field{"key", std::move(key_type), true},
                                Field{"value", std::move(value_type), true}});
    DataType dt;
    dt.id = TypeId::Map;
    dt.fields.push_back(Field{"item", std::move(entry), true});
    return dt;
}

/// A Timestamp DataType at `unit` granularity, optionally with `tz`.
inline DataType timestamp(TimeUnit unit, std::string tz = "") {
    DataType dt;
    dt.id = TypeId::Timestamp;
    dt.time_unit = unit;
    dt.timezone = std::move(tz);
    return dt;
}

/// A Time32/Time64 DataType at `unit` granularity (Second/Milli use Time32's
/// Int32 storage, Micro/Nano use Time64's Int64 storage).
inline DataType time_of(TimeUnit unit) {
    DataType dt;
    dt.id = (unit == TimeUnit::Second || unit == TimeUnit::Milli)
                ? TypeId::Time32
                : TypeId::Time64;
    dt.time_unit = unit;
    return dt;
}

/// A Duration DataType at `unit` granularity.
inline DataType duration_of(TimeUnit unit) {
    DataType dt;
    dt.id = TypeId::Duration;
    dt.time_unit = unit;
    return dt;
}

/// A Decimal128 DataType with the given precision and scale.
inline DataType decimal128(std::int32_t precision, std::int32_t scale) {
    DataType dt;
    dt.id = TypeId::Decimal128;
    dt.decimal_precision = precision;
    dt.decimal_scale = scale;
    return dt;
}

/// A Decimal256 DataType with the given precision and scale.
inline DataType decimal256(std::int32_t precision, std::int32_t scale) {
    DataType dt;
    dt.id = TypeId::Decimal256;
    dt.decimal_precision = precision;
    dt.decimal_scale = scale;
    return dt;
}

/// A FixedSizeBinary DataType of `size` bytes per row.
inline DataType fixed_size_binary(std::int32_t size) {
    DataType dt;
    dt.id = TypeId::FixedSizeBinary;
    dt.fixed_size = size;
    return dt;
}

/// True for a type usable in numeric arithmetic/reduction after the
/// promotions in `dataframe/internal/type_promotion.h` are applied: Float16
/// promotes to Float32 (exact - every half value has a precise float32
/// representation) and Decimal128/Decimal256 promote to Float64 (lossy for
/// values that need exact decimal math, e.g. money - compare/sort/group_by/
/// hash instead work on the exact scaled integer and do not use this).
/// Bool/String/Binary/List/Struct and every other logical type (temporal,
/// FixedSizeBinary, Large*, FixedSizeList, Map) are excluded: they have no
/// numeric interpretation, or none this engine defines yet.
constexpr bool is_arithmetic_type(TypeId t) noexcept {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float32:
        case TypeId::Float64:
        case TypeId::Float16:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
            return true;
        case TypeId::Unknown:
        case TypeId::Bool:
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::List:
        case TypeId::Struct:
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
        case TypeId::FixedSizeBinary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Map:
            return false;
    }
    return false;
}

/// True for the exact set DF_NUMERIC_DISPATCH (internal/numeric_dispatch.h)
/// has a case for: Int8-64, Uint8-64, Float32, Float64. A kernel that
/// dispatches on physical_type(t) checks this first, so an opaque or
/// unmapped type (Bool, Decimal128/256, FixedSizeBinary, ...) is refused
/// instead of silently reaching the macro's default no-op.
constexpr bool is_numeric_dispatchable(TypeId t) noexcept {
    switch (t) {
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float32:
        case TypeId::Float64:
            return true;
        case TypeId::Unknown:
        case TypeId::Bool:
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::List:
        case TypeId::Struct:
        case TypeId::Float16:
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
        case TypeId::FixedSizeBinary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Map:
            return false;
    }
    return false;
}

/// True for the temporal family (Date32/64, Time32/64, Timestamp, Duration):
/// physical_type() maps each to Int32 or Int64, so compare/sort/min/max/
/// group_by/hash work on the exact value via that mapping. Arithmetic among
/// Timestamp/Duration/Time64 follows the rule table in temporal_arith.h (see
/// Timestamp's and Duration's doc comments above); Date32/Date64/Time32
/// arithmetic stays refused (see arithmetic.cpp).
constexpr bool is_temporal_type(TypeId t) noexcept {
    switch (t) {
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            return true;
        case TypeId::Unknown:
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float32:
        case TypeId::Float64:
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::List:
        case TypeId::Struct:
        case TypeId::Float16:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
        case TypeId::FixedSizeBinary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Map:
            return false;
    }
    return false;
}

/// How a column's rows compare, hash and count as values. Numeric: one number
/// per row, read by read_f64. Bytes: an opaque byte string per row, compared
/// by memcmp (read_bytes); Decimal128/256 belong here because two distinct
/// decimals can round to the same double. None: a nested type with no per-row
/// value, which a kernel must refuse rather than read as a zero.
enum class ValueDomain : std::int32_t {
    None,
    Numeric,
    Bytes,
};

/// The comparison and hashing domain of `t`.
constexpr ValueDomain value_domain(TypeId t) noexcept {
    switch (t) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float16:
        case TypeId::Float32:
        case TypeId::Float64:
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            return ValueDomain::Numeric;
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::FixedSizeBinary:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
            return ValueDomain::Bytes;
        case TypeId::List:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Struct:
        case TypeId::Map:
        case TypeId::Unknown:
            return ValueDomain::None;
    }
    return ValueDomain::None;
}

/// True if `t` has a total order the sort and compare kernels implement.
constexpr bool is_orderable_type(TypeId t) noexcept {
    return value_domain(t) != ValueDomain::None;
}

/// The TypeId whose buffer layout `t` shares, for kernel dispatch: a logical
/// type maps to the physical type its data is actually stored as (e.g.
/// Timestamp -> Int64, Map -> List); every other type maps to itself.
constexpr TypeId physical_type(TypeId t) noexcept {
    switch (t) {
        case TypeId::Date32:
        case TypeId::Time32:
            return TypeId::Int32;
        case TypeId::Date64:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            return TypeId::Int64;
        case TypeId::Map:
            return TypeId::List;
        case TypeId::Unknown:
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float32:
        case TypeId::Float64:
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::List:
        case TypeId::Struct:
        case TypeId::Float16:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
        case TypeId::FixedSizeBinary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
            return t;
    }
    return t;
}

/// The 32-bit-offset TypeId sharing `t`'s buffer shape: LargeString ->
/// String, LargeBinary -> Binary, LargeList -> List; every other type maps to
/// itself. A String/Binary/List-kind kernel dispatches on
/// `narrow_varwidth_type(t)` so it does not need a separate case for the
/// corresponding Large type; `is_wide_offset_type(t)` then says which offsets
/// buffer (32-bit or 64-bit) actually backs the column.
constexpr TypeId narrow_varwidth_type(TypeId t) noexcept {
    switch (t) {
        case TypeId::LargeString:
            return TypeId::String;
        case TypeId::LargeBinary:
            return TypeId::Binary;
        case TypeId::LargeList:
            return TypeId::List;
        default:
            return t;
    }
}

/// True if `t` stores its offsets in the 64-bit `offsets64` buffer
/// (LargeString/LargeBinary/LargeList); false if it uses the 32-bit
/// `offsets` buffer (String/Binary/List) or has no offsets buffer at all.
constexpr bool is_wide_offset_type(TypeId t) noexcept {
    return t == TypeId::LargeString || t == TypeId::LargeBinary ||
           t == TypeId::LargeList;
}

/// FLAT: values contiguous. CONSTANT: one value, logical length N. DICTIONARY:
/// value[i] = base[codes[i]]. SELECTION: value[i] = base[sel[i]], a view over a
/// base with no data movement.
enum class Encoding : std::int32_t {
    Flat,
    Constant,
    Dictionary,
    Selection,
};

/// C++ mirror of the C ABI op-code enums (same values). Kernels cast the raw
/// int32 code from the ABI to these so their dispatch switches are exhaustive
/// and -Wswitch flags a missing case.

/// Mirrors dftu_cmp_op.
enum class CmpOp : std::int32_t {
    Gt = 0,
    Ge = 1,
    Lt = 2,
    Le = 3,
    Eq = 4,
    Ne = 5,
};

/// Mirrors dftu_logical_op.
enum class LogicalOp : std::int32_t {
    And = 0,
    Or = 1,
};

/// Mirrors dftu_rank_method.
enum class RankMethod : std::int32_t {
    Average = 0,
    Min = 1,
    Dense = 2,
    Ordinal = 3,
    Max = 4,
};

/// Mirrors dftu_groupwise_op: the group-wise transforms (one value per input
/// row, in input order) a GroupBy / LazyGroupBy runs.
enum class GroupwiseOp : std::int32_t {
    CumSum = 0,
    CumMax = 1,
    CumMin = 2,
    CumCount = 3,
    Shift = 4,
    Diff = 5,
    PctChange = 6,
    Rank = 7,
    NGroup = 8,
    Head = 9,
    Tail = 10,
    Nth = 11,
    FFill = 12,
    BFill = 13,
    RollingSum = 14,
    RollingMean = 15,
    RollingMin = 16,
    RollingMax = 17,
    CumProd = 18,
};

/// Mirrors dftu_rolling_op.
enum class RollingOp : std::int32_t {
    Sum = 0,
    Mean = 1,
    Min = 2,
    Max = 3,
};

/// Mirrors dftu_str_pred_op: a string predicate against one literal pattern,
/// yielding Bool. Like is SQL LIKE (`%` / `_`, `\\` escapes); Matches is a
/// whole-string ECMAScript regex match, Search the same regex anywhere in the
/// string.
enum class StrPredOp : std::int32_t {
    Contains = 0,
    StartsWith = 1,
    EndsWith = 2,
    Like = 3,
    Matches = 4,
    Search = 5,
};

/// Mirrors dftu_str_map_op: a String -> String map with no argument.
enum class StrMapOp : std::int32_t {
    Lower = 0,
    Upper = 1,
    Strip = 2,
    Lstrip = 3,
    Rstrip = 4,
};

/// Mirrors dftu_join_how. Which rows a join keeps: Inner keeps matched pairs;
/// Left/Right also keep the unmatched rows of that side (the other side
/// null-filled); Outer keeps both; Semi/Anti keep the left rows with / without
/// a match, left columns only; Cross pairs every left row with every right row.
enum class JoinHow : std::int32_t {
    Inner = 0,
    Left = 1,
    Right = 2,
    Outer = 3,
    Semi = 4,
    Anti = 5,
    Cross = 6,
};

/// Mirrors the dftu_series_prim op codes: a unary numeric primitive over a FLAT
/// Int64/Uint64 column, producing an Int64 column.
enum class PrimOp : std::int32_t {
    Ilog2 = 0,
    BitWidth = 1,
    Popcount = 2,
    Clz = 3,
    Ctz = 4,
    Mix64 = 5,
};

/// Unary numeric op codes for the expression engine (elementwise, one column
/// operand). Log/Sqrt/Exp widen to Float64; Abs/Round/Floor/Ceil/Trunc/Sign/
/// Negate keep the input's numeric type; IsNan/IsFinite/IsInfinite yield Bool.
enum class UnaryOp : std::int32_t {
    Abs = 0,
    Round = 1,
    Floor = 2,
    Ceil = 3,
    Log = 4,
    Sqrt = 5,
    Exp = 6,
    Sign = 7,
    Negate = 8,
    Trunc = 9,
    IsNan = 10,
    IsFinite = 11,
    IsInfinite = 12,
};

/// Mirrors dftu_scalar_tag: the domain a dftu_scalar carries.
enum class ScalarTag : std::int32_t {
    I64 = 0,
    U64 = 1,
    F64 = 2,
};

/// Bytes to hold `n` values of `t` in Arrow layout. Bool is bit-packed
/// (ceil(n/8)); other fixed-width types are n * byte_width; String/Binary
/// (variable-width) return 0 - their sizing is offset-driven, not by this.
/// FixedSizeBinary, whose width is not a per-TypeId constant, also returns 0
/// here; use the DataType or (TypeId, fixed_size) overload for it.
constexpr std::size_t buffer_bytes(TypeId t, std::int64_t n) noexcept;

/// Byte width of a fixed-width type. std::nullopt for a variable-width type
/// (String/Binary/List/Struct/Map, their Large* counterparts) and for a type
/// whose width is a DataType parameter rather than a per-TypeId constant
/// (FixedSizeBinary, FixedSizeList): a TypeId alone cannot answer for those,
/// and returning 0 for "cannot answer" is indistinguishable from 0 meaning
/// "not fixed width", which is how this used to silently mis-size buffers and
/// mis-key rows for FixedSizeBinary. Callers holding a DataType or a
/// dftu_series/Series (has `fixed_size`) should use one of the overloads
/// below instead, which can always answer for FixedSizeBinary.
constexpr std::optional<std::size_t> byte_width(TypeId t) noexcept {
    switch (t) {
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Uint8:
            return 1;
        case TypeId::Int16:
        case TypeId::Uint16:
        case TypeId::Float16:
            return 2;
        case TypeId::Int32:
        case TypeId::Uint32:
        case TypeId::Float32:
        case TypeId::Date32:
        case TypeId::Time32:
            return 4;
        case TypeId::Int64:
        case TypeId::Uint64:
        case TypeId::Float64:
        case TypeId::Date64:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            return 8;
        case TypeId::Decimal128:
            return 16;
        case TypeId::Decimal256:
            return 32;
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::List:
        case TypeId::Struct:
        case TypeId::FixedSizeBinary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Map:
        case TypeId::Unknown:
            return std::nullopt;
    }
    return std::nullopt;
}

/// Byte width of `t`, given its own `fixed_size` DataType parameter. Answers
/// correctly for FixedSizeBinary (width = `fixed_size`); every other type
/// ignores `fixed_size` and defers to the TypeId-only overload. The shape
/// most callers holding a dftu_series/Series/DataType want, since all three
/// carry a `fixed_size` field alongside the type.
constexpr std::optional<std::size_t> byte_width(
    TypeId t, std::int32_t fixed_size) noexcept {
    if (t == TypeId::FixedSizeBinary)
        return fixed_size >= 0 ? std::optional<std::size_t>(
                                     static_cast<std::size_t>(fixed_size))
                               : std::nullopt;
    return byte_width(t);
}

/// Byte width of `dt`, resolving FixedSizeBinary via `dt.fixed_size`.
constexpr std::optional<std::size_t> byte_width(const DataType& dt) noexcept {
    return byte_width(dt.id, dt.fixed_size);
}

constexpr std::size_t buffer_bytes(TypeId t, std::int64_t n) noexcept {
    if (t == TypeId::Bool) return static_cast<std::size_t>((n + 7) / 8);
    return static_cast<std::size_t>(n) * byte_width(t).value_or(0);
}

/// The lowercase dtype name (e.g. "int64", "float64"), for diagnostics.
constexpr const char* type_name(TypeId t) noexcept {
    switch (t) {
        case TypeId::Bool:
            return "bool";
        case TypeId::Int8:
            return "int8";
        case TypeId::Int16:
            return "int16";
        case TypeId::Int32:
            return "int32";
        case TypeId::Int64:
            return "int64";
        case TypeId::Uint8:
            return "uint8";
        case TypeId::Uint16:
            return "uint16";
        case TypeId::Uint32:
            return "uint32";
        case TypeId::Uint64:
            return "uint64";
        case TypeId::Float32:
            return "float32";
        case TypeId::Float64:
            return "float64";
        case TypeId::String:
            return "string";
        case TypeId::Binary:
            return "binary";
        case TypeId::List:
            return "list";
        case TypeId::Struct:
            return "struct";
        case TypeId::Float16:
            return "float16";
        case TypeId::Date32:
            return "date32";
        case TypeId::Date64:
            return "date64";
        case TypeId::Time32:
            return "time32";
        case TypeId::Time64:
            return "time64";
        case TypeId::Timestamp:
            return "timestamp";
        case TypeId::Duration:
            return "duration";
        case TypeId::Decimal128:
            return "decimal128";
        case TypeId::Decimal256:
            return "decimal256";
        case TypeId::FixedSizeBinary:
            return "fixed_size_binary";
        case TypeId::LargeString:
            return "large_string";
        case TypeId::LargeBinary:
            return "large_binary";
        case TypeId::LargeList:
            return "large_list";
        case TypeId::FixedSizeList:
            return "fixed_size_list";
        case TypeId::Map:
            return "map";
        case TypeId::Unknown:
            return "unknown";
    }
    return "unknown";
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_TYPES_H
