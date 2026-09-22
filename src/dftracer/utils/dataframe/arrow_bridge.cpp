#include <dftracer/utils/dataframe/arrow_bridge.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <nanoarrow/nanoarrow.h>

#include <cstring>
#include <memory>
#include <vector>

namespace dftracer::utils::dataframe {
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
            out = TypeId::String;
            return true;
        case NANOARROW_TYPE_BINARY:
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
                "Arrow import: unsupported type '%s'",
                ArrowTypeString(view.type) ? ArrowTypeString(view.type) : "?");
            return false;
    }
}

/// Keeps a column's buffers (and, for a selection, its base) alive for as long
/// as an exported ArrowArray references them, and owns the array's buffer list.
struct ExportedArray {
    std::vector<std::shared_ptr<Buffer>> keep;
    std::shared_ptr<dftu_series> child_keep;
    std::vector<std::shared_ptr<dftu_series>> children_keep;
    const void* buffers[3];
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
        delete array->dictionary;
        array->dictionary = nullptr;
    }
    for (std::int64_t i = 0; i < array->n_children; ++i) {
        if (array->children[i]->release != nullptr)
            array->children[i]->release(array->children[i]);
        delete array->children[i];
    }
    delete[] array->children;
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
    h->buffers[0] = col.validity ? col.validity->data() : nullptr;
    h->buffers[1] = col.data ? col.data->data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 2;
    array->buffers = h->buffers;
    array->private_data = h;
    array->release = release_exported;
}

// SELECTION and DICTIONARY both export as an Arrow dictionary array: int32
// indices/codes with the base/dictionary as the child array.
void export_dict(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->keep.push_back(col.data);
    h->child_keep = col.child;
    h->buffers[0] = col.validity ? col.validity->data() : nullptr;
    h->buffers[1] = col.data ? col.data->data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 2;
    array->buffers = h->buffers;
    array->private_data = h;
    array->release = release_exported;
    array->dictionary = new ArrowArray();
    export_array(*col.child, array->dictionary);
}

