// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/vector.h>

// nanoarrow must precede c_api.h: it defines ARROW_FLAG_DICTIONARY_ORDERED,
// nanoarrow's outer include guard, which would otherwise suppress its
// ArrowArrayStream definition.
#include <dftracer/utils/arrow/internal.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/view_builder.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <nanoarrow/nanoarrow.h>

#include <cstring>
#include <memory>
#include <stdexcept>

namespace dftracer::utils::arrow {

using dataframe::adopt_type_from;
using dataframe::borrow_buffer;
using dataframe::BufferView;
using dataframe::DataFrame;
using dataframe::Encoding;
using dataframe::new_buffer;
using dataframe::Series;
using dataframe::TimeUnit;
using dataframe::TypeId;
using dataframe::writable;

namespace {

ArrowTimeUnit to_arrow_time_unit(TimeUnit u) {
    return static_cast<ArrowTimeUnit>(u);
}

TimeUnit from_arrow_time_unit(ArrowTimeUnit u) {
    return static_cast<TimeUnit>(u);
}

ArrowType to_arrow_type(TypeId t) {
    switch (t) {
        case TypeId::Bool:
            return NANOARROW_TYPE_BOOL;
        case TypeId::Int8:
            return NANOARROW_TYPE_INT8;
        case TypeId::Int16:
            return NANOARROW_TYPE_INT16;
        case TypeId::Int32:
            return NANOARROW_TYPE_INT32;
        case TypeId::Int64:
            return NANOARROW_TYPE_INT64;
        case TypeId::Uint8:
            return NANOARROW_TYPE_UINT8;
        case TypeId::Uint16:
            return NANOARROW_TYPE_UINT16;
        case TypeId::Uint32:
            return NANOARROW_TYPE_UINT32;
        case TypeId::Uint64:
            return NANOARROW_TYPE_UINT64;
        case TypeId::Float32:
            return NANOARROW_TYPE_FLOAT;
        case TypeId::Float64:
            return NANOARROW_TYPE_DOUBLE;
        case TypeId::String:
            return NANOARROW_TYPE_STRING;
        case TypeId::Binary:
            return NANOARROW_TYPE_BINARY;
        case TypeId::List:
            return NANOARROW_TYPE_LIST;
        case TypeId::Struct:
            return NANOARROW_TYPE_STRUCT;
        case TypeId::Float16:
            return NANOARROW_TYPE_HALF_FLOAT;
        case TypeId::Date32:
            return NANOARROW_TYPE_DATE32;
        case TypeId::Date64:
            return NANOARROW_TYPE_DATE64;
        case TypeId::Time32:
            return NANOARROW_TYPE_TIME32;
        case TypeId::Time64:
            return NANOARROW_TYPE_TIME64;
        case TypeId::Timestamp:
            return NANOARROW_TYPE_TIMESTAMP;
        case TypeId::Duration:
            return NANOARROW_TYPE_DURATION;
        case TypeId::Decimal128:
            return NANOARROW_TYPE_DECIMAL128;
        case TypeId::Decimal256:
            return NANOARROW_TYPE_DECIMAL256;
        case TypeId::FixedSizeBinary:
            return NANOARROW_TYPE_FIXED_SIZE_BINARY;
        case TypeId::LargeString:
            return NANOARROW_TYPE_LARGE_STRING;
        case TypeId::LargeBinary:
            return NANOARROW_TYPE_LARGE_BINARY;
        case TypeId::LargeList:
            return NANOARROW_TYPE_LARGE_LIST;
        case TypeId::FixedSizeList:
            return NANOARROW_TYPE_FIXED_SIZE_LIST;
        case TypeId::Map:
            return NANOARROW_TYPE_MAP;
        case TypeId::Unknown:
            break;  // schema-only marker, never a real Series' type
    }
    return NANOARROW_TYPE_UNINITIALIZED;
}

// Populates `out` from `view.type`, and fails loudly (naming the Arrow type)
// rather than silently for a type this bridge cannot represent. List/
// LargeList/FixedSizeList/Struct/Map/DICTIONARY are handled by their own
// import_* functions before this is reached.
bool from_arrow_type(const ArrowSchemaView& view, TypeId& out) {
    switch (view.type) {
        case NANOARROW_TYPE_BOOL:
            out = TypeId::Bool;
            return true;
        case NANOARROW_TYPE_INT8:
            out = TypeId::Int8;
            return true;
        case NANOARROW_TYPE_INT16:
            out = TypeId::Int16;
            return true;
        case NANOARROW_TYPE_INT32:
            out = TypeId::Int32;
            return true;
        case NANOARROW_TYPE_INT64:
            out = TypeId::Int64;
            return true;
        case NANOARROW_TYPE_UINT8:
            out = TypeId::Uint8;
            return true;
        case NANOARROW_TYPE_UINT16:
            out = TypeId::Uint16;
            return true;
        case NANOARROW_TYPE_UINT32:
            out = TypeId::Uint32;
            return true;
        case NANOARROW_TYPE_UINT64:
            out = TypeId::Uint64;
            return true;
        case NANOARROW_TYPE_FLOAT:
            out = TypeId::Float32;
            return true;
        case NANOARROW_TYPE_DOUBLE:
            out = TypeId::Float64;
            return true;
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_STRING_VIEW:
            out = TypeId::String;
            return true;
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_BINARY_VIEW:
            out = TypeId::Binary;
            return true;
        case NANOARROW_TYPE_HALF_FLOAT:
            out = TypeId::Float16;
            return true;
        case NANOARROW_TYPE_DATE32:
            out = TypeId::Date32;
            return true;
        case NANOARROW_TYPE_DATE64:
            out = TypeId::Date64;
            return true;
        case NANOARROW_TYPE_TIME32:
            out = TypeId::Time32;
            return true;
        case NANOARROW_TYPE_TIME64:
            out = TypeId::Time64;
            return true;
        case NANOARROW_TYPE_TIMESTAMP:
            out = TypeId::Timestamp;
            return true;
        case NANOARROW_TYPE_DURATION:
            out = TypeId::Duration;
            return true;
        case NANOARROW_TYPE_DECIMAL128:
            out = TypeId::Decimal128;
            return true;
        case NANOARROW_TYPE_DECIMAL256:
            out = TypeId::Decimal256;
            return true;
        case NANOARROW_TYPE_FIXED_SIZE_BINARY:
            out = TypeId::FixedSizeBinary;
            return true;
        case NANOARROW_TYPE_LARGE_STRING:
            out = TypeId::LargeString;
            return true;
        case NANOARROW_TYPE_LARGE_BINARY:
            out = TypeId::LargeBinary;
            return true;
        default:
            DFTRACER_UTILS_LOG_ERROR(
                "Arrow import: unsupported type '{}'",
                ArrowTypeString(view.type) ? ArrowTypeString(view.type) : "?");
            return false;
    }
}

/// Keeps a column's buffers (and, for a selection, its base) alive for as long
/// as an exported ArrowArray references them, and owns the array's buffer list.
struct ExportedArray : dstd::HeapObject {
    dstd::Vector<BufferView> keep;
    dstd::SharedPtr<dftu_series> child_keep;
    dstd::Vector<dstd::SharedPtr<dftu_series>> children_keep;
    const void* buffers[3];
    dstd::Vector<const void*> view_buffers;
    dstd::Vector<std::int64_t> view_sizes;
    /// The exported dictionary or child arrays, and pointers to the children.
    dstd::Vector<ArrowArray> arrays;
    dstd::Vector<ArrowArray*> child_ptrs;

