#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COLUMN_TYPE_CODEC_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COLUMN_TYPE_CODEC_H

#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::indexer::internal {

/// Recursive TLV encoding of a dataframe::DataType, own format (not Arrow
/// IPC): the index is core and must be readable without the optional Arrow
/// IPC build flag.
///
///   type  := u8 type_id, [params by id], varint n_fields, n_fields * field
///   field := varint name_len, name bytes, u8 nullable, type
///   params: Timestamp -> u8 time_unit, varint tz_len, tz bytes
///           Time32/Time64/Duration -> u8 time_unit
///           Decimal128/Decimal256 -> varint precision, varint scale
///           FixedSizeBinary/FixedSizeList -> varint size
///
/// Integers are unsigned LEB128 varints; precision/scale/size use the zigzag
/// variant so a negative value round-trips.
std::string encode_data_type(const dataframe::DataType& type);

/// Decodes bytes produced by encode_data_type. Returns nullopt for truncated,
/// malformed (unknown type id, absurd field count), or trailing-byte input;
/// never reads past the end of `bytes`.
std::optional<dataframe::DataType> decode_data_type(std::string_view bytes);

/// The DataType a harvested ColumnType observation maps to for storage.
dataframe::DataType column_type_to_data_type(ColumnType type);

/// The ColumnType a stored DataType folds back to, for callers still on the
/// ColumnType surface. Any nested (List/Struct/...) shape reports String,
/// matching merge_data_type's conflict rule.
ColumnType data_type_to_column_type(const dataframe::DataType& type);

/// Folds two observations of the same column's stored type into one:
/// Unknown yields to the other, Int64+Float64 widen to Float64, identical
/// types pass through unchanged, and any other mismatch (including two
/// different nested shapes) widens to a scalar String - there is no
/// meaningful common type, and every JSON scalar has a lossless string
/// representation. Commutative and associative, so it folds across files.
dataframe::DataType merge_data_type(const dataframe::DataType& a,
                                    const dataframe::DataType& b);

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COLUMN_TYPE_CODEC_H