void export_varwidth(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->keep.push_back(col.offsets);
    h->keep.push_back(col.data);
    h->buffers[0] = col.validity ? col.validity->data() : nullptr;
    h->buffers[1] = col.offsets ? col.offsets->data() : nullptr;
    h->buffers[2] = col.data ? col.data->data() : nullptr;
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
    h->keep.push_back(col.offsets64);
    h->keep.push_back(col.data);
    h->buffers[0] = col.validity ? col.validity->data() : nullptr;
    h->buffers[1] = col.offsets64 ? col.offsets64->data() : nullptr;
    h->buffers[2] = col.data ? col.data->data() : nullptr;
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
    h->buffers[0] = col.validity ? col.validity->data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 1;
    array->buffers = h->buffers;
    array->n_children = static_cast<std::int64_t>(col.children.size());
    array->children = new ArrowArray*[col.children.size()];
    for (std::size_t i = 0; i < col.children.size(); ++i) {
        array->children[i] = new ArrowArray();
        export_array(*col.children[i], array->children[i]);
        h->children_keep.push_back(col.children[i]);
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
    h->child_keep = col.child;
    h->buffers[0] = col.validity ? col.validity->data() : nullptr;
    h->buffers[1] = col.offsets ? col.offsets->data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 2;
    array->buffers = h->buffers;
    array->n_children = 1;
    array->children = new ArrowArray*[1];
    array->children[0] = new ArrowArray();
    export_array(*col.child, array->children[0]);
    array->private_data = h;
    array->release = release_exported;
}

// LargeList: same shape as export_list but with int64 offsets (offsets64).
void export_list_large(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->keep.push_back(col.offsets64);
    h->child_keep = col.child;
    h->buffers[0] = col.validity ? col.validity->data() : nullptr;
    h->buffers[1] = col.offsets64 ? col.offsets64->data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 2;
    array->buffers = h->buffers;
    array->n_children = 1;
    array->children = new ArrowArray*[1];
    array->children[0] = new ArrowArray();
    export_array(*col.child, array->children[0]);
    array->private_data = h;
    array->release = release_exported;
}

// FixedSizeList: validity only, no offsets - each row is `col.fixed_size`
// elements of the flattened child.
void export_fixed_size_list(const dftu_series& col, ArrowArray* array) {
    auto* h = new ExportedArray();
    h->keep.push_back(col.validity);
    h->child_keep = col.child;
    h->buffers[0] = col.validity ? col.validity->data() : nullptr;
    array->length = col.length;
    array->null_count = col.null_count;
    array->offset = 0;
    array->n_buffers = 1;
    array->buffers = h->buffers;
    array->n_children = 1;
    array->children = new ArrowArray*[1];
    array->children[0] = new ArrowArray();
    export_array(*col.child, array->children[0]);
    array->private_data = h;
    array->release = release_exported;
}

bool is_dict_encoded(const dftu_series& col) {
    return col.encoding == Encoding::Selection ||
           col.encoding == Encoding::Dictionary;
}

void export_array(const dftu_series& col, ArrowArray* array) {
    std::memset(array, 0, sizeof(*array));
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
    else if (is_varwidth(col.type))
        export_varwidth(col, array);
    else if (is_varwidth_large(col.type))
        export_varwidth_large(col, array);
    else
        export_flat(col, array);
}

void build_schema(const dftu_series& col, ArrowSchema* schema);

// Sets `schema`'s format (and recurses into children) for `col`'s type,
// WITHOUT calling ArrowSchemaInit/InitFromType: `schema` may already be
// initialized and named (a List/LargeList/FixedSizeList "item" child, or a
// Map's pre-built "entries"/"key"/"value" nodes) and re-initing it would
// orphan that name allocation.
void set_type_in_place(const dftu_series& col, ArrowSchema* schema) {
    if (col.type == TypeId::Struct) {
        ArrowSchemaSetType(schema, NANOARROW_TYPE_STRUCT);
        ArrowSchemaAllocateChildren(
            schema, static_cast<std::int64_t>(col.children.size()));
        for (std::size_t i = 0; i < col.children.size(); ++i) {
            build_schema(*col.children[i], schema->children[i]);
            ArrowSchemaSetName(schema->children[i], col.field_names[i].c_str());
        }
    } else if (col.type == TypeId::List) {
        ArrowSchemaSetType(schema, NANOARROW_TYPE_LIST);
        set_type_in_place(*col.child, schema->children[0]);
    } else if (col.type == TypeId::LargeList) {
        ArrowSchemaSetType(schema, NANOARROW_TYPE_LARGE_LIST);
        set_type_in_place(*col.child, schema->children[0]);
    } else if (col.type == TypeId::FixedSizeList) {
        ArrowSchemaSetTypeFixedSize(schema, NANOARROW_TYPE_FIXED_SIZE_LIST,
                                    col.fixed_size);
        set_type_in_place(*col.child, schema->children[0]);
    } else if (col.type == TypeId::Map) {
        ArrowSchemaSetType(schema, NANOARROW_TYPE_MAP);
        ArrowSchema* entries = schema->children[0];
        set_type_in_place(*col.child->children[0], entries->children[0]);
        set_type_in_place(*col.child->children[1], entries->children[1]);
    } else if (col.type == TypeId::Timestamp || col.type == TypeId::Time32 ||
               col.type == TypeId::Time64 || col.type == TypeId::Duration) {
        const char* tz =
            (col.type == TypeId::Timestamp && !col.timezone.empty())
                ? col.timezone.c_str()
                : nullptr;
        ArrowSchemaSetTypeDateTime(schema, to_arrow_type(col.type),
                                   to_arrow_time_unit(col.time_unit), tz);
    } else if (col.type == TypeId::Decimal128 ||
               col.type == TypeId::Decimal256) {
        ArrowSchemaSetTypeDecimal(schema, to_arrow_type(col.type),
                                  col.decimal_precision, col.decimal_scale);
    } else if (col.type == TypeId::FixedSizeBinary) {
        ArrowSchemaSetTypeFixedSize(schema, NANOARROW_TYPE_FIXED_SIZE_BINARY,
                                    col.fixed_size);
    } else {
        ArrowSchemaSetType(schema, to_arrow_type(col.type));
    }
}

// Initialize a fresh `schema` (root, or a child from AllocateChildren) for
// `col`.
void build_schema(const dftu_series& col, ArrowSchema* schema) {
    ArrowSchemaInit(schema);
    if (is_dict_encoded(col)) {
        // SELECTION indices are int64; DICTIONARY codes are int32.
        ArrowSchemaSetType(schema, col.encoding == Encoding::Selection
                                       ? NANOARROW_TYPE_INT64
                                       : NANOARROW_TYPE_INT32);
        ArrowSchemaAllocateDictionary(schema);
        build_schema(*col.child, schema->dictionary);
    } else {
        set_type_in_place(col, schema);
    }
}

}  // namespace

