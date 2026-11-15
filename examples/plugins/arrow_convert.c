// SPDX-License-Identifier: MIT
// This is the one file outside the arrow library and the Python module that
// may include nanoarrow: it shows how a plugin turns one dftu_series into an
// ArrowSchema/ArrowArray pair with its own Arrow library and back, sharing the
// buffers (zero copy) wherever the layouts already match.
//
// Every buffer nanoarrow gives out here is borrowed from the column: nanoarrow
// owns no memory, and its release callback drops a reference on the column
// instead. The way back does the same for the ArrowArray, so the column dies
// when the ArrowArray's own release runs.
//
// Build: cmake --preset tests && cmake --build --preset tests --target
// arrow_convert, then run the binary; it round-trips Int64, Float64, String
// (offsets and views) and a dictionary column and returns 0 when every value
// and null matches.

#include <dftracer/utils/dataframe/c_api.h>
#include <nanoarrow/nanoarrow.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Encoding codes of dftu_series_encoding (the dataframe::Encoding ordinals).
enum {
    ENC_FLAT = 0,
    ENC_CONSTANT = 1,
    ENC_DICTIONARY = 2,
    ENC_SELECTION = 3,
    ENC_VIEW = 4,
    ENC_CHUNKED = 5
};

#define WHY_SIZE 256

static int fail(char* why, const char* message) {
    snprintf(why, WHY_SIZE, "%s", message);
    return -1;
}

// Drops what an export already built before a later step fails, so a failed
// export always hands back an empty pair instead of a leak.
static int fail_release(char* why, const char* message,
                        struct ArrowSchema* schema, struct ArrowArray* array) {
    if (schema != NULL && schema->release != NULL) schema->release(schema);
    if (array != NULL && array->release != NULL) array->release(array);
    return fail(why, message);
}

// Drops the column reference a borrowed Arrow buffer holds. nanoarrow calls it
// once per buffer, when the array is released.
static void release_column(struct ArrowBufferAllocator* allocator, uint8_t* ptr,
                           int64_t size) {
    (void)ptr;
    (void)size;
    dftu_series_free((dftu_series*)allocator->private_data);
}

// Hands `array` the `size` bytes at `data` with no copy, holding a reference on
// `col` so the memory outlives the array. One reference per buffer: each is
// dropped by nanoarrow's own release of that buffer.
static int borrow_buffer(struct ArrowArray* array, int64_t index,
                         const void* data, int64_t size,
                         const dftu_series* col) {
    if (data == NULL) return 0;  // optional buffer, left empty
    dftu_series* ref = dftu_series_share(col);
    if (ref == NULL) return -1;

    struct ArrowBuffer buffer;
    // The array only reads the bytes: drop the const qualifier through an
    // integer so no cast-qual warning fires.
    buffer.data = (uint8_t*)(uintptr_t)data;
    buffer.size_bytes = size;
    buffer.capacity_bytes = 0;
    buffer.allocator = ArrowBufferDeallocator(release_column, ref);
    if (ArrowArraySetBuffer(array, index, &buffer) != NANOARROW_OK) {
        release_column(&buffer.allocator, buffer.data, 0);
        return -1;
    }
    return 0;
}

// Copies `size` int64 bytes into an Arrow buffer of its own and hands it to
// `array` at `index` (the StringView sizes buffer has no column buffer behind
// it).
static int copy_int64s(struct ArrowArray* array, int64_t index,
                       const int64_t* values, int32_t count) {
    struct ArrowBuffer buffer;
    ArrowBufferInit(&buffer);
    const int64_t bytes = (int64_t)count * (int64_t)sizeof(int64_t);
    if (ArrowBufferResize(&buffer, bytes, 0) != NANOARROW_OK) {
        ArrowBufferReset(&buffer);
        return -1;
    }
    if (bytes > 0) memcpy(buffer.data, values, (size_t)bytes);
    buffer.size_bytes = bytes;
    if (ArrowArraySetBuffer(array, index, &buffer) != NANOARROW_OK) {
        ArrowBufferReset(&buffer);
        return -1;
    }
    return 0;
}

