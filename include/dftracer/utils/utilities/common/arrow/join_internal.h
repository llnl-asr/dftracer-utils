#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_JOIN_INTERNAL_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_JOIN_INTERNAL_H

#include <dftracer/utils/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <optional>
#include <string_view>
#include <vector>

/// Shared sort/compare + value-passthrough helpers backing the Arrow relational
/// ops (join, asof_join, interval_join, window). Internal to the arrow layer;
/// not part of the public API.
namespace dftracer::utils::utilities::common::arrow {

inline std::optional<ColumnType> scalar_type_from_storage(
    ArrowType t) noexcept {
    switch (t) {
        case NANOARROW_TYPE_INT64:
            return ColumnType::Int64;
        case NANOARROW_TYPE_INT32:
            return ColumnType::Int32;
        case NANOARROW_TYPE_INT16:
            return ColumnType::Int16;
        case NANOARROW_TYPE_INT8:
            return ColumnType::Int8;
        case NANOARROW_TYPE_UINT64:
            return ColumnType::Uint64;
        case NANOARROW_TYPE_UINT32:
            return ColumnType::Uint32;
        case NANOARROW_TYPE_UINT16:
            return ColumnType::Uint16;
        case NANOARROW_TYPE_UINT8:
            return ColumnType::Uint8;
        case NANOARROW_TYPE_DOUBLE:
            return ColumnType::Double;
        case NANOARROW_TYPE_FLOAT:
            return ColumnType::Float32;
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING:
            return ColumnType::String;
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY:
            return ColumnType::Binary;
        case NANOARROW_TYPE_BOOL:
            return ColumnType::Bool;
        default:
            return std::nullopt;
    }
}

enum class KeyKind { Signed, Unsigned, Float, Bytes };

inline std::optional<KeyKind> key_kind_from_storage(ArrowType t) noexcept {
    switch (t) {
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_BOOL:
            return KeyKind::Signed;
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64:
            return KeyKind::Unsigned;
        case NANOARROW_TYPE_FLOAT:
        case NANOARROW_TYPE_DOUBLE:
            return KeyKind::Float;
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING:
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY:
            return KeyKind::Bytes;
        default:
            return std::nullopt;
    }
}

/// field_types holds the inner struct field types for STRUCT_LIST; empty for
/// scalars and scalar lists.
struct ValueCol {
    const ArrowArrayView* view;
    ColumnType type;
    std::vector<ColumnType> field_types;
};

/// Supports scalars, list<struct<...>>, list<utf8/binary>, and list of any
/// integer-family scalar (int8/16/32/64, uint8/16/32/64, bool); an integer-list
/// element normalizes to list<int64> since the builder has no other list-of-
/// scalar element type. A floating-point list element is unsupported and
/// throws.
inline ValueCol plan_value_col(const ArrowArrayView* v, const ArrowSchema* s,
                               const char* name, ColumnSpec& spec) {
    ValueCol vc;
    vc.view = v;
    if (auto st = scalar_type_from_storage(v->storage_type)) {
        vc.type = *st;
        spec = {name ? name : "", *st};
        return vc;
    }
    if (v->storage_type == NANOARROW_TYPE_LIST ||
        v->storage_type == NANOARROW_TYPE_LARGE_LIST) {
        const ArrowArrayView* elem = v->children[0];
        if (elem->storage_type == NANOARROW_TYPE_STRUCT) {
            const ArrowSchema* item_schema = s->children[0];
            std::vector<ColumnSpec> fields;
            fields.reserve(static_cast<std::size_t>(elem->n_children));
            for (std::int64_t f = 0; f < elem->n_children; ++f) {
                auto ft =
                    scalar_type_from_storage(elem->children[f]->storage_type);
                if (!ft) {
                    throw DFTUtilsException(
                        ErrorCode::InvalidArgument,
                        "arrow: unsupported struct-list field type");
                }
                vc.field_types.push_back(*ft);
                const char* fname = item_schema->children[f]->name;
                fields.push_back({fname ? fname : "", *ft});
            }
            vc.type = ColumnType::StructList;
            spec = {name ? name : "", ColumnType::StructList,
                    std::move(fields)};
            return vc;
        }
        if (auto ek = key_kind_from_storage(elem->storage_type)) {
            if (*ek == KeyKind::Bytes) {
                vc.type = ColumnType::StringList;
                spec = {name ? name : "", ColumnType::StringList};
                return vc;
            }
            if (*ek == KeyKind::Signed || *ek == KeyKind::Unsigned) {
                vc.type = ColumnType::Int64List;
                spec = {name ? name : "", ColumnType::Int64List};
                return vc;
            }
        }
        throw DFTUtilsException(ErrorCode::InvalidArgument,
                                "arrow: unsupported list element type");
    }
    throw DFTUtilsException(ErrorCode::InvalidArgument,
                            "arrow: unsupported value column type");
}

inline void append_scalar(RecordBatchBuilder& b, std::size_t out, ColumnType ct,
                          const ArrowArrayView* v, std::int64_t pos) {
    switch (ct) {
        case ColumnType::Int64:
        case ColumnType::Int32:
        case ColumnType::Int16:
        case ColumnType::Int8:
            b.append_int64(out, ArrowArrayViewGetIntUnsafe(v, pos));
            break;
        case ColumnType::Uint64:
        case ColumnType::Uint32:
        case ColumnType::Uint16:
        case ColumnType::Uint8:
            b.append_uint64(out, ArrowArrayViewGetUIntUnsafe(v, pos));
            break;
        case ColumnType::Double:
        case ColumnType::Float32:
            b.append_double(out, ArrowArrayViewGetDoubleUnsafe(v, pos));
            break;
        case ColumnType::Bool:
            b.append_bool(out, ArrowArrayViewGetIntUnsafe(v, pos) != 0);
            break;
        case ColumnType::String: {
            ArrowStringView s = ArrowArrayViewGetStringUnsafe(v, pos);
            b.append_string(
                out, std::string_view(s.data,
                                      static_cast<std::size_t>(s.size_bytes)));
            break;
        }
        case ColumnType::Binary: {
            ArrowStringView s = ArrowArrayViewGetStringUnsafe(v, pos);
            b.append_binary(
                out, std::string_view(s.data,
                                      static_cast<std::size_t>(s.size_bytes)));
            break;
        }
        default:
            throw DFTUtilsException(ErrorCode::Internal,
                                    "arrow: unhandled scalar column type");
    }
}

inline void append_value(RecordBatchBuilder& b, std::size_t out,
                         const ValueCol& vc, std::int64_t row) {
    if (ArrowArrayViewIsNull(vc.view, row)) {
        b.append_null(out);
        return;
    }
    switch (vc.type) {
        case ColumnType::StringList: {
            const ArrowArrayView* elem = vc.view->children[0];
            const std::int64_t start =
                ArrowArrayViewListChildOffset(vc.view, row);
            const std::int64_t end =
                ArrowArrayViewListChildOffset(vc.view, row + 1);
            std::vector<std::string_view> vals;
            vals.reserve(static_cast<std::size_t>(end - start));
            for (std::int64_t p = start; p < end; ++p) {
                ArrowStringView s = ArrowArrayViewGetStringUnsafe(elem, p);
                vals.emplace_back(s.data,
                                  static_cast<std::size_t>(s.size_bytes));
            }
            b.append_string_list(out, vals);
            break;
        }
        case ColumnType::Int64List: {
            const ArrowArrayView* elem = vc.view->children[0];
            const std::int64_t start =
                ArrowArrayViewListChildOffset(vc.view, row);
            const std::int64_t end =
                ArrowArrayViewListChildOffset(vc.view, row + 1);
            std::vector<std::int64_t> vals;
            vals.reserve(static_cast<std::size_t>(end - start));
            for (std::int64_t p = start; p < end; ++p)
                vals.push_back(ArrowArrayViewGetIntUnsafe(elem, p));
            b.append_int64_list(out, vals);
            break;
        }
        case ColumnType::StructList: {
            const ArrowArrayView* st = vc.view->children[0];
            const std::int64_t start =
                ArrowArrayViewListChildOffset(vc.view, row);
            const std::int64_t end =
                ArrowArrayViewListChildOffset(vc.view, row + 1);
            std::vector<std::vector<StructCell>> rows;
            rows.reserve(static_cast<std::size_t>(end - start));
            for (std::int64_t p = start; p < end; ++p) {
                std::vector<StructCell> cells(vc.field_types.size());
                for (std::size_t f = 0; f < vc.field_types.size(); ++f) {
                    const ArrowArrayView* fv = st->children[f];
                    StructCell& c = cells[f];
                    switch (vc.field_types[f]) {
                        case ColumnType::Double:
                        case ColumnType::Float32:
                            c.f64 = ArrowArrayViewGetDoubleUnsafe(fv, p);
                            break;
                        case ColumnType::Uint64:
                        case ColumnType::Uint32:
                        case ColumnType::Uint16:
                        case ColumnType::Uint8:
                            c.u64 = ArrowArrayViewGetUIntUnsafe(fv, p);
                            break;
                        case ColumnType::String:
                        case ColumnType::Binary: {
                            ArrowStringView s =
                                ArrowArrayViewGetStringUnsafe(fv, p);
                            c.str = std::string_view(
                                s.data, static_cast<std::size_t>(s.size_bytes));
                            break;
                        }
                        default:
                            c.i64 = ArrowArrayViewGetIntUnsafe(fv, p);
                            break;
                    }
                }
                rows.push_back(std::move(cells));
            }
            b.append_struct_list(out, rows);
            break;
        }
        default:
            append_scalar(b, out, vc.type, vc.view, row);
            break;
    }
}

using KeyViews = std::vector<const ArrowArrayView*>;

inline bool row_key_null(const KeyViews& keys, std::int64_t row) {
    for (const ArrowArrayView* v : keys)
        if (ArrowArrayViewIsNull(v, row)) return true;
    return false;
}

/// Compare one non-null cell of a given KeyKind; returns <0, 0, >0.
inline int cell_cmp(const ArrowArrayView* va, std::int64_t ia,
                    const ArrowArrayView* vb, std::int64_t ib, KeyKind k) {
    switch (k) {
        case KeyKind::Signed: {
            std::int64_t a = ArrowArrayViewGetIntUnsafe(va, ia);
            std::int64_t b = ArrowArrayViewGetIntUnsafe(vb, ib);
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        case KeyKind::Unsigned: {
            std::uint64_t a = ArrowArrayViewGetUIntUnsafe(va, ia);
            std::uint64_t b = ArrowArrayViewGetUIntUnsafe(vb, ib);
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        case KeyKind::Float: {
            double a = ArrowArrayViewGetDoubleUnsafe(va, ia);
            double b = ArrowArrayViewGetDoubleUnsafe(vb, ib);
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        default: {  // BYTES
            ArrowStringView a = ArrowArrayViewGetStringUnsafe(va, ia);
            ArrowStringView b = ArrowArrayViewGetStringUnsafe(vb, ib);
            std::int64_t n = std::min(a.size_bytes, b.size_bytes);
            int c =
                n > 0 ? std::memcmp(a.data, b.data, static_cast<std::size_t>(n))
                      : 0;
            if (c != 0) return c < 0 ? -1 : 1;
            if (a.size_bytes != b.size_bytes)
                return a.size_bytes < b.size_bytes ? -1 : 1;
            return 0;
        }
    }
}

/// Compare two non-null key tuples; returns <0, 0, >0. Both sides share the
/// same per-column KeyKind (validated by the caller).
inline int key_cmp(const KeyViews& ka, std::int64_t ia, const KeyViews& kb,
                   std::int64_t ib, const std::vector<KeyKind>& kinds) {
    for (std::size_t k = 0; k < kinds.size(); ++k) {
        int c = cell_cmp(ka[k], ia, kb[k], ib, kinds[k]);
        if (c != 0) return c;
    }
    return 0;
}

/// Row indices [0, n) ordered by the equi-key tuple (a null-keyed tuple sorts
/// last), then an optional numeric `secondary` column (null sorts last), then
/// the original row index for a stable, deterministic tie-break. Pass secondary
/// = nullptr to sort by the equi tuple alone.
inline std::vector<std::int64_t> order_rows(const KeyViews& equi,
                                            const std::vector<KeyKind>& ekinds,
                                            const ArrowArrayView* secondary,
                                            KeyKind secondary_kind,
                                            std::int64_t n) {
    std::vector<std::int64_t> order(static_cast<std::size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::int64_t a, std::int64_t bb) {
        bool ea = row_key_null(equi, a);
        bool eb = row_key_null(equi, bb);
        if (ea != eb) return !ea;
        if (!ea) {
            int c = key_cmp(equi, a, equi, bb, ekinds);
            if (c != 0) return c < 0;
        }
        if (secondary) {
            bool sa = ArrowArrayViewIsNull(secondary, a);
            bool sb = ArrowArrayViewIsNull(secondary, bb);
            if (sa != sb) return !sa;
            if (!sa) {
                int c = cell_cmp(secondary, a, secondary, bb, secondary_kind);
                if (c != 0) return c < 0;
            }
        }
        return a < bb;
    });
    return order;
}

/// Given `order` sorted by order_rows, [i, return) is the maximal run sharing
/// order[i]'s equi key (null-keyed rows form their own run).
inline std::int64_t equi_partition_end(const std::vector<std::int64_t>& order,
                                       const KeyViews& equi,
                                       const std::vector<KeyKind>& ekinds,
                                       std::int64_t i, std::int64_t n) {
    std::int64_t rep = order[static_cast<std::size_t>(i)];
    bool rn = row_key_null(equi, rep);
    std::int64_t j = i + 1;
    while (j < n) {
        std::int64_t x = order[static_cast<std::size_t>(j)];
        bool xn = row_key_null(equi, x);
        if (xn != rn) break;
        if (!rn && key_cmp(equi, rep, equi, x, ekinds) != 0) break;
        ++j;
    }
    return j;
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_JOIN_INTERNAL_H