    /// Sets aside @p n zeroed arrays; call once.
    ArrowArray* reserve_arrays(std::size_t n) {
        arrays.resize(n);
        return arrays.data();
    }
    void set_children(ArrowArray* array, std::size_t n) {
        reserve_arrays(n);
        child_ptrs.resize(n);
        for (std::size_t i = 0; i < n; ++i) child_ptrs[i] = &arrays[i];
        array->n_children = static_cast<std::int64_t>(n);
        array->children = child_ptrs.data();
    }
};

bool is_varwidth(TypeId t) {
    return t == TypeId::String || t == TypeId::Binary;
}

bool is_varwidth_large(TypeId t) {
    return t == TypeId::LargeString || t == TypeId::LargeBinary;
}

bool is_list_like(TypeId t) { return t == TypeId::List || t == TypeId::Map; }

void release_exported(ArrowArray* array) {
    if (array->dictionary != nullptr) {
        if (array->dictionary->release != nullptr)
            array->dictionary->release(array->dictionary);
        array->dictionary = nullptr;
    }
    for (std::int64_t i = 0; i < array->n_children; ++i) {
        if (array->children[i]->release != nullptr)
            array->children[i]->release(array->children[i]);
    }
    array->children = nullptr;
    delete static_cast<ExportedArray*>(array->private_data);
    array->private_data = nullptr;
    array->release = nullptr;
}

void export_array(const dftu_series& col, ArrowArray* array);

void export_flat(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->keep.push_back(col.data);
    h->buffers[0] = col.validity.block() ? col.validity.data() : nullptr;
    h->buffers[1] = col.data.block() ? col.data.data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 2;
    array->buffers = h->buffers;
    array->private_data = h;
    array->release = release_exported;
}

// Buffers: validity, views, each data buffer, then the int64 sizes of the data
// buffers that the C Data Interface requires last. All but the sizes alias the
// column; the sizes live in `h`.
void export_view(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->keep.push_back(col.data);
    h->view_buffers.push_back(col.validity.block() ? col.validity.data()
                                                   : nullptr);
    h->view_buffers.push_back(col.data.block() ? col.data.data() : nullptr);
    if (col.blobs)
        for (const auto& b : *col.blobs) {
            h->keep.push_back(b);
            h->view_buffers.push_back(b.data());
            h->view_sizes.push_back(static_cast<std::int64_t>(b.size()));
        }
    h->view_buffers.push_back(h->view_sizes.data());
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = static_cast<std::int64_t>(h->view_buffers.size());
    array->buffers = h->view_buffers.data();
    array->private_data = h;
    array->release = release_exported;
}

// SELECTION and DICTIONARY both export as an Arrow dictionary array: int32
// indices/codes with the base/dictionary as the child array. A SELECTION
// outer-fill row (index -1) exports as a null row over index 0.
void export_dict(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->keep.push_back(col.data);
    h->child_keep = col.child();
    h->buffers[0] = col.validity.block() ? col.validity.data() : nullptr;
    h->buffers[1] = col.data.block() ? col.data.data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    if (col.encoding == Encoding::Selection && col.data.block()) {
        const auto* idx =
            reinterpret_cast<const std::int64_t*>(col.data.data());
        const std::int64_t n = col.length;
        std::int64_t fills = 0;
        for (std::int64_t i = 0; i < n; ++i) fills += idx[i] < 0;
        if (fills > 0) {
            auto fixed =
                new_buffer(static_cast<std::size_t>(n) * sizeof(std::int64_t));
            auto valid = new_buffer(static_cast<std::size_t>(n + 7) / 8);
            auto* out = reinterpret_cast<std::int64_t*>(writable(fixed));
            if (col.validity.block())
                std::memcpy(writable(valid), col.validity.data(), valid.size());
            else
                std::memset(writable(valid), 0xff, valid.size());
            for (std::int64_t i = 0; i < n; ++i) {
                out[i] = idx[i] < 0 ? 0 : idx[i];
                if (idx[i] < 0)
                    writable(valid)[i >> 3] &=
                        static_cast<std::uint8_t>(~(1u << (i & 7)));
            }
            std::int64_t nulls = 0;
            for (std::int64_t i = 0; i < n; ++i)
                nulls += !((valid.data()[i >> 3] >> (i & 7)) & 1);
            h->keep.push_back(fixed);
            h->keep.push_back(valid);
            h->buffers[0] = valid.data();
            h->buffers[1] = fixed.data();
            array->null_count = nulls;
        }
    }
    array->offset = 0;
    array->n_buffers = 2;
    array->buffers = h->buffers;
    array->private_data = h;
    array->release = release_exported;
    array->dictionary = h->reserve_arrays(1);
    export_array(*col.child(), array->dictionary);
}

void export_varwidth(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->keep.push_back(col.offsets);
    h->keep.push_back(col.data);
    h->buffers[0] = col.validity.block() ? col.validity.data() : nullptr;
    h->buffers[1] = col.offsets.block() ? col.offsets.data() : nullptr;
    h->buffers[2] = col.data.block() ? col.data.data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 3;
    array->buffers = h->buffers;
    array->private_data = h;
    array->release = release_exported;
}

// LargeString/LargeBinary: same layout as export_varwidth but the offsets
// buffer is int64 (offsets64), a distinct physical layout from String/Binary.
void export_varwidth_large(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->keep.push_back(col.offsets);
    h->keep.push_back(col.data);
    h->buffers[0] = col.validity.block() ? col.validity.data() : nullptr;
    h->buffers[1] = col.offsets.block() ? col.offsets.data() : nullptr;
    h->buffers[2] = col.data.block() ? col.data.data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 3;
    array->buffers = h->buffers;
    array->private_data = h;
    array->release = release_exported;
}

// STRUCT: one validity buffer, one child array per field.
void export_struct(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->buffers[0] = col.validity.block() ? col.validity.data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 1;
    array->buffers = h->buffers;
    h->set_children(array, col.num_fields());
    for (std::size_t i = 0; i < col.num_fields(); ++i) {
        export_array(*col.nested[i].series, array->children[i]);
        h->children_keep.push_back(col.nested[i].series);
    }
    array->private_data = h;
    array->release = release_exported;
}

// LIST and MAP: validity + int32 offsets, one child array (the flattened
// values for LIST, the Struct{key, value} entries for MAP). Both use the
// same layout, so MAP reuses this verbatim.
void export_list(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->keep.push_back(col.offsets);
    h->child_keep = col.child();
    h->buffers[0] = col.validity.block() ? col.validity.data() : nullptr;
    h->buffers[1] = col.offsets.block() ? col.offsets.data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 2;
    array->buffers = h->buffers;
    h->set_children(array, 1);
    export_array(*col.child(), array->children[0]);
    array->private_data = h;
    array->release = release_exported;
}

// LargeList: same shape as export_list but with int64 offsets (offsets64).
void export_list_large(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->keep.push_back(col.offsets);
    h->child_keep = col.child();
    h->buffers[0] = col.validity.block() ? col.validity.data() : nullptr;
    h->buffers[1] = col.offsets.block() ? col.offsets.data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 2;
    array->buffers = h->buffers;
    h->set_children(array, 1);
    export_array(*col.child(), array->children[0]);
    array->private_data = h;
    array->release = release_exported;
}

// FixedSizeList: validity only, no offsets - each row is `col.fixed_size`
// elements of the flattened child.
void export_fixed_size_list(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->child_keep = col.child();
    h->buffers[0] = col.validity.block() ? col.validity.data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 1;
    array->buffers = h->buffers;
    h->set_children(array, 1);
    export_array(*col.child(), array->children[0]);
    array->private_data = h;
    array->release = release_exported;
}

bool is_dict_encoded(const dftu_series& col) {
    return col.encoding == Encoding::Selection ||
           col.encoding == Encoding::Dictionary;
}

void export_array(const dftu_series& col, ArrowArray* array) {
    std::memset(array, 0, sizeof(*array));
    if (col.is_chunked()) {
        Series joined{dftu_series_materialize(&col)};
        if (!joined.valid())
            dstd::fatal("to_arrow: cannot join a chunked column");
        export_array(*joined.handle(), array);
        return;
    }
    if (is_dict_encoded(col))
        export_dict(col, array);
    else if (col.type == TypeId::Struct)
        export_struct(col, array);
    else if (is_list_like(col.type))
        export_list(col, array);
    else if (col.type == TypeId::LargeList)
        export_list_large(col, array);
    else if (col.type == TypeId::FixedSizeList)
        export_fixed_size_list(col, array);
    else if (col.encoding == Encoding::View)
        export_view(col, array);
    else if (is_varwidth(col.type))
        export_varwidth(col, array);
    else if (is_varwidth_large(col.type))
        export_varwidth_large(col, array);
    else
        export_flat(col, array);
}

void build_schema(const dftu_series& col, ArrowSchema* schema);

constexpr dstd::StringView JSON_EXTENSION = "arrow.json";

// Marks `schema` as the canonical `arrow.json` extension over utf8.
void set_json_extension(ArrowSchema* schema) {
    ArrowBuffer meta;
    ArrowMetadataBuilderInit(&meta, nullptr);
    ArrowMetadataBuilderAppend(
        &meta, ArrowCharView("ARROW:extension:name"),
        ArrowStringView{JSON_EXTENSION.data(),
                        static_cast<std::int64_t>(JSON_EXTENSION.size())});
    ArrowMetadataBuilderAppend(&meta, ArrowCharView("ARROW:extension:metadata"),
                               ArrowCharView(""));
    ArrowSchemaSetMetadata(schema, reinterpret_cast<const char*>(meta.data));
    ArrowBufferReset(&meta);
}

// Sets `schema`'s format (and recurses into children) for `col`'s type,
// WITHOUT calling ArrowSchemaInit/InitFromType: `schema` may already be
// initialized and named (a List/LargeList/FixedSizeList "item" child, or a
// Map's pre-built "entries"/"key"/"value" nodes) and re-initing it would
// orphan that name allocation.
void set_type_in_place(const dftu_series& col, ArrowSchema* schema) {
    if (is_dict_encoded(col)) {
        // SELECTION indices are int64; DICTIONARY codes are int32.
        ArrowSchemaSetType(schema, col.encoding == Encoding::Selection
                                       ? NANOARROW_TYPE_INT64
                                       : NANOARROW_TYPE_INT32);
        ArrowSchemaAllocateDictionary(schema);
        build_schema(*col.child(), schema->dictionary);
    } else if (col.type == TypeId::Struct) {
        ArrowSchemaSetType(schema, NANOARROW_TYPE_STRUCT);
        ArrowSchemaAllocateChildren(
            schema, static_cast<std::int64_t>(col.num_fields()));
        for (std::size_t i = 0; i < col.num_fields(); ++i) {
            build_schema(*col.nested[i].series, schema->children[i]);
            ArrowSchemaSetName(schema->children[i], col.nested[i].name.c_str());
        }
    } else if (col.type == TypeId::List) {
        ArrowSchemaSetType(schema, NANOARROW_TYPE_LIST);
        set_type_in_place(*col.child(), schema->children[0]);
    } else if (col.type == TypeId::LargeList) {
        ArrowSchemaSetType(schema, NANOARROW_TYPE_LARGE_LIST);
        set_type_in_place(*col.child(), schema->children[0]);
    } else if (col.type == TypeId::FixedSizeList) {
        ArrowSchemaSetTypeFixedSize(schema, NANOARROW_TYPE_FIXED_SIZE_LIST,
                                    col.fixed_size());
        set_type_in_place(*col.child(), schema->children[0]);
    } else if (col.type == TypeId::Map) {
        ArrowSchemaSetType(schema, NANOARROW_TYPE_MAP);
        ArrowSchema* entries = schema->children[0];
        set_type_in_place(*col.child()->nested[0].series, entries->children[0]);
        set_type_in_place(*col.child()->nested[1].series, entries->children[1]);
    } else if (col.type == TypeId::Timestamp || col.type == TypeId::Time32 ||
               col.type == TypeId::Time64 || col.type == TypeId::Duration) {
        const char* tz =
            (col.type == TypeId::Timestamp && !col.timezone().empty())
                ? col.timezone().data()
                : nullptr;
        ArrowSchemaSetTypeDateTime(schema, to_arrow_type(col.type),
                                   to_arrow_time_unit(col.time_unit()), tz);
    } else if (col.type == TypeId::Decimal128 ||
               col.type == TypeId::Decimal256) {
        ArrowSchemaSetTypeDecimal(schema, to_arrow_type(col.type),
                                  col.decimal_precision(), col.decimal_scale());
    } else if (col.type == TypeId::FixedSizeBinary) {
        ArrowSchemaSetTypeFixedSize(schema, NANOARROW_TYPE_FIXED_SIZE_BINARY,
                                    col.fixed_size());
    } else {
        ArrowType t = to_arrow_type(col.type);
        if (col.encoding == Encoding::View)
            t = col.type == TypeId::String ? NANOARROW_TYPE_STRING_VIEW
                                           : NANOARROW_TYPE_BINARY_VIEW;
        ArrowSchemaSetType(schema, t);
        if (col.json()) set_json_extension(schema);
    }
}

// Initialize a fresh `schema` (root, or a child from AllocateChildren) for
// `col`, which holds no CHUNKED column that the exported type depends on (see
// join_for_export).
void build_schema(const dftu_series& col, ArrowSchema* schema) {
    ArrowSchemaInit(schema);
    set_type_in_place(col, schema);
}

bool is_text(TypeId t) {
    return t == TypeId::String || t == TypeId::LargeString ||
           t == TypeId::Binary || t == TypeId::LargeBinary;
}

// Whether the Arrow type of the join of a CHUNKED column depends on the chunks
// beyond their type: a nested one keeps the layout of its children, and a text
// one joins into a view or a dictionary.
bool type_depends_on_join(const dftu_series& col) {
    return col.is_chunked() &&
           (dataframe::is_nested(col.type) || is_text(col.type));
}

// Sets `out` to `col` with every CHUNKED column that its exported type depends
// on joined, so the schema and the array come from one object: an Arrow array
// has no chunks, and the first chunk's schema can differ from the join's. The
// join copies the rows, as an export of a chunked column always did, except
// that chunked text dictionaries join into one dictionary (only the codes are
// copied) when their values merge, and into plain text otherwise. The
// search goes through struct fields, list-like and map children, and the
// base of a SELECTION or DICTIONARY. False, leaving `out` alone, when `col`
// has none.
bool join_for_export(const dftu_series& col, Series& out) {
    if (type_depends_on_join(col)) {
        if (is_text(col.type)) {
            dstd::Vector<Series> owned;
            dstd::Vector<const Series*> parts;
            owned.reserve(col.nested.size());
            for (const dftu_nested& n : col.nested)
                owned.emplace_back(dftu_series_share(n.series.get()));
            for (const Series& p : owned) parts.push_back(&p);
            dstd::Result<Series> dictionary =
                dataframe::concat_dictionaries(parts);
            if (!dictionary)
                dstd::fatal("to_arrow: cannot join a chunked column");
            if (dictionary->valid()) {
                out = std::move(*dictionary);
                return true;
            }
        }
        out = Series{dftu_series_materialize(&col)};
        if (!out.valid()) dstd::fatal("to_arrow: cannot join a chunked column");
        return true;
    }
    if (col.is_chunked() || col.nested.empty()) return false;
    dstd::Vector<Series> joined(col.nested.size());
    bool any = false;
    for (std::size_t i = 0; i < col.nested.size(); ++i)
        any |= join_for_export(*col.nested[i].series, joined[i]);
    if (!any) return false;
    auto* copy = new dftu_series(col);
    for (std::size_t i = 0; i < joined.size(); ++i)
        if (joined[i].valid())
            copy->nested[i].series = adopt_series(joined[i].release());
    out = Series{copy};
    return true;
}

// Whether two FLAT columns of one type export under one schema: the same
// encodings, parameters and field names down to the last child.
bool same_layout_of(const dftu_series& a, const dftu_series& b) {
    if (a.type != b.type || a.encoding != b.encoding ||
        !a.params.same(a.type, b.params) || a.nested.size() != b.nested.size())
        return false;
    for (std::size_t i = 0; i < a.nested.size(); ++i)
        if (a.nested[i].name != b.nested[i].name ||
            !same_layout_of(*a.nested[i].series, *b.nested[i].series))
            return false;
    return true;
}

// Whether `col` and everything under it is FLAT, so a batch of it exports
// without a join.
bool is_flat_through(const dftu_series& col) {
    if (col.encoding != Encoding::Flat) return false;
    for (const dftu_nested& n : col.nested)
        if (!is_flat_through(*n.series)) return false;
    return true;
}

// Whether the type of `a` and `b` is the same at every node: formats, names,
// children and dictionaries. Only the metadata of a child counts (the JSON
// extension), as the root carries the frame's index.
bool same_schema_node(const ArrowSchema& a, const ArrowSchema& b, bool root) {
    auto name_of = [](const ArrowSchema& s) {
        return dstd::StringView(s.name != nullptr ? s.name : "");
    };
    if (dstd::StringView(a.format) != dstd::StringView(b.format) ||
        name_of(a) != name_of(b) || a.n_children != b.n_children ||
        (a.dictionary == nullptr) != (b.dictionary == nullptr) ||
        (!root && (a.metadata == nullptr) != (b.metadata == nullptr)))
        return false;
    for (std::int64_t i = 0; i < a.n_children; ++i)
        if (!same_schema_node(*a.children[i], *b.children[i], false))
            return false;
    return a.dictionary == nullptr ||
           same_schema_node(*a.dictionary, *b.dictionary, false);
}

bool is_text_column(TypeId t) {
    return t == TypeId::String || t == TypeId::LargeString ||
           t == TypeId::Binary || t == TypeId::LargeBinary;
}

// `col` shaped like the Arrow type `target` where only the layout differs: a
// text column as views, plain text or a dictionary, and the children of nested
// columns the same way. Other differences stay for the caller's check. A struct
// field is matched by name when `by_name`, else by position. Copies the bytes
// of a text column whose layout changes (decoding a dictionary copies its
// values per row; encoding one builds the distinct values and the codes, after
// a copy of a view column's text into one buffer), and the headers of the
// nested columns above it.
dstd::Result<Series> conform_column(const Series& col,
                                    const ArrowSchema& target, bool by_name) {
    const dftu_series& c = *col.handle();
    const bool want_dict = target.dictionary != nullptr;
    const bool want_selection =
        want_dict && std::strcmp(target.format, "l") == 0;
    const bool decode =
        is_dict_encoded(c) &&
        (!want_dict || (c.encoding == Encoding::Selection && !want_selection));
    // Encode only into dictionary values of this column's own type, so a
    // column of another type stays as it is for the caller's check.
    auto own_values = [&](dstd::StringView f) {
        switch (c.type) {
            case TypeId::String:
                return f == "u" || f == "vu";
            case TypeId::LargeString:
                return f == "U";
            case TypeId::Binary:
                return f == "z" || f == "vz";
            case TypeId::LargeBinary:
                return f == "Z";
            default:
                return false;
        }
    };
    const bool encode = !is_dict_encoded(c) && want_dict && !want_selection &&
                        is_text_column(c.type) && !c.json() &&
                        own_values(target.dictionary->format);
    if (decode || encode) {
        Series changed = decode ? Series{dftu_series_materialize(&c)}
                                : col.dictionary_encode();
        if (!changed.valid())
            return common::make_error<
                "to_arrow_stream: a column cannot change its dictionary "
                "layout">(common::ErrorCode::InvalidArgument);
        return conform_column(changed, target, by_name);
    }
    if (want_dict != is_dict_encoded(c)) return col.share();
    if (target.dictionary != nullptr) {
        dstd::Result<Series> values =
            conform_column(Series{dftu_series_share(c.child().get())},
                           *target.dictionary, false);
        if (!values) return values;
        auto* copy = new dftu_series(c);
        copy->set_child(adopt_series(values->release()));
        return Series{copy};
    }
    if (is_text_column(c.type)) {
        const bool want_view = std::strcmp(target.format, "vu") == 0 ||
                               std::strcmp(target.format, "vz") == 0;
        if (want_view == (c.encoding == Encoding::View)) return col.share();
        if (want_view) {
            dataframe::ViewBuilder builder;
            builder.reserve(c.length);
            builder.append_column(c);
            const bool binary =
                c.type == TypeId::Binary || c.type == TypeId::LargeBinary;
            return builder.finish(binary ? TypeId::Binary : TypeId::String,
                                  c.json());
        }
        Series flat{dftu_series_materialize(&c)};
        if (!flat.valid())
            return common::make_error<
                "to_arrow_stream: a text column cannot be made plain">(
                common::ErrorCode::InvalidArgument);
        return flat;
    }
    const bool has_child =
        c.type == TypeId::List || c.type == TypeId::LargeList ||
        c.type == TypeId::FixedSizeList || c.type == TypeId::Map;
    if (has_child && c.child() && target.n_children == 1) {
        dstd::Result<Series> child =
            conform_column(Series{dftu_series_share(c.child().get())},
                           *target.children[0], false);
        if (!child) return child;
        auto* copy = new dftu_series(c);
        copy->set_child(adopt_series(child->release()));
        return Series{copy};
    }
    if (c.has_fields()) {
        if (!by_name &&
            static_cast<std::int64_t>(c.nested.size()) != target.n_children)
            return col.share();
        auto* copy = new dftu_series(c);
        Series owner{copy};
        for (std::size_t i = 0; i < c.nested.size(); ++i) {
            const ArrowSchema* want = nullptr;
            if (!by_name) {
                want = target.children[i];
            } else {
                for (std::int64_t k = 0; k < target.n_children; ++k)
                    if (c.nested[i].name == target.children[k]->name)
                        want = target.children[k];
            }
            if (want == nullptr) continue;
            dstd::Result<Series> field = conform_column(
                Series{dftu_series_share(c.nested[i].series.get())}, *want,
                false);
            if (!field) return field;
            copy->nested[i].series = adopt_series(field->release());
        }
        return owner;
    }
    return col.share();
}

// Whether every nested or dictionary column of `got` that `target` also has,
// with the same outer format, has the same type at every node. A deeper
// difference fails the stream here: the column cannot take the locked schema
// and the extra column holds only scalar values.
bool same_nested_columns(const ArrowSchema& target, const ArrowSchema& got) {
    for (std::int64_t i = 0; i < got.n_children; ++i) {
        const ArrowSchema& g = *got.children[i];
        for (std::int64_t k = 0; k < target.n_children; ++k) {
            const ArrowSchema& t = *target.children[k];
            if (dstd::StringView(t.name != nullptr ? t.name : "") !=
                    dstd::StringView(g.name != nullptr ? g.name : "") ||
                dstd::StringView(t.format) != dstd::StringView(g.format))
                continue;
            const bool nested = g.format[0] == '+' || g.dictionary != nullptr;
            if (nested && !same_schema_node(t, g, false)) return false;
        }
    }
    return true;
}

}  // namespace

namespace detail {

bool same_schema(const ArrowSchema& a, const ArrowSchema& b) {
    return same_schema_node(a, b, true);
}

dstd::Result<Series> conform_to_schema(const Series& col,
                                       const ArrowSchema& schema,
                                       bool by_name) {
    dstd::Result<Series> out = conform_column(col, schema, by_name);
    if (!out) return out;
    ArrowSchema got{};
    column_schema(*out, &got);
    const bool same =
        by_name ? same_nested_columns(schema, got) : same_schema(schema, got);
    got.release(&got);
    if (!same)
        return common::make_error<
            "to_arrow_stream: a batch has other columns than the first one; "
            "collect() the query instead">(common::ErrorCode::InvalidArgument);
    return out;
}

Series for_export(const Series& col) {
    const dftu_series* c = col.handle();
    if (c == nullptr) return Series{};
    Series joined;
    if (join_for_export(*c, joined)) return joined;
    return col.share();
}

bool same_layout(const Series& a, const Series& b) {
    return same_layout_of(*a.handle(), *b.handle());
}

void to_arrow(const Series& col, ArrowSchema* schema, ArrowArray* array) {
    const Series ready = for_export(col);
    column_schema(ready, schema);
    column_array(ready, array);
}

void column_schema(const Series& col, ArrowSchema* schema) {
    const Series ready = for_export(col);
    const dftu_series* c = ready.handle();
    if (c == nullptr) {
        ArrowSchemaInitFromType(schema, NANOARROW_TYPE_STRUCT);
        return;
    }
    build_schema(*c, schema);
}

void column_array(const Series& col, ArrowArray* array) {
    const dftu_series* c = col.handle();
    if (c != nullptr) {
        export_array(*c, array);
        return;
    }
    auto* h = new ExportedArray();
    std::memset(array, 0, sizeof(*array));
    array->length = 0;
    array->n_buffers = 1;
    array->buffers = h->buffers;
    array->private_data = h;
    array->release = release_exported;
}

dstd::Vector<ArrowArray> to_arrow_batches(const Series& st,
                                          ArrowSchema* schema) {
    const dftu_series* c = st.handle();
    dstd::Vector<ArrowArray> out;
    bool per_chunk = c != nullptr && c->type == TypeId::Struct &&
                     c->num_fields() > 0 && !c->validity.block();
    std::size_t nchunks = 0;
    if (per_chunk) {
        const dftu_series& first = *c->nested[0].series;
        per_chunk = first.is_chunked();
        if (per_chunk) nchunks = first.nested.size();
        for (std::size_t i = 0; per_chunk && i < c->num_fields(); ++i) {
            const dftu_series& f = *c->nested[i].series;
            per_chunk = f.is_chunked() && f.nested.size() == nchunks &&
                        std::memcmp(f.data.data(), first.data.data(),
                                    (nchunks + 1) * sizeof(std::int64_t)) == 0;
            // One schema describes every batch, so the chunks of a column
            // share a layout.
            for (std::size_t k = 0; per_chunk && k < nchunks; ++k)
                per_chunk =
                    is_flat_through(*f.nested[k].series) &&
                    same_layout_of(*f.nested[k].series, *f.nested[0].series);
        }
    }
    if (!per_chunk) {
        out.emplace_back();
        to_arrow(st, schema, &out.back());
        return out;
    }
    auto fill_batch = [&](std::size_t k, dftu_series& batch) {
        batch.type = TypeId::Struct;
        batch.length = c->nested[0].series->nested[k].series->length;
        for (std::size_t i = 0; i < c->num_fields(); ++i)
            batch.nested.push_back(
                {c->nested[i].series->nested[k].series, c->nested[i].name});
    };
    dftu_series first_batch;
    fill_batch(0, first_batch);
    build_schema(first_batch, schema);
    out.reserve(nchunks);
    for (std::size_t k = 0; k < nchunks; ++k) {
        dftu_series batch;
        fill_batch(k, batch);
        out.emplace_back();
        export_struct(batch, &out.back());
    }
    return out;
}

}  // namespace detail

namespace {

// The validity or bool bits [offset, offset + n) of `bits`: wrapped when the
// offset is byte-aligned, else copied into a fresh bitmap.
BufferView import_bits(const void* bits, std::int64_t offset, std::int64_t n,
                       const dstd::SharedPtr<void>& owner) {
    auto* p = static_cast<std::uint8_t*>(const_cast<void*>(bits));
    const std::size_t bytes = (static_cast<std::size_t>(n) + 7) / 8;
    if (offset % 8 == 0) return borrow_buffer(p + offset / 8, bytes, owner);
    auto out = new_buffer(bytes);
    std::memset(writable(out), 0, bytes);
    for (std::int64_t i = 0; i < n; ++i) {
        const std::int64_t b = offset + i;
        if ((p[b >> 3] >> (b & 7)) & 1)
            writable(out)[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    return out;
}

void import_validity(dftu_series* col, const ArrowArray* arr,
                     const dstd::SharedPtr<void>& owner) {
    if (arr->n_buffers == 0 || arr->buffers[0] == nullptr ||
        arr->null_count == 0)
        return;
    col->validity =
        import_bits(arr->buffers[0], arr->offset, arr->length, owner);
    if (arr->null_count < 0) {
        std::int64_t nulls = 0;
        for (std::int64_t i = 0; i < arr->length; ++i)
            nulls += !((col->validity.data()[i >> 3] >> (i & 7)) & 1);
        col->null_count = nulls;
    }
    // A column with no nulls has no bitmap; slices and kernels rely on it.
    if (col->null_count == 0) col->validity = {};
}

// A child of a sliced parent: the parent's offset shifts the child's window.
// The copy aliases the child's buffers and is never released.
ArrowArray child_window(const ArrowArray* child, std::int64_t offset,
                        std::int64_t length) {
    ArrowArray c = *child;
    c.offset += offset;
    c.length = length;
    c.null_count = -1;
    return c;
}

// `owner` keeps the backing Arrow array alive for as long as any wrapped buffer
// survives, so the imported buffers can alias `arr` zero copy.
Series import_flat(const ArrowSchema* schema, const ArrowArray* arr,
                   dstd::SharedPtr<void> owner) {
    ArrowSchemaView view;
    ArrowError error;
    if (ArrowSchemaViewInit(&view, schema, &error) != NANOARROW_OK)
        return Series{};
    TypeId type;
    if (!from_arrow_type(view, type)) return Series{};

    auto* col = new dftu_series();
    col->type = type;
    col->encoding = Encoding::Flat;
    col->length = arr->length;
    col->null_count = arr->null_count < 0 ? 0 : arr->null_count;
    std::int64_t n = arr->length;
    if (type == TypeId::Timestamp || type == TypeId::Time32 ||
        type == TypeId::Time64 || type == TypeId::Duration) {
        col->set_time_unit(from_arrow_time_unit(view.time_unit));
        if (type == TypeId::Timestamp && view.timezone != nullptr)
            col->set_timezone(view.timezone);
    } else if (type == TypeId::Decimal128 || type == TypeId::Decimal256) {
        col->set_decimal(view.decimal_precision, view.decimal_scale);
    } else if (type == TypeId::FixedSizeBinary) {
        col->set_fixed_size(view.fixed_size);
    }

    auto wrap_validity = [&]() { import_validity(col, arr, owner); };

    if (view.type == NANOARROW_TYPE_STRING_VIEW ||
        view.type == NANOARROW_TYPE_BINARY_VIEW) {
        // validity, views, n_buffers - 3 data buffers, then their int64 sizes.
        // Views are self-contained, so a slice only shifts the views pointer.
        if (arr->n_buffers < 3) {
            delete col;
            return Series{};
        }
        col->encoding = Encoding::View;
        auto* views =
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1])) +
            static_cast<std::size_t>(arr->offset) * 16;
        col->data =
            borrow_buffer(views, static_cast<std::size_t>(n) * 16, owner);
        const std::int64_t n_blobs = arr->n_buffers - 3;
        const auto* sizes =
            static_cast<const std::int64_t*>(arr->buffers[arr->n_buffers - 1]);
        auto blobs = dstd::make_shared<dstd::Vector<BufferView>>();
        for (std::int64_t k = 0; k < n_blobs; ++k)
            blobs->push_back(
                borrow_buffer(static_cast<std::uint8_t*>(
                                  const_cast<void*>(arr->buffers[2 + k])),
                              static_cast<std::size_t>(sizes[k]), owner));
        col->blobs = std::move(blobs);
        wrap_validity();
        return Series{col};
    }