static int export_fixed(const dftu_series* col, struct ArrowSchema* schema,
                        struct ArrowArray* array, char* why) {
    const int32_t dtype = dftu_series_type(col);
    enum ArrowType type;
    switch (dtype) {
        case DFTU_TYPE_INT64:
            type = NANOARROW_TYPE_INT64;
            break;
        case DFTU_TYPE_FLOAT64:
            type = NANOARROW_TYPE_DOUBLE;
            break;
        default:
            return fail(why, "only Int64 and Float64 are fixed-width here");
    }
    const int64_t n = dftu_series_length(col);
    if (dftu_series_data(col) == NULL) return fail(why, "no value buffer");

    int64_t bit_offset = 0;
    const uint8_t* validity = dftu_series_validity(col, &bit_offset);
    if (bit_offset != 0) return fail(why, "a validity bit offset is not 0");

    if (ArrowSchemaInitFromType(schema, type) != NANOARROW_OK)
        return fail(why, "schema init failed");
    if (ArrowArrayInitFromType(array, type) != NANOARROW_OK)
        return fail(why, "array init failed");
    array->length = n;
    array->null_count = dftu_series_null_count(col);

    const int32_t width = (int32_t)sizeof(int64_t);  // Int64 and Float64
    if (borrow_buffer(array, 0, validity, (n + 7) / 8, col) != 0 ||
        borrow_buffer(array, 1, dftu_series_data(col), n * width, col) != 0)
        return fail_release(why, "buffer borrow failed", schema, array);
    return 0;
}

static int export_offsets(const dftu_series* col, struct ArrowSchema* schema,
                          struct ArrowArray* array, char* why) {
    const int32_t dtype = dftu_series_type(col);
    enum ArrowType type;
    if (dtype == DFTU_TYPE_STRING)
        type = NANOARROW_TYPE_STRING;
    else if (dtype == DFTU_TYPE_BINARY)
        type = NANOARROW_TYPE_BINARY;
    else
        return fail(why, "expected String or Binary");

    const int64_t n = dftu_series_length(col);
    const int32_t* offsets = dftu_series_offsets(col);
    const void* data = dftu_series_data(col);
    if (offsets == NULL && n > 0) return fail(why, "no offsets buffer");
    if (data == NULL && n > 0) return fail(why, "no data buffer");

    int64_t bit_offset = 0;
    const uint8_t* validity = dftu_series_validity(col, &bit_offset);
    if (bit_offset != 0) return fail(why, "a validity bit offset is not 0");

    if (ArrowSchemaInitFromType(schema, type) != NANOARROW_OK)
        return fail(why, "schema init failed");
    if (ArrowArrayInitFromType(array, type) != NANOARROW_OK)
        return fail(why, "array init failed");
    array->length = n;
    array->null_count = dftu_series_null_count(col);

    const int64_t data_size = (offsets != NULL && n > 0) ? offsets[n] : 0;
    if (borrow_buffer(array, 0, validity, (n + 7) / 8, col) != 0 ||
        borrow_buffer(array, 1, offsets, (n + 1) * (int64_t)sizeof(int32_t),
                      col) != 0 ||
        borrow_buffer(array, 2, data, data_size, col) != 0)
        return fail_release(why, "buffer borrow failed", schema, array);
    return 0;
}