void to_arrow(const Series& col, ArrowSchema* schema, ArrowArray* array) {
    const dftu_series* c = col.handle();
    if (c == nullptr) {
        // A null handle is an empty result; export a valid empty struct rather
        // than dereferencing it.
        ArrowSchemaInitFromType(schema, NANOARROW_TYPE_STRUCT);
        auto* h = new ExportedArray();
        std::memset(array, 0, sizeof(*array));
        array->length = 0;
        array->n_buffers = 1;
        array->buffers = h->buffers;
        array->private_data = h;
        array->release = release_exported;
        return;
    }
    build_schema(*c, schema);
    export_array(*c, array);
}

namespace {

// `owner` keeps the backing Arrow array alive for as long as any wrapped buffer
// survives, so the imported buffers can alias `arr` zero copy.
Series import_flat(const ArrowSchema* schema, const ArrowArray* arr,
                   std::shared_ptr<void> owner) {
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
        col->time_unit = from_arrow_time_unit(view.time_unit);
        if (type == TypeId::Timestamp && view.timezone != nullptr)
            col->timezone = view.timezone;
    } else if (type == TypeId::Decimal128 || type == TypeId::Decimal256) {
        col->decimal_precision = view.decimal_precision;
        col->decimal_scale = view.decimal_scale;
    } else if (type == TypeId::FixedSizeBinary) {
        col->fixed_size = view.fixed_size;
    }

    auto wrap_validity = [&]() {
        if (arr->buffers[0] != nullptr) {
            auto* val =
                static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[0]));
            col->validity = Buffer::wrap(
                val, (static_cast<std::size_t>(n) + 7) / 8, [owner](void*) {});
        }
    };

    if (is_varwidth(type)) {
        // 3 buffers: validity, int32 offsets (n+1), data. We import offset 0.
        const std::int32_t* offs =
            static_cast<const std::int32_t*>(arr->buffers[1]);
        std::size_t data_len =
            offs != nullptr ? static_cast<std::size_t>(offs[n]) : 0;
        col->offsets = Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1])),
            static_cast<std::size_t>(n + 1) * sizeof(std::int32_t),
            [owner](void*) {});
        col->data = Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[2])),
            data_len, [owner](void*) {});
        wrap_validity();
        return Series{col};
    }

    if (is_varwidth_large(type)) {
        // 3 buffers: validity, int64 offsets (n+1), data.
        const std::int64_t* offs =
            static_cast<const std::int64_t*>(arr->buffers[1]);
        std::size_t data_len =
            offs != nullptr ? static_cast<std::size_t>(offs[n]) : 0;
        col->offsets64 = Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1])),
            static_cast<std::size_t>(n + 1) * sizeof(std::int64_t),
            [owner](void*) {});
        col->data = Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[2])),
            data_len, [owner](void*) {});
        wrap_validity();
        return Series{col};
    }

    // Bool is bit-packed, so its data buffer is sized by buffer_bytes and a
    // (rare) nonzero Arrow offset would be bit-level; we only import offset 0.
    // FixedSizeBinary's per-row width is col->fixed_size, not a per-TypeId
    // constant, so byte_width needs it passed alongside the type.
    std::size_t width = byte_width(type, col->fixed_size).value_or(0);
    std::size_t ptr_off = (type == TypeId::Bool)
                              ? 0
                              : static_cast<std::size_t>(arr->offset) * width;
    auto* data = static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1]));
    std::size_t data_len = (type == TypeId::FixedSizeBinary)
                               ? static_cast<std::size_t>(n) * width
                               : buffer_bytes(type, n);
    col->data = Buffer::wrap(data + ptr_off, data_len, [owner](void*) {});
    wrap_validity();
    return Series{col};
}