    if (is_varwidth(type)) {
        // 3 buffers: validity, int32 offsets (n+1), data. The offsets start at
        // arr->offset and index the data as they are.
        const std::int32_t* offs =
            static_cast<const std::int32_t*>(arr->buffers[1]) + arr->offset;
        std::size_t data_size =
            offs != nullptr ? static_cast<std::size_t>(offs[n]) : 0;
        col->offsets = borrow_buffer(
            reinterpret_cast<std::uint8_t*>(const_cast<std::int32_t*>(offs)),
            static_cast<std::size_t>(n + 1) * sizeof(std::int32_t), owner);
        col->data = borrow_buffer(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[2])),
            data_size, owner);
        wrap_validity();
        return Series{col};
    }

    if (is_varwidth_large(type)) {
        // 3 buffers: validity, int64 offsets (n+1), data.
        const std::int64_t* offs =
            static_cast<const std::int64_t*>(arr->buffers[1]) + arr->offset;
        std::size_t data_size =
            offs != nullptr ? static_cast<std::size_t>(offs[n]) : 0;
        col->offsets = borrow_buffer(
            reinterpret_cast<std::uint8_t*>(const_cast<std::int64_t*>(offs)),
            static_cast<std::size_t>(n + 1) * sizeof(std::int64_t), owner);
        col->data = borrow_buffer(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[2])),
            data_size, owner);
        wrap_validity();
        return Series{col};
    }

    // Bool is bit-packed, so a nonzero offset is a bit offset.
    // FixedSizeBinary's per-row width is col->fixed_size, not a per-TypeId
    // constant, so byte_width needs it passed alongside the type.
    std::size_t width = byte_width(type, col->fixed_size()).value_or(0);
    std::size_t ptr_off = static_cast<std::size_t>(arr->offset) * width;
    if (type == TypeId::Bool) {
        col->data = import_bits(arr->buffers[1], arr->offset, n, owner);
        wrap_validity();
        return Series{col};
    }
    auto* data = static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1]));
    std::size_t data_size = (type == TypeId::FixedSizeBinary)
                                ? static_cast<std::size_t>(n) * width
                                : buffer_bytes(type, n);
    col->data = borrow_buffer(data + ptr_off, data_size, owner);
    wrap_validity();
    return Series{col};
}