static int export_view(const dftu_series* col, struct ArrowSchema* schema,
                       struct ArrowArray* array, char* why) {
    const int32_t dtype = dftu_series_type(col);
    enum ArrowType type;
    if (dtype == DFTU_TYPE_STRING)
        type = NANOARROW_TYPE_STRING_VIEW;
    else if (dtype == DFTU_TYPE_BINARY)
        type = NANOARROW_TYPE_BINARY_VIEW;
    else
        return fail(why, "expected a StringView or BinaryView column");

    const void* views = dftu_series_view_data(col);
    const int32_t n_buffers = dftu_series_view_buffer_count(col);
    if (n_buffers < 0 || views == NULL) return fail(why, "not a VIEW column");

    const int64_t n = dftu_series_length(col);
    int64_t bit_offset = 0;
    const uint8_t* validity = dftu_series_validity(col, &bit_offset);
    if (bit_offset != 0) return fail(why, "a validity bit offset is not 0");

    if (ArrowSchemaInitFromType(schema, type) != NANOARROW_OK)
        return fail(why, "schema init failed");
    if (ArrowArrayInitFromType(array, type) != NANOARROW_OK)
        return fail(why, "array init failed");
    if (n_buffers > 0 &&
        ArrowArrayAddVariadicBuffers(array, n_buffers) != NANOARROW_OK)
        return fail(why, "variadic buffer allocation failed");
    array->length = n;
    array->null_count = dftu_series_null_count(col);

    // The column hands out one data buffer at a time, so this file builds the
    // two arrays an Arrow view array needs itself.
    const size_t slots = n_buffers > 0 ? (size_t)n_buffers : 1;
    const void** buffers = (const void**)calloc(slots, sizeof(*buffers));
    int64_t* sizes = (int64_t*)calloc(slots, sizeof(*sizes));
    if (buffers == NULL || sizes == NULL) {
        free((void*)buffers);
        free(sizes);
        return fail(why, "out of memory");
    }
    for (int32_t k = 0; k < n_buffers; ++k)
        buffers[k] = dftu_series_view_buffer(col, k, &sizes[k]);

    int rc = 0;
    if (borrow_buffer(array, 0, validity, (n + 7) / 8, col) != 0 ||
        borrow_buffer(array, 1, views, n * 16, col) != 0) {
        rc = fail_release(why, "buffer borrow failed", schema, array);
    } else {
        for (int32_t k = 0; k < n_buffers && rc == 0; ++k) {
            if (borrow_buffer(array, 2 + k, buffers[k], sizes[k], col) != 0)
                rc = fail_release(why, "variadic buffer borrow failed", schema,
                                  array);
        }
        // The sizes buffer is int64 per data buffer, and nanoarrow allocates
        // it.
        if (rc == 0 &&
            copy_int64s(array, array->n_buffers - 1, sizes, n_buffers) != 0)
            rc = fail_release(why, "sizes buffer failed", schema, array);
    }
    free((void*)buffers);
    free(sizes);
    return rc;
}

static int export_dictionary(const dftu_series* col, struct ArrowSchema* schema,
                             struct ArrowArray* array, char* why) {
    dftu_series* codes = dftu_series_dictionary_codes(col);
    dftu_series* values = dftu_series_dictionary_values(col);
    if (codes == NULL || values == NULL) {
        dftu_series_free(codes);
        dftu_series_free(values);
        return fail(why, "a dictionary column needs codes and values");
    }
    const int64_t n = dftu_series_length(col);
    int64_t bit_offset = 0;
    const uint8_t* validity = dftu_series_validity(codes, &bit_offset);
    const void* raw = dftu_series_data(codes);
    const int32_t values_type = dftu_series_type(values);

    enum ArrowType value_arrow;
    if (values_type == DFTU_TYPE_STRING)
        value_arrow = NANOARROW_TYPE_STRING;
    else if (values_type == DFTU_TYPE_BINARY)
        value_arrow = NANOARROW_TYPE_BINARY;
    else
        value_arrow = NANOARROW_TYPE_UNINITIALIZED;

    int rc = -1;
    struct ArrowSchema* dict_schema = NULL;
    if (value_arrow == NANOARROW_TYPE_UNINITIALIZED) {
        fail(why, "only a String or Binary dictionary is exported here");
        goto done;
    }
    if (raw == NULL && n > 0) {
        fail(why, "the codes column has no buffer");
        goto done;
    }
    if (ArrowSchemaInitFromType(schema, NANOARROW_TYPE_INT32) != NANOARROW_OK ||
        ArrowArrayInitFromType(array, NANOARROW_TYPE_INT32) != NANOARROW_OK) {
        fail(why, "dictionary init failed");
        goto done;
    }
    array->length = n;
    array->null_count = dftu_series_null_count(codes);
    if (borrow_buffer(array, 0, validity, (n + 7) / 8, codes) != 0 ||
        borrow_buffer(array, 1, raw, n * (int64_t)sizeof(int32_t), codes) !=
            0) {
        fail(why, "codes buffer borrow failed");
        goto done;
    }
    // The index's format is the plain int32 one; the values hang off the
    // schema and the array as their dictionaries.
    dict_schema = (struct ArrowSchema*)ArrowMalloc(sizeof(struct ArrowSchema));
    if (dict_schema == NULL) {
        fail(why, "out of memory");
        goto done;
    }
    if (ArrowSchemaInitFromType(dict_schema, value_arrow) != NANOARROW_OK) {
        ArrowFree(dict_schema);
        dict_schema = NULL;
        fail(why, "dictionary schema init failed");
        goto done;
    }
    schema->dictionary = dict_schema;
    dict_schema = NULL;
    if (ArrowArrayAllocateDictionary(array) != NANOARROW_OK) {
        fail(why, "dictionary array allocation failed");
        goto done;
    }
    {
        struct ArrowSchema values_schema = {0};
        const int values_rc =
            export_offsets(values, &values_schema, array->dictionary, why);
        if (values_schema.release != NULL)
            values_schema.release(&values_schema);
        if (values_rc != 0) {
            fail(why, "dictionary values export failed");
            goto done;
        }
    }
    rc = 0;
done:
    // A failed export hands back an empty pair; a good one keeps both.
    if (rc != 0) {
        if (schema->release != NULL) schema->release(schema);
        if (array->release != NULL) array->release(array);
    }
    ArrowFree(dict_schema);
    dftu_series_free(codes);
    dftu_series_free(values);
    return rc;
}