Series import_any(const ArrowSchema* schema, const ArrowArray* arr,
                  std::shared_ptr<void> owner);

// LIST and MAP: validity + int32 offsets (n+1), one child array. `map_type`
// selects which TypeId to tag the result with; both share the same wire
// layout (int32 offsets over a child array - the Struct{key, value} entries,
// for MAP), so only the tag differs. Only int32-offset lists and parent
// offset 0 are imported; the child is imported recursively.
Series import_list_like(TypeId result_type, const ArrowSchema* schema,
                        const ArrowArray* arr, std::shared_ptr<void> owner) {
    if (schema->n_children != 1 || arr->n_children != 1) return Series{};
    auto* col = new dftu_series();
    col->type = result_type;
    col->encoding = Encoding::Flat;
    col->length = arr->length;
    col->null_count = arr->null_count < 0 ? 0 : arr->null_count;
    std::int64_t n = arr->length;
    col->offsets = Buffer::wrap(
        static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1])),
        static_cast<std::size_t>(n + 1) * sizeof(std::int32_t),
        [owner](void*) {});
    if (arr->buffers[0] != nullptr)
        col->validity = Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[0])),
            (static_cast<std::size_t>(n) + 7) / 8, [owner](void*) {});
    Series child = import_any(schema->children[0], arr->children[0], owner);
    if (!child.valid()) {
        delete col;
        return Series{};
    }
    col->child = std::shared_ptr<dftu_series>(child.release());
    return Series{col};
}

// LargeList: same shape as import_list_like but with int64 offsets.
Series import_large_list(const ArrowSchema* schema, const ArrowArray* arr,
                         std::shared_ptr<void> owner) {
    if (schema->n_children != 1 || arr->n_children != 1) return Series{};
    auto* col = new dftu_series();
    col->type = TypeId::LargeList;
    col->encoding = Encoding::Flat;
    col->length = arr->length;
    col->null_count = arr->null_count < 0 ? 0 : arr->null_count;
    std::int64_t n = arr->length;
    col->offsets64 = Buffer::wrap(
        static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1])),
        static_cast<std::size_t>(n + 1) * sizeof(std::int64_t),
        [owner](void*) {});
    if (arr->buffers[0] != nullptr)
        col->validity = Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[0])),
            (static_cast<std::size_t>(n) + 7) / 8, [owner](void*) {});
    Series child = import_any(schema->children[0], arr->children[0], owner);
    if (!child.valid()) {
        delete col;
        return Series{};
    }
    col->child = std::shared_ptr<dftu_series>(child.release());
    return Series{col};
}

// FixedSizeList: validity only, no offsets - the child array holds
// length * fixed_size flattened elements. Only parent offset 0 is imported.
Series import_fixed_size_list(const ArrowSchemaView& view,
                              const ArrowSchema* schema, const ArrowArray* arr,
                              std::shared_ptr<void> owner) {
    if (schema->n_children != 1 || arr->n_children != 1) return Series{};
    auto* col = new dftu_series();
    col->type = TypeId::FixedSizeList;
    col->encoding = Encoding::Flat;
    col->length = arr->length;
    col->null_count = arr->null_count < 0 ? 0 : arr->null_count;
    col->fixed_size = view.fixed_size;
    std::int64_t n = arr->length;
    if (arr->n_buffers > 0 && arr->buffers[0] != nullptr)
        col->validity = Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[0])),
            (static_cast<std::size_t>(n) + 7) / 8, [owner](void*) {});
    Series child = import_any(schema->children[0], arr->children[0], owner);
    if (!child.valid()) {
        delete col;
        return Series{};
    }
    col->child = std::shared_ptr<dftu_series>(child.release());
    return Series{col};
}