Series import_any(const ArrowSchema* schema, const ArrowArray* arr,
                  dstd::SharedPtr<void> owner);

// LIST and MAP: validity + int32 offsets (n+1), one child array. `map_type`
// selects which TypeId to tag the result with; both share the same wire
// layout (int32 offsets over a child array - the Struct{key, value} entries,
// for MAP), so only the tag differs. The child is imported recursively.
Series import_list_like(TypeId result_type, const ArrowSchema* schema,
                        const ArrowArray* arr, dstd::SharedPtr<void> owner) {
    if (schema->n_children != 1 || arr->n_children != 1) return Series{};
    auto* col = new dftu_series();
    col->type = result_type;
    col->encoding = Encoding::Flat;
    col->length = arr->length;
    col->null_count = arr->null_count < 0 ? 0 : arr->null_count;
    std::int64_t n = arr->length;
    col->offsets = borrow_buffer(
        static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1])) +
            static_cast<std::size_t>(arr->offset) * sizeof(std::int32_t),
        static_cast<std::size_t>(n + 1) * sizeof(std::int32_t), owner);
    import_validity(col, arr, owner);
    Series child = import_any(schema->children[0], arr->children[0], owner);
    if (!child.valid()) {
        delete col;
        return Series{};
    }
    col->set_child(adopt_series(child.release()));
    return Series{col};
}