// Fills `schema` and `array` from `col`, borrowing its buffers. Returns 0 on
// success, or -1 with `why` set and both left released, so the caller releases
// nothing on a failure.
static int dftu_plugin_export_arrow(const dftu_series* col,
                                    struct ArrowSchema* schema,
                                    struct ArrowArray* array, char* why) {
    schema->release = NULL;
    array->release = NULL;
    if (col == NULL) return fail(why, "no column");
    const int32_t encoding = dftu_series_encoding(col);
    if (encoding == ENC_VIEW) return export_view(col, schema, array, why);
    if (encoding == ENC_DICTIONARY)
        return export_dictionary(col, schema, array, why);
    if (encoding != ENC_FLAT)
        return fail(why, "only FLAT, VIEW and DICTIONARY columns export here");
    const int32_t dtype = dftu_series_type(col);
    if (dtype == DFTU_TYPE_STRING || dtype == DFTU_TYPE_BINARY)
        return export_offsets(col, schema, array, why);
    return export_fixed(col, schema, array, why);
}

// Keeps the ArrowArray a column borrowed from alive, and releases it once,
// when the last column that borrowed from it is freed.
struct arrow_holder {
    struct ArrowArray array;
};

static void release_holder(void* ctx) {
    struct arrow_holder* holder = (struct arrow_holder*)ctx;
    if (holder->array.release != NULL) holder->array.release(&holder->array);
    ArrowFree(holder);
}

// Decodes an Arrow dictionary (int32 codes over a String values array) into a
// flat column. The codes have to be resolved, so this one copies.
static dftu_series* import_dictionary(const struct ArrowArray* a, char* why) {
    const struct ArrowArray* values = a->dictionary;
    const int64_t n = a->length;
    const uint8_t* codes_validity = (const uint8_t*)a->buffers[0];
    const int32_t* codes = (const int32_t*)a->buffers[1];
    if (values == NULL || values->n_buffers != 3)
        return (fail(why, "a dictionary needs String values"), NULL);
    if (values->buffers[0] != NULL)
        return (fail(why, "a null dictionary value is not imported here"),
                NULL);
    if (codes == NULL && n > 0)
        return (fail(why, "the dictionary array has no codes"), NULL);
    const int32_t* value_offsets = (const int32_t*)values->buffers[1];
    const char* value_data = (const char*)values->buffers[2];
    if (value_offsets == NULL && values->length > 0)
        return (fail(why, "the dictionary values have no offsets"), NULL);

    int64_t total = 0;
    for (int64_t i = 0; i < n; ++i) {
        if (codes_validity != NULL &&
            ((codes_validity[i >> 3] >> (i & 7)) & 1) == 0)
            continue;
        const int32_t code = codes[i];
        if (code < 0 || code >= values->length)
            return (fail(why, "a dictionary code is out of range"), NULL);
        total += value_offsets[code + 1] - value_offsets[code];
    }

    int32_t* offsets = (int32_t*)malloc((size_t)(n + 1) * sizeof(int32_t));
    char* data = (char*)malloc((size_t)total + 1);
    if (offsets == NULL || data == NULL) {
        free(offsets);
        free(data);
        return (fail(why, "out of memory"), NULL);
    }
    int64_t at = 0;
    offsets[0] = 0;
    for (int64_t i = 0; i < n; ++i) {
        if (codes_validity == NULL ||
            ((codes_validity[i >> 3] >> (i & 7)) & 1) != 0) {
            const int32_t code = codes[i];
            const int32_t size = value_offsets[code + 1] - value_offsets[code];
            memcpy(data + at, value_data + value_offsets[code], (size_t)size);
            at += size;
        }
        offsets[i + 1] = (int32_t)at;
    }
    dftu_series* col = dftu_series_new_string(DFTU_TYPE_STRING, offsets, data,
                                              n, codes_validity);
    free(offsets);
    free(data);
    if (col == NULL) fail(why, "the flat copy was refused");
    return col;
}

