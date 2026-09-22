#include <dftracer/utils/utilities/indexer/internal/column_type_codec.h>

#include <cstdint>

namespace dftracer::utils::utilities::indexer::internal {

namespace {

namespace df = dftracer::utils::dataframe;

constexpr std::size_t MAX_FIELDS = 1u << 16;
constexpr std::size_t MAX_NAME_LEN = 1u << 20;
constexpr int MAX_NEST_DEPTH = 64;
constexpr auto MAX_TYPE_ID = static_cast<std::uint8_t>(df::TypeId::Map);
constexpr auto MAX_TIME_UNIT = static_cast<std::uint8_t>(df::TimeUnit::Nano);

void append_varint(std::string& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

std::uint64_t zigzag_encode(std::int64_t v) {
    return (static_cast<std::uint64_t>(v) << 1) ^
           static_cast<std::uint64_t>(v >> 63);
}

std::int64_t zigzag_decode(std::uint64_t v) {
    return static_cast<std::int64_t>(v >> 1) ^
           -static_cast<std::int64_t>(v & 1);
}

void append_svarint(std::string& out, std::int32_t value) {
    append_varint(out, zigzag_encode(value));
}

/// Bounds-checked cursor over the encoded bytes; every read fails closed
/// (returns false, leaves the cursor unchanged) rather than reading past the
/// end, so a truncated or corrupt value can never cause an out-of-bounds
/// access.
class Reader {
   public:
    explicit Reader(std::string_view data) : data_(data) {}

    bool read_u8(std::uint8_t& out) {
        if (pos_ + 1 > data_.size()) return false;
        out = static_cast<std::uint8_t>(data_[pos_]);
        pos_ += 1;
        return true;
    }

    bool read_varint(std::uint64_t& out) {
        std::uint64_t result = 0;
        for (int shift = 0; shift <= 63; shift += 7) {
            std::uint8_t byte = 0;
            if (!read_u8(byte)) return false;
            result |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if (!(byte & 0x80)) {
                out = result;
                return true;
            }
        }
        return false;  // varint too long: malformed
    }

    bool read_svarint(std::int32_t& out) {
        std::uint64_t raw = 0;
        if (!read_varint(raw)) return false;
        out = static_cast<std::int32_t>(zigzag_decode(raw));
        return true;
    }

    bool read_bytes(std::size_t n, std::string_view& out) {
        if (pos_ + n > data_.size()) return false;
        out = data_.substr(pos_, n);
        pos_ += n;
        return true;
    }

    bool at_end() const { return pos_ == data_.size(); }

   private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

void encode_type(const df::DataType& type, std::string& out) {
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(type.id)));
    switch (type.id) {
        case df::TypeId::Timestamp:
            out.push_back(
                static_cast<char>(static_cast<std::uint8_t>(type.time_unit)));
            append_varint(out, type.timezone.size());
            out.append(type.timezone);
            break;
        case df::TypeId::Time32:
        case df::TypeId::Time64:
        case df::TypeId::Duration:
            out.push_back(
                static_cast<char>(static_cast<std::uint8_t>(type.time_unit)));
            break;
        case df::TypeId::Decimal128:
        case df::TypeId::Decimal256:
            append_svarint(out, type.decimal_precision);
            append_svarint(out, type.decimal_scale);
            break;
        case df::TypeId::FixedSizeBinary:
        case df::TypeId::FixedSizeList:
            append_svarint(out, type.fixed_size);
            break;
        default:
            break;
    }
    append_varint(out, type.fields.size());
    for (const auto& field : type.fields) {
        append_varint(out, field.name.size());
        out.append(field.name);
        out.push_back(static_cast<char>(field.nullable ? 1 : 0));
        encode_type(field.type, out);
    }
}

/// Decodes one type at `depth` nesting levels below the top. Fails closed
/// (returns false) on any malformed byte, an unknown type id, an absurd
/// field/name count, or nesting deeper than MAX_NEST_DEPTH (a corrupt or
/// adversarial length prefix must not blow the stack via unbounded
/// recursion).
bool decode_type(Reader& r, int depth, df::DataType& out) {
    if (depth > MAX_NEST_DEPTH) return false;

    std::uint8_t raw_id = 0;
    if (!r.read_u8(raw_id)) return false;
    if (raw_id > MAX_TYPE_ID) return false;
    out.id = static_cast<df::TypeId>(raw_id);

    switch (out.id) {
        case df::TypeId::Timestamp: {
            std::uint8_t unit = 0;
            if (!r.read_u8(unit) || unit > MAX_TIME_UNIT) return false;
            out.time_unit = static_cast<df::TimeUnit>(unit);
            std::uint64_t tz_len = 0;
            if (!r.read_varint(tz_len) || tz_len > MAX_NAME_LEN) return false;
            std::string_view tz;
            if (!r.read_bytes(static_cast<std::size_t>(tz_len), tz))
                return false;
            out.timezone.assign(tz);
            break;
        }
        case df::TypeId::Time32:
        case df::TypeId::Time64:
        case df::TypeId::Duration: {
            std::uint8_t unit = 0;
            if (!r.read_u8(unit) || unit > MAX_TIME_UNIT) return false;
            out.time_unit = static_cast<df::TimeUnit>(unit);
            break;
        }
        case df::TypeId::Decimal128:
        case df::TypeId::Decimal256:
            if (!r.read_svarint(out.decimal_precision)) return false;
            if (!r.read_svarint(out.decimal_scale)) return false;
            break;
        case df::TypeId::FixedSizeBinary:
        case df::TypeId::FixedSizeList:
            if (!r.read_svarint(out.fixed_size)) return false;
            break;
        default:
            break;
    }

    std::uint64_t n_fields = 0;
    if (!r.read_varint(n_fields) || n_fields > MAX_FIELDS) return false;
    out.fields.clear();
    out.fields.reserve(static_cast<std::size_t>(n_fields));
    for (std::uint64_t i = 0; i < n_fields; ++i) {
        std::uint64_t name_len = 0;
        if (!r.read_varint(name_len) || name_len > MAX_NAME_LEN) return false;
        std::string_view name;
        if (!r.read_bytes(static_cast<std::size_t>(name_len), name))
            return false;
        std::uint8_t nullable = 0;
        if (!r.read_u8(nullable)) return false;

        df::Field field;
        field.name.assign(name);
        field.nullable = nullable != 0;
        if (!decode_type(r, depth + 1, field.type)) return false;
        out.fields.push_back(std::move(field));
    }
    return true;
}

}  // namespace

std::string encode_data_type(const df::DataType& type) {
    std::string out;
    encode_type(type, out);
    return out;
}

std::optional<df::DataType> decode_data_type(std::string_view bytes) {
    Reader r(bytes);
    df::DataType out;
    if (!decode_type(r, 0, out)) return std::nullopt;
    if (!r.at_end()) return std::nullopt;  // trailing garbage: malformed
    return out;
}

df::DataType column_type_to_data_type(ColumnType type) {
    switch (type) {
        case ColumnType::Int64:
            return df::scalar(df::TypeId::Int64);
        case ColumnType::Float64:
            return df::scalar(df::TypeId::Float64);
        case ColumnType::String:
            return df::scalar(df::TypeId::String);
        case ColumnType::Unknown:
            return df::scalar(df::TypeId::Unknown);
    }
    return df::scalar(df::TypeId::Unknown);
}

ColumnType data_type_to_column_type(const df::DataType& type) {
    switch (type.id) {
        case df::TypeId::Unknown:
            return ColumnType::Unknown;
        case df::TypeId::Int64:
            return ColumnType::Int64;
        case df::TypeId::Float64:
            return ColumnType::Float64;
        default:
            return ColumnType::String;
    }
}

df::DataType merge_data_type(const df::DataType& a, const df::DataType& b) {
    if (a.id == df::TypeId::Unknown) return b;
    if (b.id == df::TypeId::Unknown) return a;
    if (a == b) return a;
    if ((a.id == df::TypeId::Int64 && b.id == df::TypeId::Float64) ||
        (a.id == df::TypeId::Float64 && b.id == df::TypeId::Int64))
        return df::scalar(df::TypeId::Float64);
    return df::scalar(df::TypeId::String);
}

}  // namespace dftracer::utils::utilities::indexer::internal