// LargeList: same shape as import_list_like but with int64 offsets.
Series import_large_list(const ArrowSchema* schema, const ArrowArray* arr,
                         dstd::SharedPtr<void> owner) {
    if (schema->n_children != 1 || arr->n_children != 1) return Series{};
    auto* col = new dftu_series();
    col->type = TypeId::LargeList;
    col->encoding = Encoding::Flat;
    col->length = arr->length;
    col->null_count = arr->null_count < 0 ? 0 : arr->null_count;
    std::int64_t n = arr->length;
    col->offsets = borrow_buffer(
        static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1])) +
            static_cast<std::size_t>(arr->offset) * sizeof(std::int64_t),
        static_cast<std::size_t>(n + 1) * sizeof(std::int64_t), owner);
    import_validity(col, arr, owner);
    Series child = import_any(schema->children[0], arr->children[0], owner);
    if (!child.valid()) {
        delete col;
        return Series{};
    }
    col->set_child(adopt_series(child.release()));
    return Series{col};
}

// FixedSizeList: validity only, no offsets - the child array holds
// length * fixed_size flattened elements.
Series import_fixed_size_list(const ArrowSchemaView& view,
                              const ArrowSchema* schema, const ArrowArray* arr,
                              dstd::SharedPtr<void> owner) {
    if (schema->n_children != 1 || arr->n_children != 1) return Series{};
    auto* col = new dftu_series();
    col->type = TypeId::FixedSizeList;
    col->encoding = Encoding::Flat;
    col->length = arr->length;
    col->null_count = arr->null_count < 0 ? 0 : arr->null_count;
    col->set_fixed_size(view.fixed_size);
    std::int64_t n = arr->length;
    import_validity(col, arr, owner);
    const std::int64_t fsz = view.fixed_size;
    ArrowArray window =
        child_window(arr->children[0], arr->offset * fsz, n * fsz);
    Series child = import_any(schema->children[0], &window, owner);
    if (!child.valid()) {
        delete col;
        return Series{};
    }
    col->set_child(adopt_series(child.release()));
    return Series{col};
}