// Takes ownership of `array` (released here when the import fails) and returns
// a column borrowing its buffers. Returns NULL with `why` set on an unsupported
// layout.
static dftu_series* dftu_plugin_import_arrow(const struct ArrowSchema* schema,
                                             struct ArrowArray* array,
                                             char* why) {
    if (array == NULL || array->release == NULL) {
        fail(why, "the array is already released");
        return NULL;
    }
    if (schema == NULL || schema->format == NULL) {
        array->release(array);
        fail(why, "the schema is not readable");
        return NULL;
    }
    if (array->offset != 0) {
        array->release(array);
        fail(why, "only an array with offset 0 imports here");
        return NULL;
    }
    struct ArrowError error;
    struct ArrowSchemaView view;
    if (ArrowSchemaViewInit(&view, schema, &error) != NANOARROW_OK) {
        array->release(array);
        fail(why, "the schema is not readable");
        return NULL;
    }
    if (view.type == NANOARROW_TYPE_DICTIONARY && array->dictionary != NULL) {
        dftu_series* decoded = import_dictionary(array, why);
        array->release(array);
        return decoded;
    }
    const bool borrowable = view.type == NANOARROW_TYPE_INT64 ||
                            view.type == NANOARROW_TYPE_DOUBLE ||
                            view.type == NANOARROW_TYPE_STRING ||
                            view.type == NANOARROW_TYPE_BINARY ||
                            view.type == NANOARROW_TYPE_STRING_VIEW ||
                            view.type == NANOARROW_TYPE_BINARY_VIEW;
    if (!borrowable) {
        array->release(array);
        fail(why,
             "only Int64, Float64, String, StringView and a String dictionary "
             "import here");
        return NULL;
    }

    struct arrow_holder* holder =
        (struct arrow_holder*)ArrowMalloc(sizeof(struct arrow_holder));
    if (holder == NULL) {
        array->release(array);
        fail(why, "out of memory");
        return NULL;
    }
    holder->array = *array;
    array->release = NULL;  // moved into the holder

    const struct ArrowArray* a = &holder->array;
    const int64_t n = a->length;
    const uint8_t* validity = (const uint8_t*)a->buffers[0];
    dftu_series* col = NULL;
    switch (view.type) {
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_DOUBLE:
            if (a->buffers[1] == NULL && n > 0) {
                release_holder(holder);
                fail(why, "the array has no value buffer");
                return NULL;
            }
            col = dftu_series_new_flat_borrowed(
                view.type == NANOARROW_TYPE_INT64 ? DFTU_TYPE_INT64
                                                  : DFTU_TYPE_FLOAT64,
                a->buffers[1], n, validity, release_holder, holder);
            break;
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_BINARY: {
            const int32_t* offsets = (const int32_t*)a->buffers[1];
            if (offsets == NULL && n > 0) {
                release_holder(holder);
                fail(why, "the array has no offsets buffer");
                return NULL;
            }
            col = dftu_series_new_string_borrowed(
                view.type == NANOARROW_TYPE_STRING ? DFTU_TYPE_STRING
                                                   : DFTU_TYPE_BINARY,
                offsets, a->buffers[2], n > 0 ? offsets[n] : 0, n, validity,
                release_holder, holder);
            break;
        }
        case NANOARROW_TYPE_STRING_VIEW:
        case NANOARROW_TYPE_BINARY_VIEW: {
            if (a->n_buffers < 3) {
                release_holder(holder);
                fail(why, "the view array has no sizes buffer");
                return NULL;
            }
            col = dftu_series_new_string_view(
                view.type == NANOARROW_TYPE_STRING_VIEW ? DFTU_TYPE_STRING
                                                        : DFTU_TYPE_BINARY,
                a->buffers[1], n, validity,
                (const void* const*)(a->buffers + 2),
                (const int64_t*)a->buffers[a->n_buffers - 1],
                (int32_t)(a->n_buffers - 3), release_holder, holder);
            break;
        }
        default:
            break;
    }
    if (col == NULL) {
        // The constructor ran the holder's release itself on a refusal.
        fail(why, "the borrowed column was refused");
        return NULL;
    }
    return col;
}