// Inverse of export_struct; each field column is imported recursively and
// aliases `arr` through `owner`.
Series import_struct(const ArrowSchema* schema, const ArrowArray* arr,
                     std::shared_ptr<void> owner) {
    if (schema->n_children != arr->n_children) return Series{};
    auto* col = new dftu_series();
    col->type = TypeId::Struct;
    col->encoding = Encoding::Flat;
    col->length = arr->length;
    col->null_count = arr->null_count < 0 ? 0 : arr->null_count;
    const std::int64_t n = arr->length;
    if (arr->n_buffers > 0 && arr->buffers[0] != nullptr)
        col->validity = Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[0])),
            (static_cast<std::size_t>(n) + 7) / 8, [owner](void*) {});
    for (std::int64_t i = 0; i < arr->n_children; ++i) {
        Series child = import_any(schema->children[i], arr->children[i], owner);
        if (!child.valid()) {
            delete col;
            return Series{};
        }
        col->children.push_back(std::shared_ptr<dftu_series>(child.release()));
        col->field_names.emplace_back(
            schema->children[i]->name ? schema->children[i]->name : "");
    }
    return Series{col};
}

// Inverse of export_dict. int32 indices map to a DICTIONARY column and int64 to
// a SELECTION column (the widths the engine and exporter use); any other index
// width is copied into int32 DICTIONARY codes.
Series import_dict(const ArrowSchema* schema, const ArrowArray* arr,
                   std::shared_ptr<void> owner) {
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
    col->child = std::shared_ptr<dftu_series>(values.release());
    if (arr->n_buffers > 0 && arr->buffers[0] != nullptr)
        col->validity = Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[0])),
            (static_cast<std::size_t>(n) + 7) / 8, [owner](void*) {});

    if (index_type == TypeId::Int64) {
        col->encoding = Encoding::Selection;
        col->data = Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1])) +
                off * sizeof(std::int64_t),
            static_cast<std::size_t>(n) * sizeof(std::int64_t),
            [owner](void*) {});
        return Series{col};
    }
    if (index_type == TypeId::Int32) {
        col->encoding = Encoding::Dictionary;
        col->data = Buffer::wrap(
            static_cast<std::uint8_t*>(const_cast<void*>(arr->buffers[1])) +
                off * sizeof(std::int32_t),
            static_cast<std::size_t>(n) * sizeof(std::int32_t),
            [owner](void*) {});
        return Series{col};
    }

    // Other index widths (int8/16, uint*): copy into int32 DICTIONARY codes.
    col->encoding = Encoding::Dictionary;
    col->data =
        Buffer::allocate(static_cast<std::size_t>(n) * sizeof(std::int32_t));
    auto* codes = reinterpret_cast<std::int32_t*>(col->data->data());
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
                  std::shared_ptr<void> owner) {
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
    return import_flat(schema, arr, owner);
}

// Move `array` into a shared owner that runs its release once the last wrapped
// buffer is dropped; the caller's array is marked released.
std::shared_ptr<ArrowArray> adopt_array(ArrowArray* array) {
    auto* moved = new ArrowArray(*array);
    array->release = nullptr;
    return std::shared_ptr<ArrowArray>(moved, [](ArrowArray* a) {
        if (a->release != nullptr) a->release(a);
        delete a;
    });
}

}  // namespace

Series from_arrow(const ArrowSchema* schema, ArrowArray* array) {
    std::shared_ptr<ArrowArray> owner = adopt_array(array);
    return import_any(schema, owner.get(), owner);
}

DataFrame dataframe_from_arrow(const ArrowSchema* schema, ArrowArray* array) {
    ArrowSchemaView view;
    ArrowError error;
    if (ArrowSchemaViewInit(&view, schema, &error) != NANOARROW_OK)
        return DataFrame{};
    if (view.type != NANOARROW_TYPE_STRUCT) return DataFrame{};

    std::shared_ptr<ArrowArray> owner = adopt_array(array);
    DataFrame df;
    df.names.reserve(static_cast<std::size_t>(schema->n_children));
    df.columns.reserve(static_cast<std::size_t>(schema->n_children));
    for (std::int64_t i = 0; i < schema->n_children; ++i) {
        const ArrowSchema* cs = schema->children[i];
        df.names.emplace_back(cs->name != nullptr ? cs->name : "");
        df.columns.push_back(import_any(cs, owner->children[i], owner));
    }
    return df;
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_ENABLE_ARROW