// Inverse of export_struct; each field column is imported recursively and
// aliases `arr` through `owner`.
Series import_struct(const ArrowSchema* schema, const ArrowArray* arr,
                     dstd::SharedPtr<void> owner) {
    if (schema->n_children != arr->n_children) return Series{};
    auto* col = new dftu_series();
    col->type = TypeId::Struct;
    col->encoding = Encoding::Flat;
    col->length = arr->length;
    col->null_count = arr->null_count < 0 ? 0 : arr->null_count;
    const std::int64_t n = arr->length;
    import_validity(col, arr, owner);
    for (std::int64_t i = 0; i < arr->n_children; ++i) {
        ArrowArray window = child_window(arr->children[i], arr->offset, n);
        Series child = import_any(schema->children[i], &window, owner);
        if (!child.valid()) {
            delete col;
            return Series{};
        }
        col->nested.push_back(
            {adopt_series(child.release()),
             schema->children[i]->name ? schema->children[i]->name : ""});
    }
    return Series{col};
}

// Inverse of export_dict. int32 indices map to a DICTIONARY column and int64 to
// a SELECTION column (the widths the engine and exporter use); any other index
// width is copied into int32 DICTIONARY codes.
Series import_dict(const ArrowSchema* schema, const ArrowArray* arr,
                   dstd::SharedPtr<void> owner) {
    if (schema->dictionary == nullptr || arr->dictionary == nullptr)
        return Series{};
    ArrowSchemaView view;
    ArrowError error;
    if (ArrowSchemaViewInit(&view, schema, &error) != NANOARROW_OK)
        return Series{};
    // A dictionary schema reports view.type == DICTIONARY; the index integer
    // type is in storage_type.
    TypeId index_type;
    ArrowSchemaView index_view = view;
    index_view.type = view.storage_type;
    if (!from_arrow_type(index_view, index_type)) return Series{};

    Series values = import_any(schema->dictionary, arr->dictionary, owner);
    if (!values.valid()) return Series{};

    const std::int64_t n = arr->length;
    // A sliced array starts its index buffer at arr->offset (mirrors
    // import_flat adding offset*width to the data pointer).
    const std::size_t off = static_cast<std::size_t>(arr->offset);
    auto* col = new dftu_series();
    adopt_type_from(*col, *values.handle());
    col->length = n;
    col->null_count = arr->null_count < 0 ? 0 : arr->null_count;
    col->set_child(adopt_series(values.release()));
    import_validity(col, arr, owner);

    if (index_type == TypeId::Int64) {
        col->encoding = Encoding::Selection;
        col->data = borrow_buffer(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1])) +
                off * sizeof(std::int64_t),
            static_cast<std::size_t>(n) * sizeof(std::int64_t), owner);
        return Series{col};
    }
    if (index_type == TypeId::Int32) {
        col->encoding = Encoding::Dictionary;
        col->data = borrow_buffer(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1])) +
                off * sizeof(std::int32_t),
            static_cast<std::size_t>(n) * sizeof(std::int32_t), owner);
        return Series{col};
    }

    // Other index widths (int8/16, uint*): copy into int32 DICTIONARY codes.
    col->encoding = Encoding::Dictionary;
    col->data = new_buffer(static_cast<std::size_t>(n) * sizeof(std::int32_t));
    auto* codes = reinterpret_cast<std::int32_t*>(writable(col->data));
    const std::size_t w = byte_width(index_type).value_or(0);
    const auto* raw =
        static_cast<const std::uint8_t*>(arr->buffers[1]) + off * w;
    const bool is_signed =
        index_type == TypeId::Int8 || index_type == TypeId::Int16;
    for (std::int64_t i = 0; i < n; ++i) {
        std::int64_t code = 0;
        if (is_signed) {
            std::int64_t s = 0;
            std::memcpy(&s, raw + static_cast<std::size_t>(i) * w, w);
            // Sign-extend from the index width.
            const int shift = static_cast<int>((sizeof(std::int64_t) - w) * 8);
            code = (s << shift) >> shift;
        } else {
            std::memcpy(&code, raw + static_cast<std::size_t>(i) * w, w);
        }
        codes[i] = static_cast<std::int32_t>(code);
    }
    return Series{col};
}