// Equal types, lengths, nulls and values (string_at resolves a dictionary).
static int compare_columns(const dftu_series* a, const dftu_series* b,
                           char* why) {
    if (dftu_series_type(a) != dftu_series_type(b))
        return fail(why, "types differ");
    const int64_t n = dftu_series_length(a);
    if (n != dftu_series_length(b)) return fail(why, "lengths differ");
    if (dftu_series_null_count(a) != dftu_series_null_count(b))
        return fail(why, "null counts differ");
    for (int64_t i = 0; i < n; ++i) {
        const int32_t na = dftu_series_is_null(a, i);
        if (na != dftu_series_is_null(b, i)) return fail(why, "nulls differ");
        if (na) continue;
        switch (dftu_series_type(a)) {
            case DFTU_TYPE_INT64:
                if (((const int64_t*)dftu_series_data(a))[i] !=
                    ((const int64_t*)dftu_series_data(b))[i])
                    return fail(why, "Int64 values differ");
                break;
            case DFTU_TYPE_FLOAT64:
                if (((const double*)dftu_series_data(a))[i] !=
                    ((const double*)dftu_series_data(b))[i])
                    return fail(why, "Float64 values differ");
                break;
            default: {
                int64_t la = 0;
                int64_t lb = 0;
                const char* sa = dftu_series_string_at(a, i, &la);
                const char* sb = dftu_series_string_at(b, i, &lb);
                if (sa == NULL || sb == NULL || la != lb ||
                    memcmp(sa, sb, (size_t)la) != 0)
                    return fail(why, "string values differ");
                break;
            }
        }
    }
    return 0;
}

// Exports `col`, imports it back and compares. `*expected_values`/
// `*expected_offsets` (when given) must be the column's own buffer, which the
// array has to borrow, not copy. Releases the schema and array on every path.
static int round_trip(const dftu_series* col, const char* label,
                      const void* expected_values, const void* expected_offsets,
                      char* why) {
    struct ArrowSchema schema = {0};
    struct ArrowArray array = {0};
    int rc = dftu_plugin_export_arrow(col, &schema, &array, why);
    if (rc == 0 && (array.release == NULL || schema.release == NULL)) {
        fail(why, "the export left an empty array or schema");
        rc = -1;
    }
    // A String column keeps its offsets in buffer 1 and its bytes in 2; every
    // other layout here keeps its values in buffer 1.
    const int values_index = expected_offsets != NULL ? 2 : 1;
    if (rc == 0 && expected_values != NULL &&
        array.buffers[values_index] != expected_values) {
        fail(why, "the export copied the values instead of borrowing them");
        rc = -1;
    }
    if (rc == 0 && expected_offsets != NULL &&
        array.buffers[1] != expected_offsets) {
        fail(why, "the export copied the offsets instead of borrowing them");
        rc = -1;
    }
    if (rc != 0) {
        if (array.release != NULL) array.release(&array);
        if (schema.release != NULL) schema.release(&schema);
        fprintf(stderr, "arrow_convert: %s: %s\n", label, why);
        return -1;
    }
    // The import takes the array over and releases it.
    dftu_series* back = dftu_plugin_import_arrow(&schema, &array, why);
    if (schema.release != NULL) schema.release(&schema);
    if (back == NULL) {
        fprintf(stderr, "arrow_convert: %s: %s\n", label, why);
        return -1;
    }
    rc = compare_columns(col, back, why);
    dftu_series_free(back);
    if (rc != 0) fprintf(stderr, "arrow_convert: %s: %s\n", label, why);
    return rc;
}

// Counts one round trip, or one column that could not be built. `fail` returns
// -1, so the count only ever grows.
static void count_round_trip(const dftu_series* col, const char* label,
                             const void* expected_values,
                             const void* expected_offsets, char* why,
                             int* failures) {
    if (col == NULL) {
        fail(why, "the source column was not built");
        fprintf(stderr, "arrow_convert: %s: %s\n", label, why);
        ++*failures;
        return;
    }
    if (round_trip(col, label, expected_values, expected_offsets, why) != 0)
        ++*failures;
}

int main(void) {
    char why[WHY_SIZE] = {0};
    int failures = 0;

    // Int64 with a null row.
    {
        const int64_t values[4] = {10, 20, 30, 40};
        const uint8_t validity[1] = {0x0d};
        dftu_series* col =
            dftu_series_new_flat(DFTU_TYPE_INT64, values, 4, validity);
        // dftu_series_data needs a column, so it is asked only when built.
        count_round_trip(col, "Int64",
                         col != NULL ? dftu_series_data(col) : NULL, NULL, why,
                         &failures);
        dftu_series_free(col);
    }

    // Float64 with a null row.
    {
        const double values[3] = {1.5, 2.5, 3.5};
        const uint8_t validity[1] = {0x03};
        dftu_series* col =
            dftu_series_new_flat(DFTU_TYPE_FLOAT64, values, 3, validity);
        count_round_trip(col, "Float64",
                         col != NULL ? dftu_series_data(col) : NULL, NULL, why,
                         &failures);
        dftu_series_free(col);
    }

    // String with int32 offsets.
    {
        const char data[] =
            "alpha"
            "beta beta"
            "gamma";
        const int32_t offsets[4] = {0, 5, 14, 19};
        dftu_series* col =
            dftu_series_new_string(DFTU_TYPE_STRING, offsets, data, 3, NULL);
        count_round_trip(
            col, "String offsets", col != NULL ? dftu_series_data(col) : NULL,
            col != NULL ? dftu_series_offsets(col) : NULL, why, &failures);
        dftu_series_free(col);
    }

    // StringView: one inline row, two rows out of separate buffers.
    {
        const char blob_a[] = "a value longer than twelve bytes";
        const char blob_b[] = "another one that is long too";
        uint8_t views[3 * 16] = {0};
        const char* texts[3] = {"short", blob_a, blob_b};
        const int32_t sizes[3] = {5, 32, 28};
        for (int i = 0; i < 3; ++i) {
            memcpy(views + i * 16, &sizes[i], sizeof(int32_t));
            if (sizes[i] <= 12) {
                memcpy(views + i * 16 + 4, texts[i], (size_t)sizes[i]);
            } else {
                memcpy(views + i * 16 + 4, texts[i], 4);
                const int32_t index = i - 1;
                memcpy(views + i * 16 + 8, &index, sizeof(int32_t));
                const int32_t offset = 0;
                memcpy(views + i * 16 + 12, &offset, sizeof(int32_t));
            }
        }
        const void* const buffers[2] = {blob_a, blob_b};
        const int64_t buffer_sizes[2] = {(int64_t)sizeof(blob_a),
                                         (int64_t)sizeof(blob_b)};
        dftu_series* col =
            dftu_series_new_string_view(DFTU_TYPE_STRING, views, 3, NULL,
                                        buffers, buffer_sizes, 2, NULL, NULL);
        count_round_trip(col, "StringView", col != NULL ? views : NULL, NULL,
                         why, &failures);
        dftu_series_free(col);
    }

    // A dictionary column: int32 codes over a String values column.
    {
        const char data[] =
            "alpha"
            "beta"
            "gamma";
        const int32_t offsets[4] = {0, 5, 9, 14};
        dftu_series* flat =
            dftu_series_new_string(DFTU_TYPE_STRING, offsets, data, 3, NULL);
        dftu_series* dict =
            flat != NULL ? dftu_series_dictionary_encode(flat) : NULL;
        if (dict == NULL) {
            fail(why, "the dictionary column was not built");
            fprintf(stderr, "arrow_convert: Dictionary: %s\n", why);
            ++failures;
        } else {
            dftu_series* codes = dftu_series_dictionary_codes(dict);
            count_round_trip(dict, "Dictionary",
                             codes != NULL ? dftu_series_data(codes) : NULL,
                             NULL, why, &failures);
            dftu_series_free(codes);
        }
        dftu_series_free(dict);
        dftu_series_free(flat);
    }

    if (failures != 0) {
        fprintf(stderr, "arrow_convert: %d failure(s): %s\n", failures, why);
        return 1;
    }
    printf(
        "arrow_convert: Int64, Float64, String, StringView and Dictionary "
        "round-trip with no copy\n");
    return 0;
}