// Dispatch on the Arrow type: dictionary-encoded via import_dict, List/
// LargeList/FixedSizeList/Struct/Map nest recursively, everything else (flat
// fixed-width and variable-width) through import_flat.
Series import_any(const ArrowSchema* schema, const ArrowArray* arr,
                  dstd::SharedPtr<void> owner) {
    if (schema->dictionary != nullptr) return import_dict(schema, arr, owner);
    ArrowSchemaView view;
    ArrowError error;
    if (ArrowSchemaViewInit(&view, schema, &error) != NANOARROW_OK)
        return Series{};
    if (view.type == NANOARROW_TYPE_LIST)
        return import_list_like(TypeId::List, schema, arr, owner);
    if (view.type == NANOARROW_TYPE_LARGE_LIST)
        return import_large_list(schema, arr, owner);
    if (view.type == NANOARROW_TYPE_FIXED_SIZE_LIST)
        return import_fixed_size_list(view, schema, arr, owner);
    if (view.type == NANOARROW_TYPE_STRUCT)
        return import_struct(schema, arr, owner);
    if (view.type == NANOARROW_TYPE_MAP) {
        // MAP's single child is the pre-built Struct{key, value} "entries"
        // array; reuse the List import path (same int32-offset + one-child
        // layout) and just tag the result Map.
        return import_list_like(TypeId::Map, schema, arr, owner);
    }
    // An Arrow null column has no buffers. The engine has no null type, so it
    // is a String column of nulls (polars and pandas give it for an all-null
    // column).
    if (view.type == NANOARROW_TYPE_NA)
        return Series::nulls(TypeId::String, arr->length);
    Series out = import_flat(schema, arr, owner);
    if (view.extension_name.data != nullptr &&
        dstd::StringView(
            view.extension_name.data,
            static_cast<std::size_t>(view.extension_name.size_bytes)) ==
            JSON_EXTENSION &&
        out.type() == TypeId::String)
        return out.as_json();
    return out;
}

// An imported array plus whatever else must outlive its buffers: the IPC file
// mapping, a caller's bytes, or a sibling struct, any of which can have a
// shorter lifetime than the array itself.
struct ArrayOwner {
    ArrowArray array;
    dstd::SharedPtr<void> keep;
};

// Move `array` into a shared owner that runs its release once the last wrapped
// buffer is dropped; the caller's array is marked released.
dstd::SharedPtr<ArrayOwner> adopt_array(ArrowArray* array,
                                        dstd::SharedPtr<void> keep) {
    auto* moved = dstd::make_unique<ArrayOwner>().release();
    moved->array = *array;
    moved->keep = std::move(keep);
    array->release = nullptr;
    return dstd::adopt_shared_ptr(moved, [](ArrayOwner* o) {
        if (o->array.release != nullptr) o->array.release(&o->array);
        dstd::UniquePtr<ArrayOwner>(o).reset();
    });
}

}  // namespace

namespace detail {

Series import_series(const ArrowSchema* schema, ArrowArray* array,
                     dstd::SharedPtr<void> keep) {
    dstd::SharedPtr<ArrayOwner> owner = adopt_array(array, std::move(keep));
    return import_any(schema, &owner->array, owner);
}

DataFrame import_frame(const ArrowSchema* schema, ArrowArray* array,
                       dstd::SharedPtr<void> keep) {
    ArrowSchemaView view;
    ArrowError error;
    if (ArrowSchemaViewInit(&view, schema, &error) != NANOARROW_OK)
        return DataFrame{};
    if (view.type != NANOARROW_TYPE_STRUCT) return DataFrame{};

    dstd::SharedPtr<ArrayOwner> owner = adopt_array(array, std::move(keep));
    DataFrame df;
    df.names.reserve(static_cast<std::size_t>(schema->n_children));
    df.columns.reserve(static_cast<std::size_t>(schema->n_children));
    for (std::int64_t i = 0; i < schema->n_children; ++i) {
        const ArrowSchema* cs = schema->children[i];
        df.names.emplace_back(cs->name != nullptr ? cs->name : "");
        df.columns.push_back(import_any(cs, owner->array.children[i], owner));
    }
    import_frame_metadata(schema, df);
    return df;
}

bool is_struct(const ArrowSchema* schema) {
    if (schema == nullptr) return false;
    ArrowSchemaView view;
    ArrowError error;
    return ArrowSchemaViewInit(&view, schema, &error) == NANOARROW_OK &&
           view.type == NANOARROW_TYPE_STRUCT;
}

dataframe::DataType field_type(const ArrowSchema* schema) {
    dataframe::DataType type;
    if (schema == nullptr) return type;
    if (schema->dictionary != nullptr) return field_type(schema->dictionary);
    ArrowSchemaView view;
    ArrowError error;
    if (ArrowSchemaViewInit(&view, schema, &error) != NANOARROW_OK) return type;
    const bool map_view = view.type == NANOARROW_TYPE_MAP;
    auto one_child = [&]() {
        return field_type(schema->n_children == 1 ? schema->children[0]
                                                  : nullptr);
    };
    if (view.type == NANOARROW_TYPE_STRUCT) {
        dstd::Vector<dataframe::Field> fields;
        fields.reserve(static_cast<std::size_t>(schema->n_children));
        for (std::int64_t i = 0; i < schema->n_children; ++i) {
            const ArrowSchema* cs = schema->children[i];
            fields.push_back({cs->name != nullptr ? cs->name : "",
                              field_type(cs),
                              (cs->flags & ARROW_FLAG_NULLABLE) != 0});
        }
        return dataframe::struct_of(std::move(fields));
    }
    if (view.type == NANOARROW_TYPE_LIST)
        return dataframe::list_of(one_child());
    if (view.type == NANOARROW_TYPE_LARGE_LIST)
        return dataframe::large_list_of(one_child());
    if (view.type == NANOARROW_TYPE_FIXED_SIZE_LIST)
        return dataframe::fixed_size_list_of(one_child(), view.fixed_size);
    if (map_view && schema->n_children == 1 &&
        schema->children[0]->n_children == 2)
        return dataframe::map_of(field_type(schema->children[0]->children[0]),
                                 field_type(schema->children[0]->children[1]));

    if (view.type == NANOARROW_TYPE_NA)
        return dataframe::scalar(TypeId::String);
    TypeId id;
    if (!from_arrow_type(view, id)) return type;
    type = dataframe::scalar(id);
    if (id == TypeId::Timestamp || id == TypeId::Time32 ||
        id == TypeId::Time64 || id == TypeId::Duration) {
        type.set_time_unit(from_arrow_time_unit(view.time_unit));
        if (id == TypeId::Timestamp && view.timezone != nullptr)
            type.set_timezone(view.timezone);
    } else if (id == TypeId::Decimal128 || id == TypeId::Decimal256) {
        type.set_decimal(view.decimal_precision, view.decimal_scale);
    } else if (id == TypeId::FixedSizeBinary) {
        type.set_fixed_size(view.fixed_size);
    }
    return type;
}

}  // namespace detail

}  // namespace dftracer::utils::arrow
