// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_C_API_H
#define DFTRACER_UTILS_DATAFRAME_C_API_H

#include <dftracer/utils/common/c_api.h>
#include <dftracer/utils/common/export.h>
#include <dftracer/utils/duql/c_api.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Stable C ABI for the dataframe columnar engine. The handle is opaque; its
 * layout is defined only in the implementation. Type codes match
 * dataframe::TypeId ordinals, encoding codes match dataframe::Encoding
 * ordinals. Every returned column is owned by the caller and freed with
 * dftu_series_free. NULL is returned on an unsupported type or a failed
 * operation.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DFTU_TYPEDEF_DFTU_SERIES
#define DFTU_TYPEDEF_DFTU_SERIES
typedef struct dftu_series dftu_series;
#endif

/** The domain a dftu_scalar carries (its `kind`). */
typedef int32_t dftu_scalar_tag;
enum {
    DFTU_SCALAR_TAG_I64 = 0,
    DFTU_SCALAR_TAG_U64 = 1,
    DFTU_SCALAR_TAG_F64 = 2,
    DFTU_SCALAR_TAG_STR = 3,
    DFTU_SCALAR_TAG_ERR = 4,
    DFTU_SCALAR_TAG_NULL = 5
};

/** A scalar operand, tagged by its domain so a kernel converts it to the
 * column's element type. Pass a signed integer as I64, an unsigned integer as
 * U64, a float as F64, a string as STR (see dftu_scalar_tag).
 *
 * A STR scalar BORROWS `value.s` for the duration of the call: nothing stores a
 * dftu_scalar past the call it was passed to, and the owner of the text (an
 * ExprNode, a caller's string) outlives it. `len` sits in the padding
 * after `kind`, so the struct is 16 bytes and 8-aligned exactly as before and
 * no by-value call site changes.
 *
 * An ERR scalar is what a reducer returns when it refuses its input (see
 * dftu_series_reduce): `value.err` points at a dftu_error whose `message` names
 * the operation and the column type. The error and its message are BORROWED
 * from thread-local storage and stay valid until the next reduction on the same
 * thread, so copy them if they must outlive that. The other value members are
 * not meaningful for an ERR scalar.
 *
 * A NULL scalar means "no value": the C++ Series::min and Series::max return it
 * for a column with no valid row (the C functions keep their 0 result). Its
 * `value` is zero, so a reader that ignores the tag sees 0. */
#ifndef DFTU_TYPEDEF_DFTU_SCALAR
#define DFTU_TYPEDEF_DFTU_SCALAR
typedef struct dftu_scalar dftu_scalar;
#endif
struct dftu_scalar {
    int32_t kind;
    uint32_t len; /**< STR only: byte length of `value.s`; 0 otherwise */
    union {
        int64_t i;
        uint64_t u;
        double d;
        const char* s;
        const dftu_error* err;
    } value;
};

/** Element type of a dftu_series (mirrors dataframe::TypeId ordinals). */
typedef int32_t dftu_dtype;
enum {
    /** Schema-only marker (mirrors TypeId::Unknown): a type not knowable
     * without scanning, or an unset/zero-initialized dftu_dtype. Never the
     * type of a real dftu_series. */
    DFTU_TYPE_UNKNOWN = 0,
    DFTU_TYPE_BOOL = 1,
    DFTU_TYPE_INT8 = 2,
    DFTU_TYPE_INT16 = 3,
    DFTU_TYPE_INT32 = 4,
    DFTU_TYPE_INT64 = 5,
    DFTU_TYPE_UINT8 = 6,
    DFTU_TYPE_UINT16 = 7,
    DFTU_TYPE_UINT32 = 8,
    DFTU_TYPE_UINT64 = 9,
    DFTU_TYPE_FLOAT32 = 10,
    DFTU_TYPE_FLOAT64 = 11,
    DFTU_TYPE_STRING = 12,
    DFTU_TYPE_BINARY = 13,
    DFTU_TYPE_LIST = 14,
    DFTU_TYPE_STRUCT = 15,
    DFTU_TYPE_FLOAT16 = 16,
    DFTU_TYPE_DATE32 = 17,
    DFTU_TYPE_DATE64 = 18,
    DFTU_TYPE_TIME32 = 19,
    DFTU_TYPE_TIME64 = 20,
    DFTU_TYPE_TIMESTAMP = 21,
    DFTU_TYPE_DURATION = 22,
    DFTU_TYPE_DECIMAL128 = 23,
    DFTU_TYPE_DECIMAL256 = 24,
    DFTU_TYPE_FIXED_SIZE_BINARY = 25,
    DFTU_TYPE_LARGE_STRING = 26,
    DFTU_TYPE_LARGE_BINARY = 27,
    DFTU_TYPE_LARGE_LIST = 28,
    DFTU_TYPE_FIXED_SIZE_LIST = 29,
    DFTU_TYPE_MAP = 30
};

/** Second/Milli/Micro/Nano granularity (mirrors dataframe::TimeUnit), for
 * Timestamp, Time32, Time64, and Duration columns. */
typedef int32_t dftu_time_unit;
enum {
    DFTU_TIME_UNIT_SECOND = 0,
    DFTU_TIME_UNIT_MILLI = 1,
    DFTU_TIME_UNIT_MICRO = 2,
    DFTU_TIME_UNIT_NANO = 3
};

/** Create a FLAT column copying `n` values of `type` from `data`. `validity` is
 * an Arrow-layout bitmap (1 = valid) or NULL for no nulls. */
DFTU_EXPORT dftu_series* dftu_series_new_flat(dftu_dtype type, const void* data,
                                              int64_t n,
                                              const uint8_t* validity);

/** Create a FLAT column BORROWING `n` values of `type` from `data` (no copy):
 * the column's data buffer points straight at `data`. `validity` is an
 * Arrow-layout bitmap (1 = valid) or NULL for no nulls, and it is borrowed too
 * (no copy), so both buffers must stay valid until `release(release_ctx)` runs.
 *
 * Ownership: the column borrows `data` and `validity`. `release(release_ctx)`
 * runs exactly once, when the last column that uses them (this one, or any made
 * from it by filter, take, slice or share) is freed, or at once if this
 * function returns NULL; pass NULL `release` to borrow non-owned static memory.
 * `release` may run on any thread. Returns NULL, after running `release`, on a
 * variable-width type (String/Binary), where borrowing a single flat buffer is
 * not meaningful, or on a fixed-width type that is not a byte width (see
 * dftu_series_new_flat). */
DFTU_EXPORT dftu_series* dftu_series_new_flat_borrowed(
    dftu_dtype type, const void* data, int64_t n, const uint8_t* validity,
    void (*release)(void* ctx), void* release_ctx);

/** Create a VIEW column (String or Binary) of `n` Arrow 16-byte views, with no
 * copy of the views or of the data. Each view is an int32 size, then for a size
 * of at most 12 the bytes inline (zero padded), else a 4-byte prefix, an int32
 * index into `buffers` and an int32 offset into that buffer. `views` must hold
 * `n` views and `buffers[k]` `sizes[k]` bytes, all valid until `release(ctx)`
 * runs. `validity` is an Arrow-layout bitmap (1 = valid) or NULL, borrowed like
 * the views, and a null row's view is ignored.
 *
 * Ownership: the column borrows `views` and every buffer. `release(ctx)` runs
 * exactly once, when the last column that uses them (this one, or any made from
 * it by filter, take, slice or share) is freed, or at once if this function
 * returns NULL; pass NULL `release` for static memory. `release` may run on any
 * thread. Returns a column owned by the caller, freed with dftu_series_free.
 * Returns NULL, after running `release`, for a type other than String or
 * Binary, a negative `n`, `n_buffers` or size, a NULL `views` with `n` > 0, a
 * NULL `buffers` or `sizes` with `n_buffers` > 0, or a non-null row whose size
 * is negative, whose buffer index is not below `n_buffers`, or whose offset and
 * size are not within that buffer. */
DFTU_EXPORT dftu_series* dftu_series_new_string_view(
    dftu_dtype type, const void* views, int64_t n, const uint8_t* validity,
    const void* const* buffers, const int64_t* sizes, int32_t n_buffers,
    void (*release)(void* ctx), void* ctx);

/** Create a FLAT variable-width column (String or Binary) copying `n` values.
 * `offsets` has n+1 int32 entries into `data`; `data` holds offsets[n] bytes.
 * A String or Binary column made by dftu_series_slice has offsets that may
 * start above 0 and share the parent's data: value i is
 * data[offsets[i] .. offsets[i+1]], and a reader must not assume
 * offsets[0] == 0.
 * `validity` is an Arrow-layout bitmap (1 = valid) or NULL for no nulls.
 * Returns NULL for another dtype or a negative `offsets[n]` (offsets that
 * wrapped past INT32_MAX). */
DFTU_EXPORT dftu_series* dftu_series_new_string(dftu_dtype type,
                                                const int32_t* offsets,
                                                const void* data, int64_t n,
                                                const uint8_t* validity);

/** Create a FLAT variable-width column BORROWING `offsets`, `data` and
 * `validity` with no copy: String or Binary with n+1 int32 offsets, or
 * LargeString or LargeBinary with n+1 int64 offsets, into `data`, which holds
 * `data_size` usable bytes. Ownership is that of dftu_series_new_string_view:
 * the column borrows all three, `release(ctx)` runs exactly once when the last
 * column that uses them (this one, or any made from it by filter, take, slice
 * or share) is freed, or at once if this function returns NULL; pass NULL
 * `release` for static memory, and `release` may run on any thread. Returns a
 * column owned by the caller, freed with dftu_series_free. Returns NULL, after
 * running `release`, for a type other than those four, a negative `n` or
 * `data_size`, a NULL `offsets` with `n` > 0, a NULL `data` with `data_size` >
 * 0, or a row whose lower offset is negative, whose upper offset is below its
 * lower one, or whose upper offset is past `data_size`. `data` may be NULL with
 * `data_size` 0, as Arrow allows for a column whose rows are all empty. */
DFTU_EXPORT dftu_series* dftu_series_new_string_borrowed(
    dftu_dtype type, const void* offsets, const void* data, int64_t data_size,
    int64_t n, const uint8_t* validity, void (*release)(void* ctx), void* ctx);

/** Create a STRUCT column from `n_fields` child columns (each the same length),
 * named by `names`. Takes ownership of the child columns. */
DFTU_EXPORT dftu_series* dftu_series_new_struct(dftu_series** fields,
                                                const char** names,
                                                int32_t n_fields);

/** Create a LIST column: `offsets` (n+1 int32 entries) index into `values` (the
 * flattened child column). Takes ownership of `values`. */
DFTU_EXPORT dftu_series* dftu_series_new_list(const int32_t* offsets, int64_t n,
                                              dftu_series* values);

DFTU_EXPORT void dftu_series_free(dftu_series* col);

DFTU_EXPORT int32_t dftu_series_type(const dftu_series* col);
/** Storage encoding: 0 flat, 1 constant, 2 dictionary, 3 selection, 4 view,
 * 5 chunked. `dftu_series_data`, `dftu_series_offsets` and
 * `dftu_series_offsets64` return NULL for a chunked column;
 * `dftu_series_materialize` joins it into one column. */
DFTU_EXPORT int32_t dftu_series_encoding(const dftu_series* col);
DFTU_EXPORT int64_t dftu_series_length(const dftu_series* col);
DFTU_EXPORT int64_t dftu_series_null_count(const dftu_series* col);

/** Whether row `i` is null (0/1). A column with no validity bitmap is
 * all-valid.
 */
DFTU_EXPORT int32_t dftu_series_is_null(const dftu_series* col, int64_t i);

/** Bytes of row `i` of a String/Binary/LargeString/LargeBinary column, for a
 * FLAT, VIEW, DICTIONARY, SELECTION or CHUNKED column (resolved through its
 * base, recursing when the base is itself not FLAT). Writes the byte count to
 * `*len` (`len` must not be NULL) and returns a pointer into a buffer owned by
 * `col`: no allocation, valid while `col` lives, not NUL-terminated. Returns
 * NULL with `*len` 0 for a null row, an out-of-range row, or a column of
 * another type. */
DFTU_EXPORT const char* dftu_series_string_at(const dftu_series* col, int64_t i,
                                              int64_t* len);

/** Bytes held by `col`'s own buffers (values or codes, offsets, validity, and
 * for a VIEW column its views and each data buffer once) plus those of its
 * children, including the base of a DICTIONARY or SELECTION column. An
 * in-memory estimate for budgeting; 0 for NULL. */
DFTU_EXPORT int64_t dftu_series_buffer_bytes(const dftu_series* col);

/** Raw FLAT value buffer (or the byte data of a String/Binary column), or NULL
 * when the column is not FLAT. */
DFTU_EXPORT const void* dftu_series_data(const dftu_series* col);

/** int32 offset buffer (length+1 entries) of a String/Binary/List column, or
 * NULL when the column has no 32-bit offsets buffer: a fixed-width type, or a
 * LargeString/LargeBinary/LargeList column, which carries its offsets at
 * dftu_series_offsets64 instead. Exactly one of dftu_series_offsets and
 * dftu_series_offsets64 is non-NULL for a variable-width column. */
DFTU_EXPORT const int32_t* dftu_series_offsets(const dftu_series* col);

/** int64 offset buffer (length+1 entries) of a LargeString/LargeBinary/
 * LargeList column, or NULL otherwise (including a plain String/Binary/List
 * column, whose offsets are dftu_series_offsets instead). */
DFTU_EXPORT const int64_t* dftu_series_offsets64(const dftu_series* col);

/** Arrow-layout validity bitmap (1 = valid, one bit per row) of a FLAT or VIEW
 * column, or NULL when the column has no validity buffer (it may still hold
 * nulls: a slice that starts inside a byte or an import that keeps a bitmap
 * elsewhere reports them through dftu_series_is_null). NULL as well for a NULL
 * column or another encoding (CONSTANT, DICTIONARY, SELECTION, CHUNKED), whose
 * nulls are per row and come from dftu_series_is_null. Writes the bit offset of
 * row 0 (the bit of row 0 inside the returned byte) to `*bit_offset`, 0 when
 * there is no bitmap; writes nothing when `bit_offset` is NULL. Borrowed: valid
 * while `col` lives, with the unused high bits of the last byte undefined. */
DFTU_EXPORT const uint8_t* dftu_series_validity(const dftu_series* col,
                                                int64_t* bit_offset);

/** Number of chunks: the chunk count of a CHUNKED column, 1 for any other
 * encoding (one chunk, the column itself), 0 for a NULL `col`. */
DFTU_EXPORT int32_t dftu_series_chunk_count(const dftu_series* col);

/** Chunk `i` as a new owned column sharing its buffers, freed with
 * dftu_series_free: chunk `i` of a CHUNKED column, or `col` itself for `i` 0 of
 * a column of any other encoding. NULL for a negative or out-of-range `i`, or a
 * NULL `col`. */
DFTU_EXPORT dftu_series* dftu_series_chunk(const dftu_series* col, int32_t i);

/** The codes column of a DICTIONARY column as a new owned FLAT Int32 column
 * sharing its buffer and validity, so row i's value is row codes[i] of
 * dftu_series_dictionary_values(col); freed with dftu_series_free. NULL for
 * another encoding, a NULL `col`, or a DICTIONARY column with no code buffer.
 * The returned column shares memory with `col` and may outlive it. */
DFTU_EXPORT dftu_series* dftu_series_dictionary_codes(const dftu_series* col);

/** The column a DICTIONARY column's codes index into (String or Binary, in any
 * encoding), as a new owned column sharing its buffers and freed with
 * dftu_series_free; a value a code selects may itself be null. NULL for another
 * encoding, a NULL `col`, or a DICTIONARY column with no values. */
DFTU_EXPORT dftu_series* dftu_series_dictionary_values(const dftu_series* col);

/** Number of data buffers of a VIEW (StringView/BinaryView) column: the
 * buffers its 16-byte views point into. -1 when `col` is NULL or is another
 * encoding. */
DFTU_EXPORT int32_t dftu_series_view_buffer_count(const dftu_series* col);

/** The `length()` 16-byte views of a VIEW column (the layout
 * dftu_series_new_string_view takes), or NULL when `col` is NULL, is another
 * encoding, or holds no views. Borrowed: valid while `col` lives. */
DFTU_EXPORT const void* dftu_series_view_data(const dftu_series* col);

/** Data buffer `i` of a VIEW column, with its size in bytes written to `*size`
 * (`size` may be NULL). NULL with `*size` 0 for a NULL `col`, another encoding,
 * or an `i` that is negative or not below dftu_series_view_buffer_count, so a
 * zero-length buffer is the only way to get a NULL pointer with no error.
 * Borrowed: valid while `col` lives. */
DFTU_EXPORT const void* dftu_series_view_buffer(const dftu_series* col,
                                                int32_t i, int64_t* size);

/** Number of child columns: 1 for a List (its flattened values), the field
 * count for a Struct, 0 otherwise. */
DFTU_EXPORT int32_t dftu_series_num_children(const dftu_series* col);

/** Child column `i` (List: only 0, the values; Struct: field i) as a new owned
 * column sharing the child's buffers. NULL if out of range. */
DFTU_EXPORT dftu_series* dftu_series_child(const dftu_series* col, int32_t i);

/** Name of child `i` (a Struct field's name; a List's element has none). NULL
 * if `col` has no name for that child or `i` is out of range. Borrowed,
 * valid while `col` is. */
DFTU_EXPORT const char* dftu_series_field_name(const dftu_series* col,
                                               int32_t i);

/** Time unit (a dftu_time_unit code) of a Timestamp/Time32/Time64/Duration
 * column; DFTU_TIME_UNIT_MICRO for any other type. */
DFTU_EXPORT int32_t dftu_series_time_unit(const dftu_series* col);

/** Timezone of a Timestamp column, or "" (never NULL) for a naive timestamp
 * or any other type. Borrowed, valid while `col` is. */
DFTU_EXPORT const char* dftu_series_timezone(const dftu_series* col);

/** Decimal precision of a Decimal128/Decimal256 column; 0 for any other
 * type. */
DFTU_EXPORT int32_t dftu_series_decimal_precision(const dftu_series* col);

/** Decimal scale of a Decimal128/Decimal256 column; 0 for any other type. */
DFTU_EXPORT int32_t dftu_series_decimal_scale(const dftu_series* col);

/** Element byte width of a FixedSizeBinary column, or element count per row
 * of a FixedSizeList column; 0 for any other type. */
DFTU_EXPORT int32_t dftu_series_fixed_size(const dftu_series* col);

/** 1 when `col` is a String column whose values are canonical JSON text (a
 * JSON column), else 0 (also for NULL). */
DFTU_EXPORT int32_t dftu_series_is_json(const dftu_series* col);

/** A new owned JSON column sharing the String column `col`'s buffers; the
 * caller asserts each value is canonical JSON text. NULL when `col` is NULL
 * or not a String column. */
DFTU_EXPORT dftu_series* dftu_series_mark_json(const dftu_series* col);

/** A new owned column sharing `col`'s buffers zero-copy (a reference-count
 * bump, no data copy). NULL if `col` is NULL. Underpins projection/rename frame
 * ops.
 */
DFTU_EXPORT dftu_series* dftu_series_share(const dftu_series* col);

/** The rows [offset, offset+len) of `col` (both clamped to the column) as a
 * view with the values, nulls, type and type parameters of a gather of those
 * rows. Fixed-width, Bool, String, Binary and their Large forms are windowed
 * in place; VIEW, DICTIONARY and SELECTION columns keep their layout and share
 * their blobs, child or base. A string slice copies nothing: it shares the
 * parent's offsets and data, so its offsets may start above 0; a bit-packed
 * Bool or validity range off a byte boundary is repacked. The result is owned
 * by the caller (free with dftu_series_free) and keeps the base buffers alive
 * after `col` is freed. NULL for a NULL `col` and for a LIST, MAP, STRUCT or
 * CONSTANT column. */
DFTU_EXPORT dftu_series* dftu_series_slice(const dftu_series* col,
                                           int64_t offset, int64_t len);

/** The name of `s`, or NULL when it has none or `s` is NULL (then `*len` is 0;
 * `len` may be NULL). The bytes are not NUL-terminated and are borrowed: they
 * stay valid until `s` is freed. Only dftu_series_share, dftu_series_slice,
 * dftu_series_mark_json and dftu_series_rename carry the name; the result of
 * any other function does not promise one. */
DFTU_EXPORT const char* dftu_series_name(const dftu_series* s, size_t* len);

/** A new owned column sharing `s`'s buffers zero-copy (as dftu_series_share)
 * and named by the `len` bytes at `name`, which are copied. A NULL `name` or a
 * zero `len` gives a column with no name. Free the result with
 * dftu_series_free; it keeps the buffers alive after `s` is freed. NULL for a
 * NULL `s` and on allocation failure. */
DFTU_EXPORT dftu_series* dftu_series_rename(const dftu_series* s,
                                            const char* name, size_t len);

/* The row labels of a column: one label column per level, each as long as the
 * column. A column with none is positional (the pandas RangeIndex, nothing
 * materialized). dftu_series_share, dftu_series_rename, dftu_series_mark_json
 * and dftu_series_slice carry the labels, and the shape-keeping functions of
 * the C++ Series do; a function that reorders or selects rows through this C
 * ABI promises none. */

/** The number of label levels of `s`: 0 when it has none or `s` is NULL. */
DFTU_EXPORT int32_t dftu_series_index_count(const dftu_series* s);
/** Level `i` of `s`: stores its name in `*ptr` (NUL-terminated, not owned) and
 * its byte length in `*len` (`len` may be NULL). The text stays valid until `s`
 * is freed. Returns 0, or -1 and leaves `*ptr` and `*len` untouched when `s` or
 * `ptr` is NULL or `i` is out of range. */
DFTU_EXPORT int32_t dftu_series_index_name(const dftu_series* s, int32_t i,
                                           const char** ptr, size_t* len);
/** A new owned column holding the label column of level `i` of `s`, sharing its
 * buffers zero-copy and carrying no labels of its own. Free it with
 * dftu_series_free. NULL for a NULL `s` and an out-of-range `i`. */
DFTU_EXPORT dftu_series* dftu_series_index_column(const dftu_series* s,
                                                  int32_t i);
/** A new owned column sharing `s`'s buffers (as dftu_series_share) whose labels
 * are the `n` columns in `levels`, named by the `lens[i]` bytes at `names[i]`
 * (copied; not necessarily NUL-terminated). The level columns are shared, not
 * consumed, and a level of type List or Struct is refused. `n` == 0 gives a
 * column with no labels. NULL for a NULL `s`, a NULL pointer with `n` > 0, a
 * negative `n`, a level whose length differs from `s`, an invalid level, a
 * repeated name and on allocation failure. Free the result with
 * dftu_series_free. */
DFTU_EXPORT dftu_series* dftu_series_with_index(
    const dftu_series* s, const char* const* names, const size_t* lens,
    const dftu_series* const* levels, int32_t n);

/** Elementwise binary arithmetic (same numeric type, same length). */
DFTU_EXPORT dftu_series* dftu_series_add(const dftu_series* a,
                                         const dftu_series* b);
DFTU_EXPORT dftu_series* dftu_series_sub(const dftu_series* a,
                                         const dftu_series* b);
DFTU_EXPORT dftu_series* dftu_series_mul(const dftu_series* a,
                                         const dftu_series* b);
/** Elementwise a / b (floats SIMD; integers scalar with a zero guard). */
DFTU_EXPORT dftu_series* dftu_series_div(const dftu_series* a,
                                         const dftu_series* b);

/** Broadcast a scalar across a FLAT numeric column. The scalar is converted to
 * the column's element type, so 64-bit integers stay exact. */
DFTU_EXPORT dftu_series* dftu_series_add_scalar(const dftu_series* a,
                                                dftu_scalar s);
DFTU_EXPORT dftu_series* dftu_series_sub_scalar(const dftu_series* a,
                                                dftu_scalar s);
DFTU_EXPORT dftu_series* dftu_series_mul_scalar(const dftu_series* a,
                                                dftu_scalar s);
DFTU_EXPORT dftu_series* dftu_series_div_scalar(const dftu_series* a,
                                                dftu_scalar s);

/** Floor division, remainder and power with Python's rules: the quotient
 * floors toward minus infinity, the remainder takes the divisor's sign. An
 * integer zero divisor is null; a float zero divisor gives inf or NaN in a
 * floor division and null in a remainder. A negative integer exponent is
 * null. Uint64 when both operands are unsigned and one is Uint64 (in floor
 * division and remainder, a non-negative integer scalar counts as unsigned),
 * Int64 for other integral operands (Bool included), else Float64. The scalar
 * forms take an I64 or F64 scalar; `reverse` nonzero computes `s <op> a`. */
DFTU_EXPORT dftu_series* dftu_series_floordiv(const dftu_series* a,
                                              const dftu_series* b);
DFTU_EXPORT dftu_series* dftu_series_mod(const dftu_series* a,
                                         const dftu_series* b);
DFTU_EXPORT dftu_series* dftu_series_pow(const dftu_series* a,
                                         const dftu_series* b);
DFTU_EXPORT dftu_series* dftu_series_floordiv_scalar(const dftu_series* a,
                                                     dftu_scalar s,
                                                     int32_t reverse);
DFTU_EXPORT dftu_series* dftu_series_mod_scalar(const dftu_series* a,
                                                dftu_scalar s, int32_t reverse);
DFTU_EXPORT dftu_series* dftu_series_pow_scalar(const dftu_series* a,
                                                dftu_scalar s, int32_t reverse);

/** Nulls filled from the nearest present value before them (ffill) or after
 * them (bfill), any column type; a leading (trailing) run of nulls with no
 * such value stays null. A column with no nulls is returned shared. */
DFTU_EXPORT dftu_series* dftu_series_ffill(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_bfill(const dftu_series* v);

/** Comparison ops for dftu_series_compare. */
typedef int32_t dftu_cmp_op;
enum {
    DFTU_CMP_GT = 0,
    DFTU_CMP_GE = 1,
    DFTU_CMP_LT = 2,
    DFTU_CMP_LE = 3,
    DFTU_CMP_EQ = 4,
    DFTU_CMP_NE = 5
};

/** Compare each numeric value of `v` against `rhs` (op is a dftu_cmp_op
 * code); returns a Bool column (bit-packed), carrying v's validity. The scalar
 * is converted to v's element type, so a 64-bit integer compares exactly. */
DFTU_EXPORT dftu_series* dftu_series_compare(const dftu_series* v,
                                             dftu_cmp_op op, dftu_scalar rhs);

/** Compare `a[i]` against `b[i]` row by row (op is a dftu_cmp_op code);
 * returns a Bool column (bit-packed), null where either side is null. Two
 * columns of one numeric type compare in that type, exact for 64-bit
 * integers; mixed numeric types compare as double; String / Binary columns
 * compare their bytes (every op, in byte order). NULL for unequal lengths
 * or a type with no order. */
DFTU_EXPORT dftu_series* dftu_series_compare_series(const dftu_series* a,
                                                    const dftu_series* b,
                                                    dftu_cmp_op op);

/** Rows where the numeric value is greater than `threshold`, as a SELECTION
 * column over `v` (shares v's buffers). */
DFTU_EXPORT dftu_series* dftu_series_filter_gt(const dftu_series* v,
                                               dftu_scalar threshold);

/** Per row, `a[i]` where the bit-packed Bool `mask` is true, else `b[i]`
 * (`cond ? a : b`); the validity bit follows the picked side. `a` and `b`
 * must share a type and `mask`'s length; the result carries `a`'s type
 * parameters. NULL for a nested column or a shape / type mismatch. */
DFTU_EXPORT dftu_series* dftu_series_where(const dftu_series* mask,
                                           const dftu_series* a,
                                           const dftu_series* b);

/** Rows of `v` where the bit-packed Bool `mask` (same length) is true, as a
 * SELECTION column over `v`. */
DFTU_EXPORT dftu_series* dftu_series_filter(const dftu_series* v,
                                            const dftu_series* mask);

/** Cast a FLAT column to `target`. Supported: numeric to numeric (a string
 * parses, unparsable text is null); an integer, float or Bool column to String
 * (decimal digits, shortest float text that shows a point or exponent, `true` /
 * `false`); an integer or float column to Bool (zero false, other true, a NaN
 * and a null null); any non-numeric type to itself. NULL for every other pair
 * (for example String to Bool); the Python wrapper names both types. */
DFTU_EXPORT dftu_series* dftu_series_cast(const dftu_series* v,
                                          dftu_dtype target);

/** Unary numeric primitives for dftu_series_prim (mirrors dataframe::PrimOp).
 */
typedef int32_t dftu_prim_op;
enum {
    DFTU_PRIM_ILOG2 = 0,
    DFTU_PRIM_BIT_WIDTH = 1,
    DFTU_PRIM_POPCOUNT = 2,
    DFTU_PRIM_CLZ = 3,
    DFTU_PRIM_CTZ = 4,
    DFTU_PRIM_MIX64 = 5
};

/** Unary numeric primitive over a FLAT Int64/Uint64 column, producing an Int64
 * column (`op` is a dftu_prim_op). Null if the input is not a 64-bit integer
 * column. */
DFTU_EXPORT dftu_series* dftu_series_prim(const dftu_series* v,
                                          dftu_prim_op op);

/** Boolean ops on bit-packed Bool columns for dftu_series_logical. A null
 * row follows Kleene logic: AND is false when either side is false, OR is
 * true when either side is true, NOT keeps it null, and any other null input
 * gives a null row. */
typedef int32_t dftu_logical_op;
enum { DFTU_LOGICAL_AND = 0, DFTU_LOGICAL_OR = 1 };
DFTU_EXPORT dftu_series* dftu_series_logical(const dftu_series* a,
                                             const dftu_series* b,
                                             dftu_logical_op op);
DFTU_EXPORT dftu_series* dftu_series_logical_not(const dftu_series* a);

/** Dictionary-encode a FLAT String/Binary column: dedup values into a
 * dictionary child, returning a DICTIONARY column of int32 codes. */
DFTU_EXPORT dftu_series* dftu_series_dictionary_encode(const dftu_series* v);

/** String predicates over a String/Binary column (FLAT or DICTIONARY), each
 * returning a Bool column carrying v's validity. A DICTIONARY input evaluates
 * the predicate once per dictionary entry, then maps codes (predicate
 * pushdown), so cost is O(dict + n) rather than O(n * len). */
DFTU_EXPORT dftu_series* dftu_series_str_eq(const dftu_series* v,
                                            const char* rhs, int32_t rhs_len);
DFTU_EXPORT dftu_series* dftu_series_str_contains(const dftu_series* v,
                                                  const char* needle,
                                                  int32_t needle_len);
DFTU_EXPORT dftu_series* dftu_series_str_starts_with(const dftu_series* v,
                                                     const char* prefix,
                                                     int32_t prefix_len);
/** True where the string ends with the literal `suffix`. */
DFTU_EXPORT dftu_series* dftu_series_str_ends_with(const dftu_series* v,
                                                   const char* suffix,
                                                   int32_t suffix_len);
/* The regex kernels below take a pattern in the duql regex dialect (the
 * common subset of RE2, PCRE2 and Vectorscan: no backreferences, lookaround,
 * atomic or possessive groups, recursion or callouts), match strings as
 * UTF-8, and bound each match by a work limit: a row that reaches it is null.
 * A pattern outside the dialect fails to compile. */

/** The capture `group` (0 = the whole match) of the first `pattern` match in
 * each row, as a String column; null where the row is null or does not
 * match (pandas `str.extract`). NULL if the pattern fails to compile or
 * `group` is negative or past the pattern's groups. */
DFTU_EXPORT dftu_series* dftu_series_str_extract(const dftu_series* v,
                                                 const char* pattern,
                                                 int32_t pattern_len,
                                                 int64_t group);
/** Replaces every non-overlapping match of the regex `pattern` in each row
 * with `to`, scanning left to right, as a String column. In `to`, `$n` and
 * `${n}` insert group n, `${name}` the named group, `$$` a dollar sign; a
 * group that took no part inserts nothing. An empty match inserts `to` and
 * advances one character. A row with no match is unchanged; a null row, or a
 * row whose match reaches the work limit, is null. NULL if the pattern fails
 * to compile or `to` is invalid (a group the pattern lacks, or a `$` followed
 * by anything else). */
DFTU_EXPORT dftu_series* dftu_series_str_regex_replace(const dftu_series* v,
                                                       const char* pattern,
                                                       int32_t pattern_len,
                                                       const char* to,
                                                       int32_t to_len);
/** True where the WHOLE string matches the regex `pattern`. NULL if the
 * pattern fails to compile. */
DFTU_EXPORT dftu_series* dftu_series_str_matches(const dftu_series* v,
                                                 const char* pattern,
                                                 int32_t pattern_len);
/** True where the regex `pattern` matches ANYWHERE in the string (duql's `~`).
 * NULL if the pattern fails to compile. */
DFTU_EXPORT dftu_series* dftu_series_str_search(const dftu_series* v,
                                                const char* pattern,
                                                int32_t pattern_len);
/** True where the whole string matches the SQL LIKE `pattern`: `%` matches any
 * run (empty and newlines included), `_` exactly one UTF-8 character, and
 * `\` before `%`, `_` or `\` makes it literal; any other `\` sequence makes
 * the pattern fail (NULL). A Bool column carrying v's validity. */
DFTU_EXPORT dftu_series* dftu_series_str_like(const dftu_series* v,
                                              const char* pattern,
                                              int32_t pattern_len);

/** Per-row byte length (offsets[i+1]-offsets[i]) as an Int64 column, nulls
 * preserved. FLAT input is vectorized (Highway); DICTIONARY is scalar. */
DFTU_EXPORT dftu_series* dftu_series_str_len_bytes(const dftu_series* v);
/** Per-row UTF-8 codepoint count (non-continuation bytes) as an Int64 column,
 * nulls preserved. */
DFTU_EXPORT dftu_series* dftu_series_str_len_chars(const dftu_series* v);
/** Byte index of the first occurrence of literal `needle`, or -1, per row, as
 * an Int64 column. */
DFTU_EXPORT dftu_series* dftu_series_str_find(const dftu_series* v,
                                              const char* needle,
                                              int32_t needle_len);

/** ASCII case fold ('A'-'Z' <-> 'a'-'z'); non-ASCII bytes pass through
 * unchanged. Returns a String column, nulls preserved. FLAT input folds the
 * byte buffer with Highway. */
/** FNV-1a 64-bit hash of each row's bytes as a UInt64 column; null rows stay
 * null. Matches the host's own fnv1a over the same bytes, so a plugin can
 * derive the trace's fhash/hhash columns itself. */
DFTU_EXPORT dftu_series* dftu_series_fnv1a(const dftu_series* v);

/** Parse each row as dftracer's 16-lowercase-hex-digit 64-bit hash form into a
 * UInt64 column; a null row, or one not in that exact form, is null. */
DFTU_EXPORT dftu_series* dftu_series_hex64_parse(const dftu_series* v);

/** Format each row of a UInt64/Int64 column back into dftracer's
 * 16-lowercase-hex-digit form as a String column; null rows stay null. The
 * inverse of dftu_series_hex64_parse. Returns NULL on any other input type. */
DFTU_EXPORT dftu_series* dftu_series_hex64_format(const dftu_series* v);

DFTU_EXPORT dftu_series* dftu_series_to_lowercase(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_to_uppercase(const dftu_series* v);
/** Trim ASCII whitespace from both/left/right ends. String column, nulls
 * preserved. */
DFTU_EXPORT dftu_series* dftu_series_str_strip(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_str_lstrip(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_str_rstrip(const dftu_series* v);
/** Replace the first / all literal occurrence(s) of `pat` with `repl` per row.
 * An empty `pat` leaves the row unchanged. String column, nulls preserved. */
DFTU_EXPORT dftu_series* dftu_series_str_replace(const dftu_series* v,
                                                 const char* pat,
                                                 int32_t pat_len,
                                                 const char* repl,
                                                 int32_t repl_len);
DFTU_EXPORT dftu_series* dftu_series_str_replace_all(const dftu_series* v,
                                                     const char* pat,
                                                     int32_t pat_len,
                                                     const char* repl,
                                                     int32_t repl_len);
/** Substring by BYTE offset. `start` may be negative (from the end); a negative
 * `length` means "to the end". Bounds are clamped. String column, nulls
 * preserved. */
DFTU_EXPORT dftu_series* dftu_series_str_slice(const dftu_series* v,
                                               int64_t start, int64_t length);
/** Left/right pad to `width` bytes with `fill` (rows already >= width are
 * unchanged). String column, nulls preserved. */
DFTU_EXPORT dftu_series* dftu_series_str_pad_start(const dftu_series* v,
                                                   int64_t width, char fill);
DFTU_EXPORT dftu_series* dftu_series_str_pad_end(const dftu_series* v,
                                                 int64_t width, char fill);
/** Left-pad with '0' to `width` bytes, after an optional leading sign (Python
 * zfill). String column, nulls preserved. */
DFTU_EXPORT dftu_series* dftu_series_str_zfill(const dftu_series* v,
                                               int64_t width);
/** Split each row on the literal `sep` into a List<String> column, nulls
 * preserved (a null input row yields a null list). An empty `sep` yields a
 * single element (the whole string). */
DFTU_EXPORT dftu_series* dftu_series_str_split(const dftu_series* v,
                                               const char* sep,
                                               int32_t sep_len);
/** The element count of each row of a FLAT List column, Int64; a null list
 * stays null. NULL if `v` is not a FLAT List. */
DFTU_EXPORT dftu_series* dftu_series_list_len(const dftu_series* v);
/** The element at `index` of each row of a FLAT List column (negative counts
 * from the end), in the element type; null where the row is null or the
 * list is too short. NULL if `v` is not a FLAT List. */
DFTU_EXPORT dftu_series* dftu_series_list_get(const dftu_series* v,
                                              int64_t index);

/** The ASCII case forms of dftu_series_str_case: the first letter upper and
 * the rest lower (CAPITALIZE), each run of letters so (TITLE_CASE), every
 * letter's case flipped (SWAPCASE). Bytes outside ASCII are left as they are.
 */
typedef int32_t dftu_str_case;
enum {
    DFTU_STR_CAPITALIZE = 0,
    DFTU_STR_TITLE_CASE = 1,
    DFTU_STR_SWAPCASE = 2
};
/** `op` is a dftu_str_case. */
DFTU_EXPORT dftu_series* dftu_series_str_case(const dftu_series* v, int32_t op);

/** The character classes of dftu_series_str_is, Python's str.isalnum /
 * isalpha / isdigit / isdecimal / isnumeric / isspace / islower / isupper /
 * istitle over ASCII (DIGIT, DECIMAL and NUMERIC coincide there): a Bool
 * column, false for an empty string, null where the row is. */
typedef int32_t dftu_str_class;
enum {
    DFTU_STR_ALNUM = 0,
    DFTU_STR_ALPHA = 1,
    DFTU_STR_DIGIT = 2,
    DFTU_STR_DECIMAL = 3,
    DFTU_STR_NUMERIC = 4,
    DFTU_STR_SPACE = 5,
    DFTU_STR_LOWER = 6,
    DFTU_STR_UPPER = 7,
    DFTU_STR_TITLE = 8
};
/** `cls` is a dftu_str_class. */
DFTU_EXPORT dftu_series* dftu_series_str_is(const dftu_series* v, int32_t cls);

/** Non-overlapping occurrences of the literal `pat` per row, Int64 (an empty
 * pattern counts len + 1, as Python). */
DFTU_EXPORT dftu_series* dftu_series_str_count(const dftu_series* v,
                                               const char* pat,
                                               int32_t pat_len);
/** Byte index of the LAST `needle` per row, or -1, as Int64. */
DFTU_EXPORT dftu_series* dftu_series_str_rfind(const dftu_series* v,
                                               const char* needle,
                                               int32_t needle_len);
/** Each row without a leading `prefix` (trailing `suffix`) when it has one. */
DFTU_EXPORT dftu_series* dftu_series_str_remove_prefix(const dftu_series* v,
                                                       const char* prefix,
                                                       int32_t len);
DFTU_EXPORT dftu_series* dftu_series_str_remove_suffix(const dftu_series* v,
                                                       const char* suffix,
                                                       int32_t len);
/** Each row repeated `n` times (n >= 0). */
DFTU_EXPORT dftu_series* dftu_series_str_repeat(const dftu_series* v,
                                                int64_t n);
/** Each row centered in `width` bytes with `fill` on both sides (Python's
 * str.center placement). */
DFTU_EXPORT dftu_series* dftu_series_str_center(const dftu_series* v,
                                                int64_t width, char fill);
/** Row-wise concatenation `a[i] + b[i]` of two String columns of one length;
 * null where either is. */
DFTU_EXPORT dftu_series* dftu_series_str_cat(const dftu_series* a,
                                             const dftu_series* b);
/** Every non-overlapping match of the duql regex `pattern` per row, as a
 * List<String> (an empty list where nothing matches; a null row, or one that
 * reaches the match work limit, is null). NULL for a pattern that does not
 * compile. */
DFTU_EXPORT dftu_series* dftu_series_str_findall(const dftu_series* v,
                                                 const char* pattern,
                                                 int32_t pattern_len);
/** Each row split at the first (`from_right` zero) or last occurrence of the
 * literal `sep` into a three-element List<String> (head, sep, tail); with no
 * occurrence (row, "", "") or ("", "", row) from the right, as Python. */
DFTU_EXPORT dftu_series* dftu_series_str_partition(const dftu_series* v,
                                                   const char* sep,
                                                   int32_t sep_len,
                                                   int32_t from_right);

/** The Unicode normalization forms of dftu_series_str_normalize (Python's
 * unicodedata.normalize). */
typedef int32_t dftu_norm_form;
enum {
    DFTU_NORM_NFC = 0,
    DFTU_NORM_NFKC = 1,
    DFTU_NORM_NFD = 2,
    DFTU_NORM_NFKD = 3
};
/** Each row of a String column in the normalization `form` (a
 * dftu_norm_form). A row that is not valid UTF-8 is null, as a null row stays.
 * NULL for another input type or a bad `form`. */
DFTU_EXPORT dftu_series* dftu_series_str_normalize(const dftu_series* v,
                                                   int32_t form);
/** Each codepoint of every row of a String column replaced as the table says
 * (Python's str.translate): `from` is a String column whose rows are each one
 * codepoint, all distinct and none null; `to` is a String column of the same
 * length, where a null row deletes that codepoint and any other row (possibly
 * empty, possibly several codepoints) replaces it. A codepoint not in the
 * table is kept. A row that is not valid UTF-8 is null, as a null row stays.
 * NULL for another input type or an invalid table. */
DFTU_EXPORT dftu_series* dftu_series_str_translate(const dftu_series* v,
                                                   const dftu_series* from,
                                                   const dftu_series* to);
/** The flag bits of dftu_series_str_wrap, the keyword arguments of Python's
 * textwrap.TextWrapper. */
typedef int32_t dftu_wrap_flag;
enum {
    DFTU_WRAP_EXPAND_TABS = 1,
    DFTU_WRAP_REPLACE_WHITESPACE = 2,
    DFTU_WRAP_DROP_WHITESPACE = 4,
    DFTU_WRAP_BREAK_LONG_WORDS = 8,
    DFTU_WRAP_BREAK_ON_HYPHENS = 16
};
/** Each row of a String column as "\n".join(textwrap.wrap(row, width)) of
 * CPython 3.13.10, 3.14.1 and later, with the options in `flags` (an OR of
 * dftu_wrap_flag); `width` counts codepoints.
 * A row that is not valid UTF-8 is null, as a null row stays. NULL for another
 * input type or a `width` below 1. */
DFTU_EXPORT dftu_series* dftu_series_str_wrap(const dftu_series* v,
                                              int64_t width, int32_t flags);
/** The calendar parts of dftu_series_dt_part (the pandas .dt accessor):
 * DAY_OF_WEEK is Monday = 0, DAY_OF_YEAR and ISO_WEEK are 1-based, IS_LEAP_YEAR
 * is 0 / 1, EPOCH_DAYS the day number since 1970-01-01. */
typedef int32_t dftu_dt_part;
enum {
    DFTU_DT_YEAR = 0,
    DFTU_DT_MONTH = 1,
    DFTU_DT_DAY = 2,
    DFTU_DT_HOUR = 3,
    DFTU_DT_MINUTE = 4,
    DFTU_DT_SECOND = 5,
    DFTU_DT_MILLISECOND = 6,
    DFTU_DT_MICROSECOND = 7,
    DFTU_DT_NANOSECOND = 8,
    DFTU_DT_DAY_OF_WEEK = 9,
    DFTU_DT_DAY_OF_YEAR = 10,
    DFTU_DT_QUARTER = 11,
    DFTU_DT_IS_LEAP_YEAR = 12,
    DFTU_DT_DAYS_IN_MONTH = 13,
    DFTU_DT_ISO_WEEK = 14,
    DFTU_DT_ISO_YEAR = 15,
    DFTU_DT_EPOCH_DAYS = 16
};
/** One calendar `part` (a dftu_dt_part) per row as Int64, of a Timestamp /
 * Date32 / Date64 / Duration column in its own unit, or of an Int64 column
 * read in `unit` (a dftu_time_unit). A Timestamp column with a timezone gives
 * the parts of its wall time in that zone; a naive column, an Int64 column and
 * a UTC one give the UTC parts. Before the epoch the day still floors
 * correctly. NULL for another type, a bad code, or a zone that is not in the
 * time zone database (see dftu_tz_take_error). */
DFTU_EXPORT dftu_series* dftu_series_dt_part(const dftu_series* v, int32_t part,
                                             int32_t unit);
/** Each instant of a Timestamp / Date32 / Date64 / Duration column (its own
 * unit) or an Int64 column read in `unit` (a dftu_time_unit) as text by the
 * strftime-style `fmt` (fmt_len bytes): %Y %y %m %d %H %I %M %S %f (six-digit
 * microseconds) %j %a %A %b %B %p %F %T %s (epoch seconds, floored) %z %Z %%.
 * A Timestamp column with a timezone prints its wall time in that zone, %z as
 * the offset (+0530, and seconds when the zone has them) and %Z as the zone's
 * abbreviation at that instant; every other input prints UTC, %z as +0000 and
 * %Z as UTC. A null row stays null. NULL for another input type, a bad `unit`,
 * a NULL `fmt` with a positive length, an unknown directive, a trailing '%',
 * or a zone that is not in the database. The caller frees the result. */
DFTU_EXPORT dftu_series* dftu_series_dt_format(const dftu_series* v,
                                               const char* fmt, int32_t fmt_len,
                                               int32_t unit);
/** How dftu_series_dt_round rounds to a bucket. */
typedef int32_t dftu_dt_round_mode;
enum { DFTU_DT_FLOOR = 0, DFTU_DT_CEIL = 1, DFTU_DT_ROUND = 2 };
/** Each instant rounded to a multiple of `every` (in the column's own unit;
 * an Int64 column is taken as it is) by `mode` (a dftu_dt_round_mode; ROUND
 * is half to even, as pandas). A Timestamp column with a timezone rounds its
 * wall time and maps the result back to UTC; a wall time that happens twice
 * or never makes the call fail, as pandas. The result keeps the input's type.
 * NULL for a date column, another non-instant type, `every` <= 0, a bad code,
 * or a zone failure (see dftu_tz_take_error). */
DFTU_EXPORT dftu_series* dftu_series_dt_round(const dftu_series* v,
                                              int64_t every, int32_t mode);
/** How dftu_series_dt_as_unit rounds to a coarser unit. */
typedef int32_t dftu_dt_unit_rounding;
enum {
    /** Floor toward -infinity, as pandas `as_unit`. */
    DFTU_DT_UNIT_FLOOR = 0,
    /** As Polars `cast_time_unit`: a Timestamp floors, a Duration truncates
     * toward zero. */
    DFTU_DT_UNIT_POLARS = 1
};
/** A Timestamp or Duration column in another `unit` (a dftu_time_unit),
 * keeping the instant, with the zone kept; a coarser unit rounds by `mode` (a
 * dftu_dt_unit_rounding). NULL for another type, a bad `unit` or `mode`, or
 * when a valid row does not fit Int64 in the finer unit (an error, never a
 * wrap). */
DFTU_EXPORT dftu_series* dftu_series_dt_as_unit(const dftu_series* v,
                                                int32_t unit, int32_t mode);
/** The calendar day of each Timestamp (its own unit; a column with a timezone
 * gives the day of its wall time in that zone, else the UTC day) or Date64 row
 * as Date32, floored toward -infinity. NULL for another type, a day outside
 * Int32 or a zone that is not in the database. */
DFTU_EXPORT dftu_series* dftu_series_dt_date(const dftu_series* v);
/** The time of day of each Timestamp row, since midnight in the column's own
 * unit (the wall time in the column's zone, or UTC when it has none): Time32
 * for seconds and milliseconds, Time64 for microseconds and nanoseconds. NULL
 * for another type or a zone that is not in the database. */
DFTU_EXPORT dftu_series* dftu_series_dt_time(const dftu_series* v);
/** The same instants under the timezone `tz` (a zone name such as "UTC",
 * "Asia/Tokyo" or "+05:30"; `tz_len` bytes, 0 for naive): the Timestamp type's
 * zone is replaced and no tick moves, as pandas `tz_convert`. The buffers are
 * shared. NULL for another type, or for a name that is not in the time zone
 * database (see dftu_tz_take_error). */
DFTU_EXPORT dftu_series* dftu_series_with_timezone(const dftu_series* v,
                                                   const char* tz,
                                                   int32_t tz_len);
/** Sets the directories searched for time zone files first, as `paths`
 * (`len` bytes, not NUL terminated) separated as in PATH (':', and ';' on
 * Windows). The library then also
 * searches DFTRACER_UTILS_TZDIR, TZDIR and the system zoneinfo directories. A
 * NULL `paths` or a `len` of 0 or less clears the list. Zones already loaded
 * stay as they are. The text is copied, so the caller keeps ownership. Thread
 * safe. */
DFTU_EXPORT void dftu_tz_set_search_path(const char* paths, int32_t len);
/** How dftu_series_tz_localize reads a wall time that happens twice (the
 * clock stepping back). */
typedef int32_t dftu_tz_ambiguous;
enum {
    /** The call fails. */
    DFTU_TZ_AMBIGUOUS_RAISE = 0,
    /** The first of the two instants (the daylight time side). */
    DFTU_TZ_AMBIGUOUS_EARLIEST = 1,
    /** The second of the two instants (the standard time side). */
    DFTU_TZ_AMBIGUOUS_LATEST = 2,
    /** The row becomes null. */
    DFTU_TZ_AMBIGUOUS_NAT = 3,
    /** As pandas "infer": in each run of consecutive ambiguous rows the rows
     * before the wall time steps back take the first instant and the rest the
     * second; a run needs exactly one step back. */
    DFTU_TZ_AMBIGUOUS_INFER = 4
};
/** How dftu_series_tz_localize reads a wall time that never happens (the
 * clock stepping forward). */
typedef int32_t dftu_tz_nonexistent;
enum {
    /** The call fails. */
    DFTU_TZ_NONEXISTENT_RAISE = 0,
    /** The first instant after the gap. */
    DFTU_TZ_NONEXISTENT_SHIFT_FORWARD = 1,
    /** One tick before the gap. */
    DFTU_TZ_NONEXISTENT_SHIFT_BACKWARD = 2,
    /** The row becomes null. */
    DFTU_TZ_NONEXISTENT_NAT = 3,
    /** The wall time plus `shift` ticks, read at the offset after the gap; the
     * call fails when that is still not a single instant. */
    DFTU_TZ_NONEXISTENT_SHIFT = 4
};
/** Wall time to UTC, pandas `tz_localize`. A naive Timestamp column read as
 * wall times in the zone `tz` becomes a column in that zone, its ticks the
 * UTC instants; `ambiguous` and `nonexistent` (dftu_tz_ambiguous and
 * dftu_tz_nonexistent) say what a wall time that happens twice or never
 * becomes, and `shift` (ticks of the column's unit) is the shift of
 * DFTU_TZ_NONEXISTENT_SHIFT. A column with a timezone and an empty `tz` becomes
 * the naive column of its wall times. A naive column with an empty `tz`
 * comes back as it is. NULL for a column that is not a Timestamp, a column with
 * a timezone and a non-empty `tz`, a bad code, a zone that is not in the
 * database, a wall time that the options refuse, or a wall time whose instant
 * does not fit Int64 (see dftu_tz_take_error). The caller frees the result. */
DFTU_EXPORT dftu_series* dftu_series_tz_localize(const dftu_series* v,
                                                 const char* tz, int32_t tz_len,
                                                 int32_t ambiguous,
                                                 int32_t nonexistent,
                                                 int64_t shift);
/** dftu_series_tz_localize with its options in one Int64, for the column op
 * table: `options` is `ambiguous | nonexistent << 8 | shift_ns << 16`, the
 * shift in nanoseconds, so it must lie within +-2^47 ns (about 39 hours); the
 * caller that packs it checks that. NULL, with a DFTU_TZ_ERROR_OTHER failure,
 * for a shift that is not a whole number of the column's ticks. */
DFTU_EXPORT dftu_series* dftu_series_tz_localize_opts(const dftu_series* v,
                                                      const char* tz,
                                                      int32_t tz_len,
                                                      int64_t options);
/** The kinds of failure dftu_tz_take_error reports. */
typedef int32_t dftu_tz_error_kind;
enum {
    DFTU_TZ_ERROR_NONE = 0,
    /** The zone name is well formed and in no directory searched. */
    DFTU_TZ_ERROR_UNKNOWN_ZONE = 1,
    /** The zone name is malformed. */
    DFTU_TZ_ERROR_INVALID_ZONE = 2,
    DFTU_TZ_ERROR_AMBIGUOUS = 3,
    DFTU_TZ_ERROR_NONEXISTENT = 4,
    DFTU_TZ_ERROR_OUT_OF_RANGE = 5,
    DFTU_TZ_ERROR_OTHER = 6
};
/** The last time zone failure of the calling thread, then cleared: a kind
 * (dftu_tz_error_kind, DFTU_TZ_ERROR_NONE when the last time zone call on this
 * thread did not fail) and, when `message` is not NULL, a NUL-terminated text
 * in `*message` that stays valid until the next time zone call on this thread.
 * The calls that report a failure through a NULL result are the time zone
 * calls of this header and the calendar kernels of a zone-aware column. */
DFTU_EXPORT int32_t dftu_tz_take_error(const char** message);
/** Each row's List<String> joined with `sep` into one String; a null list
 * stays null. NULL unless `v` is a FLAT List of strings. */
DFTU_EXPORT dftu_series* dftu_series_list_join(const dftu_series* v,
                                               const char* sep,
                                               int32_t sep_len);

/* Reduce a column to a scalar (op is a dftu_reduce_op code), skipping nulls.
 * SUM accumulates in the widest type of the column's domain (i64/u64/f64) so it
 * does not overflow narrow types or lose integer precision; MIN/MAX return a
 * value in the column's domain.
 *
 * A Bool column counts true as 1 and reduces as int64: SUM is the number of
 * true values, MIN/MAX are 0 or 1. MIN/MAX of a String or LargeString column
 * return a STR scalar borrowing the smallest or largest value (bytewise) from
 * the column, empty when no value is valid; SUM of a string is refused. A
 * Duration column also has SUM; every other temporal column has MIN/MAX only.
 *
 * A reduction the engine does not define (an op other than SUM/MIN/MAX, or a
 * type the op has no meaning for) returns an ERR scalar, never a value: check
 * `kind == DFTU_SCALAR_TAG_ERR` before reading the result. A column with no
 * valid value reduces to 0, as before. */
/** Aggregation ops. A bitmask (power-of-two values) so group_by can compute
 * several in one pass; dftu_series_reduce takes a single flag (SUM/MIN/MAX). */
typedef int32_t dftu_reduce_op;
enum {
    DFTU_REDUCE_SUM = 1,
    DFTU_REDUCE_MIN = 2,
    DFTU_REDUCE_MAX = 4,
    DFTU_REDUCE_COUNT = 8,
    DFTU_REDUCE_MEAN = 16
};
DFTU_EXPORT dftu_scalar dftu_series_reduce(const dftu_series* v,
                                           dftu_reduce_op op);

/** Count of non-null rows. */
DFTU_EXPORT int64_t dftu_series_count(const dftu_series* v);

/** Product of the non-null values (accumulated in the column's wide domain,
 * like SUM). Read with scalar_value<T>(). */
DFTU_EXPORT dftu_scalar dftu_series_product(const dftu_series* v);

/** Boolean reductions over a bit-packed Bool column, skipping nulls: `all` is
 * 1 when every non-null bit is set (1 for an all-null/empty column), `any` is
 * 1 when at least one non-null bit is set. */
DFTU_EXPORT int32_t dftu_series_all(const dftu_series* v);
DFTU_EXPORT int32_t dftu_series_any(const dftu_series* v);

/** Row index of the minimum / maximum non-null value (first on ties), or -1
 * when the column has no non-null rows. */
DFTU_EXPORT int64_t dftu_series_arg_min(const dftu_series* v);
DFTU_EXPORT int64_t dftu_series_arg_max(const dftu_series* v);

/** Most frequent non-null value (hash-based; first-seen wins on ties). Read
 * with scalar_value<T>(). */
DFTU_EXPORT dftu_scalar dftu_series_mode(const dftu_series* v);

/** Group `values` by `keys` (equal length) in one pass. `op_mask` is a
 * bitwise-or of DFTU_REDUCE_* flags. Sets `*out_keys` to the distinct keys
 * (first-seen order) and fills `out_values[]` with one aggregate column per set
 * flag, in flag order (sum, min, max, count, mean), up to `max_values`; returns
 * the number written. The caller owns every returned column. SUM keeps the
 * value's wide domain type, COUNT is int64, MEAN is float64, MIN/MAX keep the
 * value type. */
DFTU_EXPORT int32_t dftu_series_group_by(
    const dftu_series* keys, const dftu_series* values, int32_t op_mask,
    dftu_series** out_keys, dftu_series** out_values, int32_t max_values);

/** Materialize any encoding to a new FLAT column (gather). A VIEW String or
 * Binary column becomes a FLAT one with int32 offsets, or with int64 offsets
 * (LargeString or LargeBinary) when its bytes exceed INT32_MAX. */
DFTU_EXPORT dftu_series* dftu_series_materialize(const dftu_series* v);

/** Gather `n` rows of `v` at row indices `idx` into a new FLAT column. Handles
 * every column type (nested List/Struct included) and propagates validity. A
 * negative index gathers a null (the join's outer fill); an index at or past
 * the length is refused (NULL). */
DFTU_EXPORT dftu_series* dftu_series_take(const dftu_series* v,
                                          const int64_t* idx, int64_t n);
/** dftu_series_take with an int32 count (the I64LIST operand form). */
DFTU_EXPORT dftu_series* dftu_series_take_i32(const dftu_series* v,
                                              const int64_t* idx, int32_t n);

/** Stable argsort: an Int64 column of row indices ordering `v` ascending, or
 * descending when `descending` is nonzero. Null rows sort last in both
 * directions. The caller owns the returned column. */
DFTU_EXPORT dftu_series* dftu_series_argsort(const dftu_series* v,
                                             int32_t descending);
/** The same with the nulls placed by `nulls_last`: last when nonzero, first
 * otherwise. Ties and nulls keep input order in every mode. The caller frees
 * the Int64 result with dftu_series_free. NULL if `v` is NULL or has a nested
 * type (no order). */
DFTU_EXPORT dftu_series* dftu_series_argsort_nulls(const dftu_series* v,
                                                   int32_t descending,
                                                   int32_t nulls_last);

/** Summary statistics over the non-null values of a FLAT column. Moment stats
 * are numeric-only; quantile sorts (SIMD); nunique/unique work for any type.
 * `sample` nonzero divides by n - 1. variance and stddev give NaN for no valid
 * value and, for the sample form, for one; 0.0 for a NULL `v` or a type with no
 * numeric value. */
DFTU_EXPORT double dftu_series_variance(const dftu_series* v, int32_t sample);
DFTU_EXPORT double dftu_series_stddev(const dftu_series* v, int32_t sample);
DFTU_EXPORT double dftu_series_skewness(const dftu_series* v);
DFTU_EXPORT double dftu_series_kurtosis(const dftu_series* v);
/** The pandas sample skewness G1 (NaN below three values) and sample excess
 * kurtosis G2 (NaN below four values); NaN for a column with no spread. 0.0 if
 * `v` is NULL or not numeric. Nothing is allocated for the caller. */
DFTU_EXPORT double dftu_series_skewness_unbiased(const dftu_series* v);
DFTU_EXPORT double dftu_series_kurtosis_unbiased(const dftu_series* v);
/** How dftu_series_quantile_interp picks a quantile between two ranked values.
 */
typedef int32_t dftu_interpolation;
enum {
    DFTU_INTERP_LINEAR = 0,
    DFTU_INTERP_NEAREST = 1,
    DFTU_INTERP_LOWER = 2,
    DFTU_INTERP_HIGHER = 3,
    DFTU_INTERP_MIDPOINT = 4
};

/** The exact `q` quantile (q clamped to [0, 1]) of the non-null values with
 * linear interpolation; NaN when there is no value or `q` is NaN. A NaN value
 * ranks above every number. Returns 0.0 when `v` is NULL. Nothing is allocated
 * for the caller. */
DFTU_EXPORT double dftu_series_quantile(const dftu_series* v, double q);
/** Like dftu_series_quantile, with `interp` (a dftu_interpolation) choosing the
 * result when `q * (n - 1)` falls between two ranks: LINEAR blends them,
 * NEAREST rounds the rank half away from zero, LOWER and HIGHER take the rank
 * below or above, MIDPOINT their mean. An unknown `interp` or a NaN `q` gives
 * NaN; a NULL `v` gives 0.0. */
DFTU_EXPORT double dftu_series_quantile_interp(const dftu_series* v, double q,
                                               dftu_interpolation interp);
DFTU_EXPORT int64_t dftu_series_nunique(const dftu_series* v);
/** The distinct non-null values, ascending. Caller owns the column. */
DFTU_EXPORT dftu_series* dftu_series_unique(const dftu_series* v);
/** The distinct values, a null kept once: in first-occurrence order when
 * `maintain_order` is nonzero, otherwise ascending with the null first. The
 * caller frees the result with dftu_series_free. NULL if `v` is NULL or has a
 * nested type. */
DFTU_EXPORT dftu_series* dftu_series_unique_nulls(const dftu_series* v,
                                                  int32_t maintain_order);

/** Elementwise transforms -> a new FLAT column (caller owns); abs of a Bool
 * column shares the input in its own encoding. abs/clip/round are SIMD;
 * fillna clears nulls; cumsum is a running sum. */
DFTU_EXPORT dftu_series* dftu_series_abs(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_clip(const dftu_series* v, dftu_scalar lo,
                                          dftu_scalar hi);
DFTU_EXPORT dftu_series* dftu_series_round(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_fillna(const dftu_series* v,
                                            dftu_scalar fill);
/** Replace each NaN of a Float32 or Float64 column with `fill`, converted to
 * the column's type; a NULL scalar turns the NaN into a null. A null row stays
 * null and a column of any other type, or a float column with no NaN, is
 * returned as it is (shared, as dftu_series_share). The caller owns the result
 * and frees it with dftu_series_free. NULL for a NULL `v` and for an ERR or
 * string scalar on a float column. */
DFTU_EXPORT dftu_series* dftu_series_fill_nan(const dftu_series* v,
                                              dftu_scalar fill);
DFTU_EXPORT dftu_series* dftu_series_cumsum(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_cummax(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_cummin(const dftu_series* v);
/** Running product (nulls contribute 1), same element type. */
DFTU_EXPORT dftu_series* dftu_series_cum_prod(const dftu_series* v);
/** Running count of non-null rows, as an Int64 column. */
DFTU_EXPORT dftu_series* dftu_series_cum_count(const dftu_series* v);

/** ceil/floor/trunc round floats toward +inf / -inf / zero (integers
 * unchanged); sign yields -1/0/1 in the column's type; negate is unary minus.
 * All SIMD. */
DFTU_EXPORT dftu_series* dftu_series_ceil(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_floor(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_trunc(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_sign(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_negate(const dftu_series* v);

/** First discrete difference x[i]-x[i-1] (same type) and percent change
 * (x[i]-x[i-1])/x[i-1] (Float64); row 0 is null in both. */
DFTU_EXPORT dftu_series* dftu_series_diff(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_pct_change(const dftu_series* v);

/** sqrt/exp/log over the values, as a Float64 column (sqrt is SIMD). */
DFTU_EXPORT dftu_series* dftu_series_sqrt(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_exp(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_log(const dftu_series* v);
/** log(1 + x) and exp(x) - 1 over the values, as a Float64 column that keeps
 * the input's validity: exact for a tiny x where log(1 + x) and exp(x) - 1
 * lose every digit. NULL for a nested, string or Bool column. Caller owns the
 * result. */
DFTU_EXPORT dftu_series* dftu_series_log1p(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_expm1(const dftu_series* v);

/** Elementwise math over the values: sin, cos, tan, cot (1 / tan), arcsin,
 * arccos, arctan, sinh, cosh, tanh, arcsinh, arccosh, arctanh, cbrt, log10, erf
 * and erfc. A Float32 column gives Float32, any other numeric column gives
 * Float64. Each value keeps its validity bit; a value outside the domain
 * (arcsin of 2) is NaN, as libm gives. NULL for a nested, string, Bool or
 * temporal column. Caller owns the result. */
DFTU_EXPORT dftu_series* dftu_series_sin(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_cos(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_tan(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_cot(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_arcsin(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_arccos(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_arctan(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_sinh(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_cosh(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_tanh(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_arcsinh(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_arccosh(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_arctanh(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_cbrt(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_log10(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_erf(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_erfc(const dftu_series* v);

/** Bit counts of an integer or Bool column, as a Uint32 column that keeps the
 * input's validity: the set bits, the clear bits, the run of set or clear bits
 * from the top, and the run from the bottom, within the column's own width
 * (a Bool is one bit; a signed value is its two's complement bits). NULL for
 * any other column. Caller owns the result. */
DFTU_EXPORT dftu_series* dftu_series_bitwise_count_ones(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_bitwise_count_zeros(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_bitwise_leading_ones(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_bitwise_leading_zeros(
    const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_bitwise_trailing_ones(
    const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_bitwise_trailing_zeros(
    const dftu_series* v);

/** The bitwise AND, OR and XOR of the non-null values of an integer or Bool
 * column: an I64 scalar for a signed integer or a Bool (0 or 1), a U64 scalar
 * for an unsigned one, a NULL scalar for a column with no non-null value, an
 * ERR scalar for any other column. */
DFTU_EXPORT dftu_scalar dftu_series_bitwise_and(const dftu_series* v);
DFTU_EXPORT dftu_scalar dftu_series_bitwise_or(const dftu_series* v);
DFTU_EXPORT dftu_scalar dftu_series_bitwise_xor(const dftu_series* v);

/** Predicate masks over a column, each a bit-packed Bool column that keeps
 * the input's validity (a null value has a null answer).
 * is_nan/is_finite/is_infinite test the IEEE class of a float column (Highway
 * lanes); an integer column is all-finite (never NaN/Inf), so the mask is
 * filled accordingly. NULL for a non-numeric column. */
DFTU_EXPORT dftu_series* dftu_series_is_nan(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_is_finite(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_is_infinite(const dftu_series* v);

/** The validity bitmap as a bit-packed Bool column (no nulls): null_mask[i]
 * is true where row i is null, valid_mask its complement. Any type and
 * encoding; a column with no bitmap has no nulls. */
DFTU_EXPORT dftu_series* dftu_series_null_mask(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_valid_mask(const dftu_series* v);

/** The pandas view of missing data, each a bit-packed Bool column with no
 * nulls. na_mask[i] is true where row i is null or, for a Float32 or Float64
 * column, NaN; notna_mask is its complement. On every other type they are
 * null_mask and valid_mask. The caller owns each result and frees it with
 * dftu_series_free. NULL if `v` is NULL. */
DFTU_EXPORT dftu_series* dftu_series_na_mask(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_notna_mask(const dftu_series* v);

/** is_unique[i] is true iff v's value at row i occurs exactly once;
 * is_duplicated is its complement (true for a repeated value). Both are
 * bit-packed Bool columns (hash pass; polars semantics). NULL rows form one
 * group. */
DFTU_EXPORT dftu_series* dftu_series_is_unique(const dftu_series* v);
DFTU_EXPORT dftu_series* dftu_series_is_duplicated(const dftu_series* v);
/** A Bool mask of the repeats of a value: `keep` 0 leaves the first row of each
 * value unmarked, 1 the last, any other value marks every row of a repeated
 * value (is_duplicated). A null is one value. The caller frees the result with
 * dftu_series_free. NULL if `v` is NULL or has a nested type. */
DFTU_EXPORT dftu_series* dftu_series_duplicated(const dftu_series* v,
                                                int32_t keep);

/** Whether `v` is sorted ascending (or descending when `descending` is
 * nonzero); a single scan. An empty or single-row column is sorted. */
DFTU_EXPORT int32_t dftu_series_is_sorted(const dftu_series* v,
                                          int32_t descending);

/** Drop null rows, returning a SELECTION over `v` of the non-null rows. */
DFTU_EXPORT dftu_series* dftu_series_drop_nulls(const dftu_series* v);

/** Drop the NaN rows of a Float32 or Float64 column (a null row stays); a
 * column of any other type, or with no NaN, is returned as it is (shared). The
 * caller owns the result and frees it with dftu_series_free. NULL if `v` is
 * NULL. */
DFTU_EXPORT dftu_series* dftu_series_drop_nans(const dftu_series* v);

/** Bit-packed Bool mask, true where `v`'s value is present in the `values`
 * column (a hash-set probe). NULL rows are never in the set. Numeric `v`
 * matches numeric `values`; String `v` matches String `values`. */
DFTU_EXPORT dftu_series* dftu_series_is_in(const dftu_series* v,
                                           const dftu_series* values);

/** Sort the rows of `v` ascending (or descending), as a new FLAT column:
 * take(argsort(v, descending)). */
DFTU_EXPORT dftu_series* dftu_series_sort(const dftu_series* v,
                                          int32_t descending);
/** The same with the nulls placed by `nulls_last`, as argsort_nulls. The
 * caller frees the result with dftu_series_free. NULL if `v` is NULL or has a
 * nested type. */
DFTU_EXPORT dftu_series* dftu_series_sort_nulls(const dftu_series* v,
                                                int32_t descending,
                                                int32_t nulls_last);

/** First / last `n` rows (n clamped to [0, length]) as a new FLAT column. */
DFTU_EXPORT dftu_series* dftu_series_head(const dftu_series* v, int64_t n);
DFTU_EXPORT dftu_series* dftu_series_tail(const dftu_series* v, int64_t n);

/** Rows of `v` in reverse order, as a new FLAT column. */
DFTU_EXPORT dftu_series* dftu_series_reverse(const dftu_series* v);

/** Shift rows by `n` (positive = down/lag, negative = up/lead); vacated rows
 * become null. New FLAT column. */
DFTU_EXPORT dftu_series* dftu_series_shift(const dftu_series* v, int64_t n);

/** The `k` largest / smallest rows of `v` (k clamped to [0, length]),
 * best-first: take(topk_indices(v, k, largest)). */
DFTU_EXPORT dftu_series* dftu_series_top_k(const dftu_series* v, int64_t k);
DFTU_EXPORT dftu_series* dftu_series_bottom_k(const dftu_series* v, int64_t k);

/** A DETERMINISTIC sample of `n` distinct rows (n clamped to [0, length]): the
 * rows whose mix64(index, seed) hash is smallest, in ascending row order. Not
 * RNG - reproducible for a given `seed`. */
DFTU_EXPORT dftu_series* dftu_series_sample(const dftu_series* v, int64_t n,
                                            uint64_t seed);

/** Tie-breaking method for dftu_series_rank. */
typedef int32_t dftu_rank_method;
enum {
    DFTU_RANK_AVERAGE = 0,
    DFTU_RANK_MIN = 1,
    DFTU_RANK_DENSE = 2,
    DFTU_RANK_ORDINAL = 3,
    DFTU_RANK_MAX = 4
};

/** Bit flags for dftu_series_rank's `flags`. */
typedef int32_t dftu_rank_flags;
enum {
    DFTU_RANK_FLAG_DESCENDING = 1, /**< rank from the largest value down */
    DFTU_RANK_FLAG_PCT = 2 /**< divide each rank by the non-null count */
};

/** Rank of each row (`method` is a dftu_rank_method:
 * DFTU_RANK_AVERAGE/MIN/DENSE/ORDINAL/MAX), ascending unless `flags` has
 * DFTU_RANK_FLAG_DESCENDING. With DFTU_RANK_FLAG_PCT each rank is divided by
 * the count of non-null values, or by the number of distinct values for
 * DFTU_RANK_DENSE as pandas does (a percentile rank in (0, 1]). Returns a
 * Float64 column; a null value has a null rank. The old `descending` argument
 * is the DFTU_RANK_FLAG_DESCENDING bit, so a caller passing 0 or 1 is
 * unchanged. */
DFTU_EXPORT dftu_series* dftu_series_rank(const dftu_series* v,
                                          dftu_rank_method method,
                                          int32_t flags);

/** Reduction for dftu_series_rolling. */
typedef int32_t dftu_rolling_op;
enum {
    DFTU_ROLLING_SUM = 0,
    DFTU_ROLLING_MEAN = 1,
    DFTU_ROLLING_MIN = 2,
    DFTU_ROLLING_MAX = 3
};

/** A string predicate against one literal pattern (dftu_expr_str_pred). */
typedef int32_t dftu_str_pred_op;
enum {
    DFTU_STR_PRED_CONTAINS = 0,
    DFTU_STR_PRED_STARTS_WITH = 1,
    DFTU_STR_PRED_ENDS_WITH = 2,
    DFTU_STR_PRED_LIKE = 3,
    DFTU_STR_PRED_MATCHES = 4,
    DFTU_STR_PRED_SEARCH = 5
};

/** A String -> String map with no argument (dftu_expr_str_map). */
typedef int32_t dftu_str_map_op;
enum {
    DFTU_STR_MAP_LOWER = 0,
    DFTU_STR_MAP_UPPER = 1,
    DFTU_STR_MAP_STRIP = 2,
    DFTU_STR_MAP_LSTRIP = 3,
    DFTU_STR_MAP_RSTRIP = 4,
    DFTU_STR_MAP_CAPITALIZE = 5,
    DFTU_STR_MAP_TITLE = 6,
    DFTU_STR_MAP_SWAPCASE = 7
};

/** A string method with text, integer or second-column operands, evaluated
 * by the Series function the eager Series.str method of the same name calls
 * (dftu_expr_str_fn). 0-8 are the dftu_series_str_is classes in order.
 * NORMALIZE takes the form in `i0`, WRAP the width in `i0` and the flag bits
 * in `i1`. TRANSLATE takes its table in the two texts: `text` holds the keys,
 * one codepoint each, and `text2` holds one entry per key, a codepoint L and
 * then the value's codepoints (L is 1 + their count, or 0 to delete the key).
 */
typedef int32_t dftu_str_fn;
enum {
    DFTU_STR_FN_ISALNUM = 0,
    DFTU_STR_FN_ISALPHA = 1,
    DFTU_STR_FN_ISDIGIT = 2,
    DFTU_STR_FN_ISDECIMAL = 3,
    DFTU_STR_FN_ISNUMERIC = 4,
    DFTU_STR_FN_ISSPACE = 5,
    DFTU_STR_FN_ISLOWER = 6,
    DFTU_STR_FN_ISUPPER = 7,
    DFTU_STR_FN_ISTITLE = 8,
    DFTU_STR_FN_PAD_START = 9,
    DFTU_STR_FN_PAD_END = 10,
    DFTU_STR_FN_CENTER = 11,
    DFTU_STR_FN_ZFILL = 12,
    DFTU_STR_FN_REMOVE_PREFIX = 13,
    DFTU_STR_FN_REMOVE_SUFFIX = 14,
    DFTU_STR_FN_REPEAT = 15,
    DFTU_STR_FN_SLICE_REPLACE = 16,
    DFTU_STR_FN_SPLIT = 17,
    DFTU_STR_FN_PARTITION = 18,
    DFTU_STR_FN_RPARTITION = 19,
    DFTU_STR_FN_FINDALL = 20,
    DFTU_STR_FN_EXTRACT = 21,
    DFTU_STR_FN_RFIND = 22,
    DFTU_STR_FN_INDEX = 23,
    DFTU_STR_FN_RINDEX = 24,
    DFTU_STR_FN_JOIN = 25,
    DFTU_STR_FN_GET = 26,
    DFTU_STR_FN_CAT = 27,
    DFTU_STR_FN_REGEX_REPLACE = 28,
    DFTU_STR_FN_NORMALIZE = 29,
    DFTU_STR_FN_TRANSLATE = 30,
    DFTU_STR_FN_WRAP = 31
};

/** Rolling-window reduction (`op` is a dftu_rolling_op:
 * DFTU_ROLLING_SUM/MEAN/MIN/MAX) over `window` trailing rows; the first
 * window-1 rows and every window holding a null are null (pandas
 * `min_periods = window`). Returns Float64. */
DFTU_EXPORT dftu_series* dftu_series_rolling(const dftu_series* v,
                                             int64_t window,
                                             dftu_rolling_op op);

/** Rolling sample variance / standard deviation over `window` trailing rows;
 * the first window-1 rows and every window holding a null are null, a window
 * of fewer than 2 values yields 0. Returns Float64. */
DFTU_EXPORT dftu_series* dftu_series_rolling_var(const dftu_series* v,
                                                 int64_t window);
DFTU_EXPORT dftu_series* dftu_series_rolling_std(const dftu_series* v,
                                                 int64_t window);
/** Rolling median / quantile (`q` in [0, 1], linear interpolation) over
 * `window` trailing rows; the first window-1 rows and every window holding a
 * null are null. Returns Float64. */
DFTU_EXPORT dftu_series* dftu_series_rolling_median(const dftu_series* v,
                                                    int64_t window);
DFTU_EXPORT dftu_series* dftu_series_rolling_quantile(const dftu_series* v,
                                                      int64_t window, double q);

/** Exponentially weighted mean / sample std with smoothing `alpha` in (0, 1]
 * (pandas `ewm(alpha, adjust=True, ignore_na=False)`: observation j carries
 * weight (1 - alpha)^(i - j) at row i, a null row adds no observation and
 * repeats the previous value), returning Float64. ewm_std is null until a
 * second observation (variance undefined). */
DFTU_EXPORT dftu_series* dftu_series_ewm_mean(const dftu_series* v,
                                              double alpha);
DFTU_EXPORT dftu_series* dftu_series_ewm_std(const dftu_series* v,
                                             double alpha);

/** Flags of dftu_series_cut. */
typedef int32_t dftu_cut_flag;
enum {
    DFTU_CUT_RIGHT = 1, /**< close each interval on the right: (a, b] */
    DFTU_CUT_INNER = 2  /**< null outside the interior intervals, bins from 0 */
};

/** Bin each value into the intervals defined by the ascending numeric `breaks`
 * column. With `flags` 0 the intervals are [a, b) and the bin index is the
 * count of breaks <= x, in 0..breaks length. DFTU_CUT_RIGHT counts the breaks
 * < x instead, so each interval is closed on the right. DFTU_CUT_INNER gives a
 * null for a value outside the interior intervals and numbers the interior
 * intervals from 0 (with both flags this is pandas cut(labels=False)). A bit
 * outside the two flags returns NULL. Returns Int32; a null input row yields a
 * null bin. */
DFTU_EXPORT dftu_series* dftu_series_cut(const dftu_series* v,
                                         const dftu_series* breaks,
                                         int32_t flags);
/** Like dftu_series_cut, but the edges are the `q`-quantiles of the column (q
 * buckets, bins 0..q-1). Returns Int32; a null input row yields a null bin. */
DFTU_EXPORT dftu_series* dftu_series_qcut(const dftu_series* v, int32_t q);

/** For each element of `values`, the lower-bound insertion index into `v`
 * (assumed sorted ascending) that keeps it sorted. Returns an Int64 column of
 * length values length. */
DFTU_EXPORT dftu_series* dftu_series_search_sorted(const dftu_series* v,
                                                   const dftu_series* values);
/** The same, inserting after the equal values when `right` is nonzero. The
 * caller frees the Int64 result with dftu_series_free. NULL if `v` or `values`
 * is NULL or either is not numeric. */
DFTU_EXPORT dftu_series* dftu_series_search_sorted_side(
    const dftu_series* v, const dftu_series* values, int32_t right);

/** Linearly interpolate null interior values between their nearest non-null
 * neighbors (by row index); leading/trailing nulls stay null. Returns
 * Float64. */
DFTU_EXPORT dftu_series* dftu_series_interpolate(const dftu_series* v);

/** Bit-packed Bool mask, true where lo <= x <= hi (inclusive); carries v's
 * validity. The scalars convert to v's element type (exact for i64). SIMD. */
DFTU_EXPORT dftu_series* dftu_series_is_between(const dftu_series* v,
                                                dftu_scalar lo, dftu_scalar hi);

/** Sum of x[i]*y[i] over the non-null pairs of `v` and `other` (equal length),
 * as an F64 scalar. SIMD for a null-free Float64 pair. Read with
 * scalar_value<double>(). */
DFTU_EXPORT dftu_scalar dftu_series_dot(const dftu_series* v,
                                        const dftu_series* other);

#ifndef __cplusplus
/** C convenience: build a tagged scalar from a value, e.g.
 * dftu_series_compare(col, DFTU_CMP_GT, DFTU_SCALAR_I64(100)). (C++ callers
 * pass a plain value to the typed templates; these are for C.) */
/* Designated, not positional: `len` sits between `kind` and `value`, so a
 * positional form would bind the second initializer to `len`. */
#define DFTU_SCALAR_I64(v) \
    ((dftu_scalar){        \
        .kind = DFTU_SCALAR_TAG_I64, .len = 0, .value = {.i = (int64_t)(v)}})
#define DFTU_SCALAR_U64(v) \
    ((dftu_scalar){        \
        .kind = DFTU_SCALAR_TAG_U64, .len = 0, .value = {.u = (uint64_t)(v)}})
#define DFTU_SCALAR_F64(v) \
    ((dftu_scalar){        \
        .kind = DFTU_SCALAR_TAG_F64, .len = 0, .value = {.d = (double)(v)}})
/** `s` is BORROWED for the call; it must outlive the call it is passed to. */
#define DFTU_SCALAR_STR(p, n)                   \
    ((dftu_scalar){.kind = DFTU_SCALAR_TAG_STR, \
                   .len = (uint32_t)(n),        \
                   .value = {.s = (p)}})
#endif

/** Evaluate `q` as a bit-packed Bool mask over a materialized batch of `n`
 * columns named by `names`. Returns an owned column (length == the columns'
 * length), or NULL if the predicate has no columnar lowering (pattern match,
 * a referenced field absent from the batch) - the
 * caller should fall back to the scan-time evaluator - or on error. */
DFTU_EXPORT dftu_series* dftu_dataframe_mask(const dftu_duql* q,
                                             const dftu_series* const* columns,
                                             const char* const* names,
                                             int32_t n);

/*
 * dftu_dataframe: an opaque handle to a named set of columns (the frame-level
 * counterpart to dftu_series). Frame ops take and return dftu_dataframe*; each
 * returned handle is owned by the caller and freed with dftu_dataframe_free.
 */
#ifndef DFTU_TYPEDEF_DFTU_DATAFRAME
#define DFTU_TYPEDEF_DFTU_DATAFRAME
typedef struct dftu_dataframe dftu_dataframe;
#endif

/** Build a frame from `n` columns named by `names`. Takes ownership of the
 * `columns` handles (they are moved in; do not free them after). NULL on a
 * length/row-count mismatch. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_new(const char* const* names,
                                               dftu_series* const* columns,
                                               int32_t n);
DFTU_EXPORT void dftu_dataframe_free(dftu_dataframe* df);

DFTU_EXPORT int64_t dftu_dataframe_num_rows(const dftu_dataframe* df);
DFTU_EXPORT int32_t dftu_dataframe_num_columns(const dftu_dataframe* df);
/** Name of column `i` (valid for the lifetime of `df`), or NULL if out of
 * range. */
DFTU_EXPORT const char* dftu_dataframe_column_name(const dftu_dataframe* df,
                                                   int32_t i);
/** A column by name, sharing its buffers zero-copy (owned; free with
 * dftu_series_free), or NULL if absent. */
DFTU_EXPORT dftu_series* dftu_dataframe_column(const dftu_dataframe* df,
                                               const char* name);
/** Column `i` of `df`, borrowed: the pointer indexes the frame's own column, so
 * it allocates nothing, and it stays valid only while `df` lives and is not
 * modified. NULL for an index out of range. */
DFTU_EXPORT const dftu_series* dftu_dataframe_column_at(
    const dftu_dataframe* df, int32_t i);

/* The row index: the ordered columns that label the rows. The columns are
 * ordinary columns of the frame; an empty index is the positional RangeIndex.
 * A frame carries its index through the frame ops by the rules in
 * frame_op_index_rules.def, and through Arrow as schema metadata. */

/** Status codes of dftu_frame_with_index. */
enum {
    DFTU_FRAME_INDEX_OK = 0,
    /** `frame`, `out`, or `names` or `lens` with `n` > 0 is NULL, or `n` is
     * negative. */
    DFTU_FRAME_INDEX_BAD_ARGUMENT = 1,
    /** A name is no column of the frame. */
    DFTU_FRAME_INDEX_NOT_FOUND = 2,
    /** A name repeats, or names a column whose type has no hash and equality
     * (list, struct). */
    DFTU_FRAME_INDEX_INVALID = 3
};

/** The number of index columns of `frame`: 0 when it has no index or `frame`
 * is NULL. */
DFTU_EXPORT int32_t dftu_frame_index_count(const dftu_dataframe* frame);
/** Index column `i` of `frame`: stores its name in `*ptr` (NUL-terminated, not
 * owned) and its byte length in `*len` (`len` may be NULL). The text stays
 * valid for the lifetime of `frame`, which is never modified. Returns 0, or -1
 * and leaves `*ptr` and `*len` untouched when `frame` or `ptr` is NULL or `i`
 * is out of range. */
DFTU_EXPORT int32_t dftu_frame_index_name(const dftu_dataframe* frame,
                                          int32_t i, const char** ptr,
                                          size_t* len);
/** A zero-copy copy of `frame` whose index is the `n` columns named by `names`
 * (each `lens[i]` bytes, not necessarily NUL-terminated); `n` == 0 drops the
 * index. On DFTU_FRAME_INDEX_OK stores an owned frame in `*out`, which the
 * caller frees with dftu_dataframe_free; `frame` is not consumed and may be
 * freed independently. On any other status `*out` is set to NULL (when `out` is
 * not NULL) and nothing is allocated. */
DFTU_EXPORT int32_t dftu_frame_with_index(const dftu_dataframe* frame,
                                          const char* const* names,
                                          const size_t* lens, int32_t n,
                                          dftu_dataframe** out);

/* Row ops (return a new frame). */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_take(const dftu_dataframe* df,
                                                const int64_t* idx, int64_t n);
/** dftu_dataframe_take with an int32 count (the I64LIST operand form). */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_take_i32(const dftu_dataframe* df,
                                                    const int64_t* idx,
                                                    int32_t n);
DFTU_EXPORT dftu_dataframe* dftu_dataframe_filter(const dftu_dataframe* df,
                                                  const dftu_series* mask);
DFTU_EXPORT dftu_dataframe* dftu_dataframe_slice(const dftu_dataframe* df,
                                                 int64_t offset, int64_t len);
DFTU_EXPORT dftu_dataframe* dftu_dataframe_head(const dftu_dataframe* df,
                                                int64_t n);
DFTU_EXPORT dftu_dataframe* dftu_dataframe_sort_by(const dftu_dataframe* df,
                                                   const char* name,
                                                   int32_t descending);
/** Order every row by the column `name`, ascending unless `descending`; null
 * keys go last when `nulls_last` is nonzero, first otherwise. Stable: rows
 * that tie, nulls included, keep their input order. Returns a new frame that
 * the caller frees with dftu_dataframe_free, or NULL if `df` or `name` is NULL
 * or `name` is absent or has no order (a nested type). */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_sort_by_nulls(
    const dftu_dataframe* df, const char* name, int32_t descending,
    int32_t nulls_last);
DFTU_EXPORT dftu_dataframe* dftu_dataframe_topk(const dftu_dataframe* df,
                                                const char* name, int64_t k,
                                                int32_t largest);

/* Projection ops (share buffers zero-copy). */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_select(const dftu_dataframe* df,
                                                  const char* const* names,
                                                  int32_t n);
DFTU_EXPORT dftu_dataframe* dftu_dataframe_rename(const dftu_dataframe* df,
                                                  const char* const* new_names,
                                                  int32_t n);
/** Add or replace column `name`. Returns NULL when `col` has a different
    length than the frame's other columns, or on a null argument. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_with_column(const dftu_dataframe* df,
                                                       const char* name,
                                                       const dftu_series* col);

/** Drop every row that is null in ANY column. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_drop_nulls(const dftu_dataframe* df);
/** Fill nulls in every column with `value`; columns whose type rejects the
 * scalar (string/bool) are carried over unchanged. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_fill_null(const dftu_dataframe* df,
                                                     dftu_scalar value);
/** Drop every row that holds a NaN in any Float32 or Float64 column (a null is
 * not a NaN). The caller owns the result and frees it with
 * dftu_dataframe_free. NULL if `df` is NULL or a float column cannot be read.
 */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_drop_nans(const dftu_dataframe* df);
/** Replace the NaN of every Float32 or Float64 column with `value` (a NULL
 * scalar makes it null, as dftu_series_fill_nan); other columns are carried
 * over unchanged. The caller owns the result and frees it with
 * dftu_dataframe_free. NULL if `df` is NULL or `value` is a string or error
 * scalar. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_fill_nan(const dftu_dataframe* df,
                                                    dftu_scalar value);
/** Distinct ROWS (keep first), hashing all columns as the composite key.
 * dftu_dataframe_drop_duplicates is an alias. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_unique(const dftu_dataframe* df);
DFTU_EXPORT dftu_dataframe* dftu_dataframe_drop_duplicates(
    const dftu_dataframe* df);
/** Distinct ROWS (keep first) keyed on the `n` `subset` columns only. NULL if
 * `df` is NULL, `n` is 0 or a name is absent. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_unique_by(const dftu_dataframe* df,
                                                     const char* const* subset,
                                                     int32_t n);
/** Stable lexicographic sort by `n` key columns named by `names`, ascending
 * unless `descending` (nulls last in both directions). */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_sort_by_multi(
    const dftu_dataframe* df, const char* const* names, int32_t n,
    int32_t descending);
/** The same with the nulls placed by `nulls_last` (last when nonzero). The
 * caller frees the new frame with dftu_dataframe_free. NULL if `df` is NULL,
 * `n` is negative, `names` is NULL while `n` is above 0, or a name is absent
 * or has no order. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_sort_by_multi_nulls(
    const dftu_dataframe* df, const char* const* names, int32_t n,
    int32_t descending, int32_t nulls_last);
/** Per-column direction form: `descending[i]` (0/1) applies to `names[i]`.
 * `descending_n` must be 1 (broadcast to every key) or equal `n`. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_sort_by_multi_per_col(
    const dftu_dataframe* df, const char* const* names, int32_t n,
    const int32_t* descending, int32_t descending_n);
/** Per-column form: `descending[i]` and `nulls_last[i]` (0/1) apply to
 * `names[i]`. `descending_n` and `nulls_last_n` must each be 1 (broadcast to
 * every key) or equal `n`. Returns a new frame that the caller frees with
 * dftu_dataframe_free. NULL if `df` is NULL, a count is negative, `names`,
 * `descending` or `nulls_last` is NULL while its count is above 0, a size is
 * neither 1 nor `n`, or a name is absent or has no order. The arrays are only
 * read during the call. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_sort_by_multi_per_col_nulls(
    const dftu_dataframe* df, const char* const* names, int32_t n,
    const int32_t* descending, int32_t descending_n, const int32_t* nulls_last,
    int32_t nulls_last_n);
/** The last `n` rows (clamped). */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_tail(const dftu_dataframe* df,
                                                int64_t n);
/** The rows in reverse order. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_reverse(const dftu_dataframe* df);
/** A DETERMINISTIC sample of `n` rows (mix64(index + seed) bottom-n, ascending
 * row order). */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_sample(const dftu_dataframe* df,
                                                  int64_t n, uint64_t seed);
/** Prepend an Int64 column [0, num_rows) named `name`. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_with_row_index(
    const dftu_dataframe* df, const char* name);
/** Per-column summary statistics: one row per statistic (count, null_count,
 * mean, std, min, max), a leading "statistic" String column, and one Float64
 * column per numeric input column. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_describe(const dftu_dataframe* df);
/** A one-row frame with one Int64 column per input column giving its null
 * count. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_null_count(const dftu_dataframe* df);
/** One row reducing each eligible column with `agg` (a one-column,
 * parameter-free dftu_agg_op code: SUM, MIN, MAX, COUNT_VALID, MEAN, VAR,
 * STD, SKEW, KURT, SUMSQ, FIRST, LAST, BIT_OR or PROD) under its own name: the
 * pandas `df.sum()` family. COUNT_VALID / FIRST / LAST take every column, the
 * rest the numeric ones. The grouped form and any two-column or parameterized
 * aggregate go through dftu_dataframe_group_by with their specs. NULL if `df`
 * is NULL or `agg` is any other code. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_reduce(const dftu_dataframe* df,
                                                  int32_t agg);
/** The group-wise transforms of a group-by (pandas `groupby(k).cumsum()` and
 * kin): one value per input row, rows in input order. */
typedef int32_t dftu_groupwise_op;
enum {
    DFTU_GROUPWISE_CUMSUM = 0,
    DFTU_GROUPWISE_CUMMAX = 1,
    DFTU_GROUPWISE_CUMMIN = 2,
    DFTU_GROUPWISE_CUMCOUNT = 3,
    DFTU_GROUPWISE_SHIFT = 4,
    DFTU_GROUPWISE_DIFF = 5,
    DFTU_GROUPWISE_PCT_CHANGE = 6,
    DFTU_GROUPWISE_RANK = 7,
    DFTU_GROUPWISE_NGROUP = 8,
    DFTU_GROUPWISE_HEAD = 9,
    DFTU_GROUPWISE_TAIL = 10,
    DFTU_GROUPWISE_NTH = 11,
    DFTU_GROUPWISE_FFILL = 12,
    DFTU_GROUPWISE_BFILL = 13,
    DFTU_GROUPWISE_ROLLING_SUM = 14,
    DFTU_GROUPWISE_ROLLING_MEAN = 15,
    DFTU_GROUPWISE_ROLLING_MIN = 16,
    DFTU_GROUPWISE_ROLLING_MAX = 17,
    DFTU_GROUPWISE_CUMPROD = 18
};
/** `kind` (a dftu_groupwise_op) over the groups of the `n_keys` `keys`
 * columns. CUMSUM / CUMPROD (Int64 or UInt64 over an integer column, wrapping)
 * / CUMMAX / CUMMIN / DIFF / PCT_CHANGE / RANK take every numeric non-key
 * column, SHIFT every non-key column, each output under the column's own name;
 * CUMCOUNT / NGROUP are one Int64 column of that name; HEAD / TAIL / NTH keep
 * the input columns of the selected rows; FFILL / BFILL fill each non-key
 * column's nulls within the group from the nearest present value before /
 * after; ROLLING_SUM / MEAN / MIN / MAX reduce the trailing `n` rows of each
 * numeric non-key column within the group, null until the window holds `n`
 * present values. `n` is the SHIFT periods (negative looks ahead), the HEAD /
 * TAIL count, the NTH position (0-based, negative from the end) or the rolling
 * window; `rank_method` (a dftu_rank_method) and `ascending` apply to RANK. A
 * null value stays null. Runs the window kernel the utilities library registers
 * as `dftu.frame.window`. NULL if `df` is NULL, `kind` is not a transform, a
 * key is absent, NGROUP has no keys or no column is eligible. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_group_transform(
    const dftu_dataframe* df, const char* const* keys, int32_t n_keys,
    int32_t kind, int64_t n, int32_t rank_method, int32_t ascending);
/** `column` replaced by the registered column op `op` (a `dftu.series.*`
 * name whose first operand is a column) run over it: the op's remaining
 * operands are filled in signature order from `column2` (a second column
 * operand, the op's `in[1]` or a `series` slot), the scalars `a` then `b`
 * (an int / enum / index token reads the scalar as an integer, a double
 * token as a double, a `scalar` token as it is) and `text` (a `str` or
 * `char` token; NULL when the op takes none). Every other column passes
 * through unchanged. The whole-column ops (cumulative, rolling, sort,
 * rank, the fills) are meaningful only over the entire table, which is why
 * the lazy plan runs this as a breaker (`dftu.frame.column_op`). NULL if a
 * name is absent, the op is not a column op, an operand token has no
 * source, or the op refuses its input. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_column_op(
    const dftu_dataframe* df, const char* column, const char* op,
    const char* column2, dftu_scalar a, dftu_scalar b, const char* text);
/** Bit-packed Bool column (length num_rows): true where the whole ROW is
 * duplicated / unique, hashing all columns. Caller owns it (dftu_series_free).
 */
DFTU_EXPORT dftu_series* dftu_dataframe_is_duplicated(const dftu_dataframe* df);
DFTU_EXPORT dftu_series* dftu_dataframe_is_unique(const dftu_dataframe* df);
/** Int32 column (length num_rows): the part in [0, n_parts) each row lands in
 * under a stable hash of the `keys` columns (equal keys share a part); the
 * shuffle primitive for a distributed group_by/join. NULL if a key is absent,
 * `n` is 0 or `n_parts < 1`. Caller owns it (dftu_series_free). */
DFTU_EXPORT dftu_series* dftu_dataframe_partition_id(const dftu_dataframe* df,
                                                     const char* const* keys,
                                                     int32_t n,
                                                     int64_t n_parts);

/** Evaluate `q` as a bit-packed Bool mask over `df`'s columns (the frame-shaped
 * counterpart to dftu_dataframe_mask). Returns an owned column, or NULL if the
 * predicate has no columnar lowering or on error. */
DFTU_EXPORT dftu_series* dftu_dataframe_mask_frame(const dftu_dataframe* df,
                                                   const dftu_duql* q);

/** The distinct values of `v` and their counts, as a frame with columns `value`
 * (v's type) and `count` (Int64), most-frequent first. Caller owns the frame
 * (dftu_dataframe_free). */
DFTU_EXPORT dftu_dataframe* dftu_series_value_counts(const dftu_series* v);

/** Reshape wide -> long: keep the `n_id` `id_vars` columns and stack the
 * `n_val` `value_vars` columns into two new columns `variable` (String) and
 * `value`. The result has num_rows * n_val rows; value columns that differ in
 * type must all be numeric and are cast to a common type (Float64 if any is
 * float, else Int64). NULL on an unknown column or an unmixable value set. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_unpivot(
    const dftu_dataframe* df, const char* const* id_vars, int32_t n_id,
    const char* const* value_vars, int32_t n_val);

/** Expand a List `column`: each element becomes its own row (other columns
 * repeated); an empty/null list yields one null row. NULL if `column` is absent
 * or not a List column. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_explode(const dftu_dataframe* df,
                                                   const char* column);

/** UNNEST a List `column`: as dftu_dataframe_explode, but an empty/null list
 * drops the row unless `keep_empty` is nonzero, and a List<Struct> column is
 * replaced by one column per struct field. NULL if `column` is absent or not
 * a List column. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_unnest(const dftu_dataframe* df,
                                                  const char* column,
                                                  int32_t keep_empty);

/** One-hot encode `column`: replace it with one Int8 column per distinct value
 * (sorted ascending), named `<column>_<value>` and 1 where the row equals that
 * value. Other columns pass through. NULL if `column` is absent. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_to_dummies(const dftu_dataframe* df,
                                                      const char* column);

/** Reshape long -> wide: rows are the distinct `index` values (sorted), one
 * value column per distinct `columns` value (sorted, named by its value), each
 * cell `values` aggregated over the matching rows. `agg` is the collision
 * reducer: first|last|sum|min|max|mean (NULL defaults to "first"; mean yields
 * Float64). NULL on an unknown column or agg. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_pivot(const dftu_dataframe* df,
                                                 const char* index,
                                                 const char* columns,
                                                 const char* values,
                                                 const char* agg);

/** One aggregate for the group_by and group_by_dynamic calls: `op` is any
 * aggregate name in agg_ops.def, or else the name of a user-defined aggregate
 * a plugin registered (dftu_svc_ops::register_udaf), which gets `column` as its
 * one argument, or none when `column` is NULL or empty, and gives a column of
 * the result type its bind chose. A name that is neither fails the call, and so
 * does the name "udaf" itself. `column` is the value column (ignored / may be
 * NULL for count), `out` the result column name. */
typedef struct dftu_group_agg {
    const char* op;
    const char* column;
    const char* out;
} dftu_group_agg;

/** Tumbling/sliding time-window aggregation over an ascending Int64 `time_col`:
 * windows start at the first time floored to a multiple of `every`, stride by
 * `every`, and each covers `[start, start + period)` (`period <= 0` means "=
 * every"; `period > every` overlaps). Emits one row per non-empty window: a
 * leading Int64 `time_col` window-start column, then one column per `aggs`
 * entry, named by its `out`. NULL if `every <= 0`, `time_col` is absent or not
 * Int64, or an agg names an unknown column/op. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_group_by_dynamic(
    const dftu_dataframe* df, const char* time_col, int64_t every,
    int64_t period, const dftu_group_agg* aggs, int32_t n_aggs);

/** Group by the `n_keys` key columns named by `keys` (a composite key) and
 * compute each aggregate in `aggs` (op|column|out, as
 * dftu_dataframe_group_by_dynamic). NULL on an unknown column/op. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_group_by(const dftu_dataframe* df,
                                                    const char* const* keys,
                                                    int32_t n_keys,
                                                    const dftu_group_agg* aggs,
                                                    int32_t n_aggs);

/** How dftu_dataframe_concat aligns its parts' columns. VERTICAL needs one
 * shared schema (same names, types and order); DIAGONAL unions the columns,
 * null-filling a column a part lacks and promoting a mixed-numeric column. */
typedef int32_t dftu_concat_how;
enum { DFTU_CONCAT_VERTICAL = 0, DFTU_CONCAT_DIAGONAL = 1 };

/** Vertically concatenate `n` frames (UNION ALL). NULL if `n` is 0, a part
 * is NULL, `how` is not a dftu_concat_how, or the schemas cannot align. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_concat(
    const dftu_dataframe* const* parts, int32_t n, dftu_concat_how how);
/** dftu_dataframe_concat of two frames, the registry form (dftu.frame.concat).
 */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_concat2(const dftu_dataframe* a,
                                                   const dftu_dataframe* b,
                                                   dftu_concat_how how);
/** SQL UNION: the distinct rows of `a` and `b` concatenated vertically. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_union(const dftu_dataframe* a,
                                                 const dftu_dataframe* b);

/** Which rows a join keeps. INNER keeps matched pairs; LEFT / RIGHT also keep
 * that side's unmatched rows with the other side null-filled; OUTER keeps
 * both; SEMI / ANTI keep the left rows with / without a match (left columns
 * only); CROSS pairs every left row with every right row (no keys).
 *
 * LOOKUP keeps every left row once, in order, and adds each right column but
 * the keys, from the matching right row, null when none. Right rows that
 * share a key must hold equal values, else the join fails naming the key and
 * the column. A right column named like a left column fills it: a left row
 * that holds a value there fails the join. NEST keeps every left row once and
 * adds one list column, named by `suffix`, of every matching right row as a
 * struct of all its columns, empty when none. Both compare keys by value:
 * integers equal floats of the same value, strings by their bytes, other
 * types never across; key pairs need not share a type. */
typedef int32_t dftu_join_how;
enum {
    DFTU_JOIN_INNER = 0,
    DFTU_JOIN_LEFT = 1,
    DFTU_JOIN_RIGHT = 2,
    DFTU_JOIN_OUTER = 3,
    DFTU_JOIN_SEMI = 4,
    DFTU_JOIN_ANTI = 5,
    DFTU_JOIN_CROSS = 6,
    DFTU_JOIN_LOOKUP = 7,
    DFTU_JOIN_NEST = 8
};

/** Hash join `df` (left) with `other` (right) on the `n` key pairs
 * `left_on[i]` = `right_on[i]`, compared exactly (a null key never matches
 * unless `nulls_equal` is nonzero, which makes a null key cell match another
 * null key cell and is refused, as NULL, for CROSS, LOOKUP and NEST; and each
 * pair must share a type, except for LOOKUP and NEST). Output columns:
 * every left column, then every right column except a key whose name equals its
 * left key (that column is emitted once, coalesced for OUTER); another right
 * column whose name collides with a left column gets `suffix` appended (NULL =
 * "_right"). Matched rows keep left order, then a right-preserving join appends
 * the unmatched right rows. CROSS ignores the keys. NULL if a key column is
 * absent, a key pair's types differ, `n` is 0 for a keyed join, or `how` is
 * not a dftu_join_how. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_join(
    const dftu_dataframe* df, const dftu_dataframe* other,
    const char* const* left_on, const char* const* right_on, int32_t n,
    dftu_join_how how, const char* suffix, int32_t nulls_equal);

/*
 * dftu_lazyframe: an opaque handle to a deferred query over a dftu_dataframe
 * (the lazy counterpart to the eager frame ops above). Builder ops record a
 * step and return a NEW dftu_lazyframe sharing the source frame's buffers (no
 * data copy); nothing runs until dftu_lazyframe_collect. Every returned handle
 * is owned by the caller and freed with dftu_lazyframe_free. Ops reuse the
 * existing dftu_expr / dftu_group_agg types.
 */
#ifndef DFTU_TYPEDEF_DFTU_LAZYFRAME
#define DFTU_TYPEDEF_DFTU_LAZYFRAME
typedef struct dftu_lazyframe dftu_lazyframe;
#endif

/* The expression handle (the C++ Expr behind it is in dataframe/expr.h). */
typedef struct dftu_expr dftu_expr;

/* Build and evaluate an expression from any C/C++ consumer (the compiler -
 * type inference + CSE + lowering - runs in dftu_expr_eval, so every consumer
 * gets the same fusion). Builders return an owned handle; children are shared,
 * so reusing a handle de-duplicates naturally. */
DFTU_EXPORT dftu_expr* dftu_expr_col(int32_t index);
DFTU_EXPORT dftu_expr* dftu_expr_lit_i64(int64_t value);
DFTU_EXPORT dftu_expr* dftu_expr_lit_f64(double value);
DFTU_EXPORT dftu_expr* dftu_expr_binary(int32_t op, const dftu_expr* a,
                                        const dftu_expr* b);
DFTU_EXPORT dftu_expr* dftu_expr_prim(int32_t prim, const dftu_expr* a);
DFTU_EXPORT dftu_expr* dftu_expr_unary(int32_t op, const dftu_expr* a);
DFTU_EXPORT dftu_expr* dftu_expr_clip(const dftu_expr* a, dftu_scalar lo,
                                      dftu_scalar hi);
DFTU_EXPORT dftu_expr* dftu_expr_cmp(int32_t cmp, const dftu_expr* a,
                                     dftu_scalar rhs);
DFTU_EXPORT dftu_expr* dftu_expr_logical(int32_t op, const dftu_expr* a,
                                         const dftu_expr* b);
DFTU_EXPORT dftu_expr* dftu_expr_not(const dftu_expr* a);
DFTU_EXPORT dftu_expr* dftu_expr_cast(int32_t type, const dftu_expr* a);
DFTU_EXPORT dftu_expr* dftu_expr_lower(const dftu_expr* a);
/** `a <op> pattern` (op is a dftu_str_pred_op); `pattern` is copied. */
DFTU_EXPORT dftu_expr* dftu_expr_str_pred(int32_t op, const dftu_expr* a,
                                          const char* pattern,
                                          int32_t pattern_len);
/** `a <op> needle` per row with `needle` an expression (op is
 * DFTU_STR_PRED_CONTAINS, STARTS_WITH or ENDS_WITH). Null where either operand
 * is null; true for an empty needle. Returns a new handle the caller frees
 * with dftu_expr_free, or NULL when `a` or `needle` is NULL or `op` is another
 * predicate. Both operands are shared by the new node, not consumed. Never
 * throws. */
DFTU_EXPORT dftu_expr* dftu_expr_str_pred_col(int32_t op, const dftu_expr* a,
                                              const dftu_expr* needle);
/** Replace every `from` with `to` per row, all three String expressions; null
 * where any is null, the row unchanged for an empty `from`. Ownership and
 * NULL as dftu_expr_str_pred_col. */
DFTU_EXPORT dftu_expr* dftu_expr_str_replace_col(const dftu_expr* a,
                                                 const dftu_expr* from,
                                                 const dftu_expr* to);
/** `len` UTF-8 characters from character `start` per row, both integer
 * expressions; `len` NULL means to the end. Null where an operand is null,
 * negative or not an integer. Ownership as dftu_expr_str_pred_col; returns
 * NULL when `a` or `start` is NULL. */
DFTU_EXPORT dftu_expr* dftu_expr_str_substr_col(const dftu_expr* a,
                                                const dftu_expr* start,
                                                const dftu_expr* len);
/** A String -> String map (op is a dftu_str_map_op). */
DFTU_EXPORT dftu_expr* dftu_expr_str_map(int32_t op, const dftu_expr* a);
/** A string method (`fn` a dftu_str_fn) over `a`. `text` and `text2` are
 * copied (a separator, fill byte, affix, pattern or replacement; NULL with
 * length 0 for none), `i0` and `i1` are the integer operands (width, repeat
 * count, group, index, start/stop), and `b` is the second column for
 * DFTU_STR_FN_CAT (NULL otherwise). Returns NULL for an unknown `fn`. */
DFTU_EXPORT dftu_expr* dftu_expr_str_fn(int32_t fn, const dftu_expr* a,
                                        const dftu_expr* b, const char* text,
                                        int32_t text_len, const char* text2,
                                        int32_t text2_len, int64_t i0,
                                        int64_t i1);
/** The UTC calendar field `part` (a dftu_dt_part code: 0-11, 14 or 15) of the
 * time `a`, Int64, where `a` counts units of `ns_per_unit` nanoseconds since
 * the Unix epoch. A null cell, or a Float64 that is NaN or infinite, is null.
 * Returns a new handle the caller frees, or NULL for a NULL `a`, an unknown
 * `part` or `ns_per_unit` <= 0. */
DFTU_EXPORT dftu_expr* dftu_expr_date_part(const dftu_expr* a, int32_t part,
                                           int64_t ns_per_unit);
/** The time `a` as UTC text by the strftime-style `fmt` (copied; see
 * dftu_series_dt_format for the directives), String. Returns a new handle the
 * caller frees, or NULL for a NULL `a`, an invalid `fmt` (unknown directive or
 * trailing '%'), a NULL `fmt` with a positive length or `ns_per_unit` <= 0. */
DFTU_EXPORT dftu_expr* dftu_expr_format_time(const dftu_expr* a,
                                             const char* fmt, int32_t fmt_len,
                                             int64_t ns_per_unit);
/** Per-row byte length, or codepoint count when `chars` is nonzero. */
DFTU_EXPORT dftu_expr* dftu_expr_str_len(const dftu_expr* a, int32_t chars);
/** Byte index of the first `needle` per row, or -1. */
DFTU_EXPORT dftu_expr* dftu_expr_str_find(const dftu_expr* a,
                                          const char* needle,
                                          int32_t needle_len);
/** Replace the first (or every, when `all` is nonzero) `from` with `to`. */
DFTU_EXPORT dftu_expr* dftu_expr_str_replace(const dftu_expr* a,
                                             const char* from, int32_t from_len,
                                             const char* to, int32_t to_len,
                                             int32_t all);
/** The byte substring [start, start + len) of each row. */
DFTU_EXPORT dftu_expr* dftu_expr_str_slice(const dftu_expr* a, int64_t start,
                                           int64_t len);
/** Membership in `values` (borrowed for the call; the node shares its
 * buffers). NULL if `values` is NULL. */
DFTU_EXPORT dftu_expr* dftu_expr_is_in(const dftu_expr* a,
                                       const dftu_series* values);
/** `cond ? a : b` per row. NULL if any handle is NULL. */
DFTU_EXPORT dftu_expr* dftu_expr_select(const dftu_expr* cond,
                                        const dftu_expr* a, const dftu_expr* b);
/** The Bool mask of the rows where `a` is null (`null` nonzero) or present
 * (`null` zero). NULL if `a` is NULL. */
DFTU_EXPORT dftu_expr* dftu_expr_is_null(const dftu_expr* a, int32_t null);
/** As dftu_expr_is_null for missing data in the pandas sense: `na` nonzero is
 * true where `a` is null or, in a float operand, NaN; zero is its complement.
 * NULL if `a` is NULL. */
DFTU_EXPORT dftu_expr* dftu_expr_is_na(const dftu_expr* a, int32_t na);

/* duql semantics (see expr.h): an unknown result is a null cell. Each builder
 * returns NULL when a handle it needs is NULL. The n-ary builders borrow
 * `args[0..n)` for the call and return NULL when `n` <= 0. */

/** A String literal; `value` is copied. */
DFTU_EXPORT dftu_expr* dftu_expr_lit_str(const char* value, int32_t len);
DFTU_EXPORT dftu_expr* dftu_expr_lit_bool(int32_t value);
/** A null literal of dftu_dtype `type`. */
DFTU_EXPORT dftu_expr* dftu_expr_lit_null(int32_t type);
/** Checked arithmetic; `op` is 0 add, 1 sub, 2 mul, 3 div, 4 floor div,
 * 5 mod. */
DFTU_EXPORT dftu_expr* dftu_expr_arith(int32_t op, const dftu_expr* a,
                                       const dftu_expr* b);
DFTU_EXPORT dftu_expr* dftu_expr_neg(const dftu_expr* a);
/** `a <cmp> b` (cmp is a dftu_cmp_op), exact across integers and doubles. */
DFTU_EXPORT dftu_expr* dftu_expr_cmp_expr(int32_t cmp, const dftu_expr* a,
                                          const dftu_expr* b);
DFTU_EXPORT dftu_expr* dftu_expr_coalesce(const dftu_expr* const* args,
                                          int32_t n);
/** The least (`least` nonzero) or greatest operand per row. */
DFTU_EXPORT dftu_expr* dftu_expr_extreme(const dftu_expr* const* args,
                                         int32_t n, int32_t least);
DFTU_EXPORT dftu_expr* dftu_expr_concat(const dftu_expr* const* args,
                                        int32_t n);
DFTU_EXPORT dftu_expr* dftu_expr_round(const dftu_expr* a, int64_t digits);
/** Round `a` to `digits` decimals per row, `digits` an integer expression;
 * null where `digits` is null or not an integer, and for a float `a` with
 * `digits` outside [-308, 308]. Integer `a` with `digits` >= 0 is unchanged
 * (Int64). Ownership as dftu_expr_str_pred_col; returns NULL when `a` or
 * `digits` is NULL. */
DFTU_EXPORT dftu_expr* dftu_expr_round_col(const dftu_expr* a,
                                           const dftu_expr* digits);
DFTU_EXPORT dftu_expr* dftu_expr_log(const dftu_expr* a);
DFTU_EXPORT dftu_expr* dftu_expr_pow(const dftu_expr* a, const dftu_expr* b);
/** `len` UTF-8 characters from character `start`; `len` < 0 to the end. */
DFTU_EXPORT dftu_expr* dftu_expr_str_substr(const dftu_expr* a, int64_t start,
                                            int64_t len);
/** `op` is 0 int, 1 float, 2 string, 3 json. */
DFTU_EXPORT dftu_expr* dftu_expr_convert(int32_t op, const dftu_expr* a);
DFTU_EXPORT dftu_expr* dftu_expr_list_len(const dftu_expr* a);
/** The element at `index`, from the end when negative. */
DFTU_EXPORT dftu_expr* dftu_expr_list_get(const dftu_expr* a, int64_t index);
DFTU_EXPORT dftu_expr* dftu_expr_list_sum(const dftu_expr* a);
/** Whether an element equals `value`; a string `value` is copied. */
DFTU_EXPORT dftu_expr* dftu_expr_list_contains(const dftu_expr* a,
                                               dftu_scalar value);
DFTU_EXPORT void dftu_expr_free(dftu_expr* e);

/* Inspecting a borrowed expression, for a source translating a pushed-down
 * predicate. Each dftu_expr_as_* returns nonzero on a match and writes nothing
 * otherwise. A child handle written out is NEW and owned by the caller
 * (dftu_expr_free); a borrowed string or scalar stays valid while `e` lives. */

/** The column index when `e` is a bare column reference, else -1. */
DFTU_EXPORT int32_t dftu_expr_col_index(const dftu_expr* e);
/** `col <cmp> rhs`: `*cmp` is a dftu_cmp_op code; a string `rhs` borrows. */
DFTU_EXPORT int32_t dftu_expr_as_col_cmp(const dftu_expr* e, int32_t* col,
                                         int32_t* cmp, dftu_scalar* rhs);
/** `a <op> b`: `*op` is a dftu_logical_op code. */
DFTU_EXPORT int32_t dftu_expr_as_logical(const dftu_expr* e, int32_t* op,
                                         dftu_expr** a, dftu_expr** b);
/** `not a`. */
DFTU_EXPORT int32_t dftu_expr_as_not(const dftu_expr* e, dftu_expr** a);
/** `col <op> pattern`: `*op` is a dftu_str_pred_op code; `pattern` borrows. */
DFTU_EXPORT int32_t dftu_expr_as_col_str_pred(const dftu_expr* e, int32_t* col,
                                              int32_t* op, const char** pattern,
                                              int32_t* pattern_len);
/** `col is_in values`: `*values` is a NEW column (dftu_series_free). */
DFTU_EXPORT int32_t dftu_expr_as_col_is_in(const dftu_expr* e, int32_t* col,
                                           dftu_series** values);

/** Compile and evaluate `root` over `n_inputs` columns. Returns an owned
 * column, or NULL on a malformed expression. */
DFTU_EXPORT dftu_series* dftu_expr_eval(const dftu_expr* root,
                                        const dftu_series* const* inputs,
                                        int32_t n_inputs);

/** Compile `n_roots` expressions into one program (CSE spans them; only
 * referenced inputs are materialized) and evaluate them in a single pass.
 * Writes one owned column per root into `out[0..n_roots)` and returns n_roots,
 * or -1 on a malformed expression (writing nothing). */
DFTU_EXPORT int32_t dftu_expr_eval_many(const dftu_expr* const* roots,
                                        int32_t n_roots,
                                        const dftu_series* const* inputs,
                                        int32_t n_inputs, dftu_series** out);

/** Wrap a materialized frame as a deferred query over a self-contained
 * in-memory source. The frame's columns are shared (no data copy) into a source
 * that owns them, so the result outlives `df` and never references external
 * scan/executor state; this is the supported way to produce a lazy result that
 * crosses the plugin ABI. `df` is borrowed, not consumed. NULL if `df` is
 * NULL. */
DFTU_EXPORT dftu_lazyframe* dftu_dataframe_lazy(const dftu_dataframe* df);

/** Run the deferred pipeline and materialize the surviving rows into a new
 * owned frame (free with dftu_dataframe_free). Does NOT consume `lf`: it stays
 * valid and re-runnable. `batch_size` is the scan chunk size; <= 0 means auto.
 * NULL on error. */
DFTU_EXPORT dftu_dataframe* dftu_lazyframe_collect(dftu_lazyframe* lf,
                                                   int64_t batch_size);

/** Write the deferred pipeline's rows to the sink registered as `name` instead
 * of returning them, then finalize the output. Does NOT consume `lf`: it stays
 * valid and re-runnable. `option_keys[i]` and `option_values[i]` are one
 * name/value pair each, both NUL-terminated and borrowed for the call, in the
 * order the sink receives them; `n_options` is their count, and both arrays are
 * unused (and may be NULL) when it is 0. `rows_out` and `batches_out` are
 * caller-owned and may each be NULL; on success they receive the rows written
 * and the frames they arrived in. Returns 0 on success. On any error - an
 * unknown sink name, a failed bind, a failed read or write - nothing is written
 * through the out pointers, the output is aborted exactly once, and the return
 * is non-zero. Returns non-zero and writes nothing when `lf` or `name` is NULL,
 * or when `n_options` is non-zero and an entry of either array is NULL. */
DFTU_EXPORT int32_t dftu_lazyframe_sink(const dftu_lazyframe* lf,
                                        const char* name,
                                        const char* const* option_keys,
                                        const char* const* option_values,
                                        uint32_t n_options, int64_t* rows_out,
                                        int64_t* batches_out);

/** Free a lazyframe handle. */
DFTU_EXPORT void dftu_lazyframe_free(dftu_lazyframe* lf);

/** A plan over the file at `path`, opened by the plugin file format that
 * claims it (dftu_file_format_vt, registered through
 * dftu_svc_file_formats). `path` is a NUL-terminated string borrowed for the
 * call. `index_dir` is the index directory the scan reads to skip the chunks
 * an index built by the Indexer proves hold no matching row: `index_dir_len`
 * bytes, borrowed for the call and copied, not NUL-terminated; NULL (with
 * `index_dir_len` 0) means the index beside the file. A file with no index, or
 * one that changed since it was indexed, is read whole. Writes a new
 * caller-owned handle to `*out` and returns 0; the caller frees it with
 * dftu_lazyframe_free. A file no format claims, two formats that tie for it, or
 * a failing plugin all produce a plan whose dftu_lazyframe_collect returns
 * NULL, exactly as they do in C++; to tell them apart, read the reason from the
 * C++ LazyFrame::status(). Returns non-zero and writes nothing when `path` or
 * `out` is NULL, or `index_dir` is NULL and `index_dir_len` is not 0. */
DFTU_EXPORT int dftu_lazyframe_scan_file(const char* path,
                                         const char* index_dir,
                                         size_t index_dir_len,
                                         dftu_lazyframe** out);

/** Remove the file format registered as `name`. Only the name is removed: a
 * plan that already opened a file through it keeps running, and a later scan
 * of a new path no longer finds it. Returns 0 on success, non-zero if `name`
 * is NULL or unregistered. A plugin host calls this for every format a plugin
 * registered before it unloads the plugin. */
DFTU_EXPORT int dftu_format_unregister(const char* name);

/** Output column names without running the query, joined by '\n' into a new
 * malloc'd C string (empty for a plan ending in a data-dependent op). The
 * caller frees it with dftu_duql_string_free. NULL on error. */
DFTU_EXPORT char* dftu_lazyframe_schema(const dftu_lazyframe* lf);

/** The optimized plan as text (one op per line), for introspection. A new
 * malloc'd C string the caller frees with dftu_duql_string_free. NULL on
 * error. */
DFTU_EXPORT char* dftu_lazyframe_explain(const dftu_lazyframe* lf);

/** Run the plan to its end, dropping the rows, and return the plan as
 * dftu_lazyframe_explain does with the rows, time and filter votes each stage
 * measured and the counters plugins added during the run. `batch_size` is the
 * scan chunk size, as for dftu_lazyframe_collect. Does NOT consume `lf`. A new
 * malloc'd C string the caller frees with dftu_duql_string_free. NULL on error,
 * including a run that fails and a second analysis started while one runs. */
DFTU_EXPORT char* dftu_lazyframe_explain_analyze(const dftu_lazyframe* lf,
                                                 int64_t batch_size);

/** Keep rows where `pred` (a dftu_expr over the frame's columns by position) is
 * true. `pred` is borrowed. NULL on error. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_filter(const dftu_lazyframe* lf,
                                                  const dftu_expr* pred);

/** Project to the `n` columns named by `names`, in that order. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_select(const dftu_lazyframe* lf,
                                                  const char* const* names,
                                                  int32_t n);

/** Add or replace column `name` with the value of `expr` (a dftu_expr over the
 * frame's columns by position). `expr` is borrowed. NULL on error. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_with_column(const dftu_lazyframe* lf,
                                                       const char* name,
                                                       const dftu_expr* expr);

/** Group by the `n_keys` key columns named by `keys` (a composite key) and
 * compute each aggregate in `aggs` (op|column|out, as
 * dftu_dataframe_group_by_dynamic). NULL on an unknown column/op. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_group_by(const dftu_lazyframe* lf,
                                                    const char* const* keys,
                                                    int32_t n_keys,
                                                    const dftu_group_agg* aggs,
                                                    int32_t n_aggs);

/** Sort by the column `name`, ascending unless `descending`. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_sort_by(const dftu_lazyframe* lf,
                                                   const char* name,
                                                   int32_t descending);
/** The same with the null keys last when `nulls_last` is nonzero, first
 * otherwise. The caller frees the new plan with dftu_lazyframe_free. NULL if
 * `lf` or `name` is NULL. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_sort_by_nulls(
    const dftu_lazyframe* lf, const char* name, int32_t descending,
    int32_t nulls_last);

/** First / last `n` rows (head is the row-limit form). */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_head(const dftu_lazyframe* lf,
                                                int64_t n);
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_tail(const dftu_lazyframe* lf,
                                                int64_t n);

/** Drop every row that is null in ANY column. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_drop_nulls(const dftu_lazyframe* lf);
/** Drop every row that holds a NaN in any float column. The caller owns the
 * result and frees it with dftu_lazyframe_free. NULL if `lf` is NULL. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_drop_nans(const dftu_lazyframe* lf);

/** Distinct rows (keep first), in original order. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_unique(const dftu_lazyframe* lf);
/** Distinct rows keyed on the `n` `subset` columns only. NULL if a handle is
 * NULL, `n` is 0 or the plan's schema lacks a name. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_unique_by(const dftu_lazyframe* lf,
                                                     const char* const* subset,
                                                     int32_t n);

/** Alias of dftu_lazyframe_unique. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_drop_duplicates(
    const dftu_lazyframe* lf);

/** For each distinct tuple of the `n_keys` columns named by `keys`, the first
 * `n` rows with that key, in input order (LazyFrame::head_by). `keys` is
 * borrowed and may be NULL when `n_keys` is 0. Returns a new owned plan (free
 * with dftu_lazyframe_free); NULL if `lf` is NULL, `n_keys` is negative, a
 * key is NULL or the plan's schema lacks a name. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_head_by(const dftu_lazyframe* lf,
                                                   const char* const* keys,
                                                   int32_t n_keys, int64_t n);

/** Replace column names positionally with the `n` `names`, keeping column
 * order. `n` must match the frame's column count; a mismatch surfaces as a
 * collect error, not here. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_rename(const dftu_lazyframe* lf,
                                                  const char* const* names,
                                                  int32_t n);

/** Remove the `n` columns named by `names`, including the columns a streaming
 * source adds per batch. A name no column carries is ignored. `names` is
 * borrowed and may be NULL when `n` is 0. Returns a new owned plan the caller
 * frees with dftu_lazyframe_free. NULL if `lf` is NULL, `n` is negative or a
 * name is NULL. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_drop(const dftu_lazyframe* lf,
                                                const char* const* names,
                                                int32_t n);

/** Rename each of the `n_from` columns in `from` to the name at the same index
 * of `to` (`n_to` must equal `n_from`), including columns a streaming source
 * adds per batch. A `from` name no column carries is ignored. Both arrays are
 * borrowed and may be NULL when their count is 0. Returns a new owned plan the
 * caller frees with dftu_lazyframe_free. NULL if `lf` is NULL, a count is
 * negative or differs from the other, or a name is NULL. A rename that leaves
 * two columns with one name surfaces as an error when the plan is lowered or
 * collected, not here. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_rename_columns(
    const dftu_lazyframe* lf, const char* const* from, int32_t n_from,
    const char* const* to, int32_t n_to);

/** Keep `len` rows starting at `offset` (negative `offset` counts from the
 * end, as DataFrame::slice). */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_slice(const dftu_lazyframe* lf,
                                                 int64_t offset, int64_t len);

/** Replace every null with `value`, cast to each column's type. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_fill_null(const dftu_lazyframe* lf,
                                                     dftu_scalar value);
/** Replace the NaN of every float column with `value` (a NULL scalar makes it
 * null); other columns pass through. The caller owns the result and frees it
 * with dftu_lazyframe_free. NULL if `lf` is NULL or `value` is a string or
 * error scalar. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_fill_nan(const dftu_lazyframe* lf,
                                                    dftu_scalar value);

/** Append an Int64 row-index column named `name`, starting at 0. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_with_row_index(
    const dftu_lazyframe* lf, const char* name);

/** A one-row frame of each column's null count. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_null_count(const dftu_lazyframe* lf);

/** dftu_dataframe_reduce as a plan step: a streaming one-group group-by with
 * `agg` broadcast over the eligible columns the plan's schema shows. NULL if
 * `lf` is NULL or `agg` is not a broadcastable code. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_reduce(const dftu_lazyframe* lf,
                                                  int32_t agg);

/** dftu_dataframe_group_transform as a plan step: the window over a
 * prepended row index, the sort back to input order and the projection, all
 * deferred. Same contract and refusals, as NULL here or at collect. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_group_transform(
    const dftu_lazyframe* lf, const char* const* keys, int32_t n_keys,
    int32_t kind, int64_t n, int32_t rank_method, int32_t ascending);

/** Expand a List `column`: each element becomes its own row. An unknown
 * column or a non-List column surfaces as a collect error, not here. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_explode(const dftu_lazyframe* lf,
                                                   const char* column);

/** Reshape wide -> long: keep the `n_id` `id_vars` columns, stack the `n_val`
 * `value_vars` columns into `variable`/`value` columns, as
 * dftu_dataframe_unpivot. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_unpivot(
    const dftu_lazyframe* lf, const char* const* id_vars, int32_t n_id,
    const char* const* value_vars, int32_t n_val);

/** Alias of dftu_lazyframe_unpivot. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_melt(const dftu_lazyframe* lf,
                                                const char* const* id_vars,
                                                int32_t n_id,
                                                const char* const* value_vars,
                                                int32_t n_val);

/** The `k` rows with the largest (or smallest, when `largest` is 0) `name`
 * values. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_topk(const dftu_lazyframe* lf,
                                                const char* name, int64_t k,
                                                int32_t largest);

/** A deterministic n-row sample (mix64 min-hash), bounded to `n` rows. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_sample(const dftu_lazyframe* lf,
                                                  int64_t n, uint64_t seed);

/** One Bool column: true where the whole row is duplicated. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_is_duplicated(
    const dftu_lazyframe* lf);

/** One Bool column: true where the whole row is unique. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_is_unique(const dftu_lazyframe* lf);

/** Tumbling/sliding time-window aggregation over an ascending Int64
 * `time_col`, as dftu_dataframe_group_by_dynamic, plus `origin` (the window
 * grid's zero point) and `origin_min` (nonzero: start the grid at the data's
 * first timestamp instead of `origin`). An invalid `every`, an unknown
 * `time_col`, or an agg naming an unknown column/op surfaces as a collect
 * error, not here. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_group_by_dynamic(
    const dftu_lazyframe* lf, const char* time_col, int64_t every,
    int64_t period, const dftu_group_agg* aggs, int32_t n_aggs, int64_t origin,
    int32_t origin_min);

/** Reshape long -> wide: rows are the distinct `index` values, one value
 * column per distinct `on` value, each cell `values` aggregated over the
 * matching rows. `agg` is the collision reducer: first|last|sum|min|max|mean
 * (NULL defaults to "first"). Output columns are data-dependent, so
 * dftu_lazyframe_schema is empty until collect. An unknown column or agg
 * surfaces as a collect error, not here. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_pivot(const dftu_lazyframe* lf,
                                                 const char* index,
                                                 const char* on,
                                                 const char* values,
                                                 const char* agg);

/** One-hot encode `column`: replace it with one Int8 column per distinct
 * value, named `<column>_<value>`. Output columns are data-dependent, so
 * dftu_lazyframe_schema is empty until collect. An unknown column surfaces as
 * a collect error, not here. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_to_dummies(const dftu_lazyframe* lf,
                                                      const char* column);

/** Per-column summary statistics (count/null_count/mean/std/min/max). Output
 * columns are data-dependent, so dftu_lazyframe_schema is empty until
 * collect. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_describe(const dftu_lazyframe* lf);

/** Out-of-core budget for the pipeline breakers (sort/unique/group_by): when a
 * sink's in-memory state grows past `bytes` it spills to a sorted temp run,
 * k-way merged at collect. 0 means "auto" (~1/3 of available memory). */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_memory_budget(
    const dftu_lazyframe* lf, uint64_t bytes);

/** Sugar for dftu_lazyframe_memory_budget(lf, 0): explicitly request the
 * default auto-spill budget. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_auto_spill(const dftu_lazyframe* lf);

/** Keep rows where the positionally-aligned `mask` is true, as
 * DataFrame::filter. `mask` is borrowed. NULL if `lf` or `mask` is NULL. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_filter_mask(const dftu_lazyframe* lf,
                                                       const dftu_series* mask);

/** Rows in reverse order, as DataFrame::reverse. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_reverse(const dftu_lazyframe* lf);

/** Rows at `idx` (0-based, may repeat or reorder), as DataFrame::take. `idx`
 * is borrowed for the call. NULL if `lf` is NULL. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_take(const dftu_lazyframe* lf,
                                                const int64_t* idx, int32_t n);

/** Stable lexicographic sort by the `n` key columns named by `by`, ascending
 * unless `descending`, as DataFrame::sort_by_multi. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_sort_by_multi(
    const dftu_lazyframe* lf, const char* const* by, int32_t n,
    int32_t descending);
/** The same with the null keys last when `nulls_last` is nonzero, first
 * otherwise. The caller frees the new plan with dftu_lazyframe_free. NULL if
 * `lf` is NULL, `n` is negative, or `by` is NULL while `n` is above 0. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_sort_by_multi_nulls(
    const dftu_lazyframe* lf, const char* const* by, int32_t n,
    int32_t descending, int32_t nulls_last);

/** Join `lf` (left) with `other` (right), as dftu_dataframe_join. The right
 * plan is collected in full when the joined plan runs (the hash build side);
 * the left plan streams through it morsel by morsel. `other` is borrowed: the
 * returned plan holds its own copy. NULL if either handle is NULL, `n` is 0
 * for a keyed join, `how` is not a dftu_join_how, or `nulls_equal` is nonzero
 * for CROSS, LOOKUP or NEST; an absent key column or a key type mismatch
 * surfaces as a collect error. A join with `nulls_equal` is not offered to a
 * source's apply. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_join(
    const dftu_lazyframe* lf, const dftu_lazyframe* other,
    const char* const* left_on, const char* const* right_on, int32_t n,
    dftu_join_how how, const char* suffix, int32_t nulls_equal);

/** Compare two aggregation results sharing their first `n_key` key columns:
 * keys (outer-joined, sorted ascending), `l_<m>` / `r_<m>` per metric, then
 * `delta_<m>` and `pct_<m>` per numeric metric. NULL if either handle is
 * NULL, `n_key < 1` or exceeds a width, or a key name differs. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_compare_agg(const dftu_dataframe* a,
                                                       const dftu_dataframe* b,
                                                       int64_t n_key);

/** UNNEST a List `column` per morsel, as dftu_dataframe_unnest. NULL if a
 * handle or `column` is NULL, or the plan's schema shows `column` absent or
 * not a List. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_unnest(const dftu_lazyframe* lf,
                                                  const char* column,
                                                  int32_t keep_empty);

/** dftu_dataframe_compare_agg over the two collected plans. NULL if a handle
 * is NULL or `n_key < 1`; a key mismatch surfaces at collect. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_compare_agg(
    const dftu_lazyframe* lf, const dftu_lazyframe* variant, int64_t n_key);

/** Every row of `lf`, then every row of `other`, as
 * dftu_dataframe_concat2(DFTU_CONCAT_VERTICAL). Both plans must have the same
 * column names in the same order with the same types where known. `other` is
 * borrowed: the returned plan holds its own copy. NULL if either handle is
 * NULL or the schemas differ. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_concat(const dftu_lazyframe* lf,
                                                  const dftu_lazyframe* other);

/* ---- Relational kernels over one table ----------------------------------- */
/* The window, gap-fill, as-of and interval ops run over a table exported to
 * Arrow and are implemented by the utilities library, which registers them as
 * dftu.frame.window / gap_fill / asof / interval at load. The operand types
 * live here so the op runner can unpack them. */

/** One window function; mirrors the utilities WindowFunc. */
typedef int32_t dftu_window_func;
enum {
    DFTU_WINDOW_ROW_NUMBER = 0,
    DFTU_WINDOW_RANK = 1,
    DFTU_WINDOW_DENSE_RANK = 2,
    DFTU_WINDOW_LAG = 3,
    DFTU_WINDOW_LEAD = 4,
    DFTU_WINDOW_RUNNING_SUM = 5,
    DFTU_WINDOW_RUNNING_MIN = 6,
    DFTU_WINDOW_RUNNING_MAX = 7,
    DFTU_WINDOW_RUNNING_COUNT = 8,
    DFTU_WINDOW_DELTA = 9,
    DFTU_WINDOW_RATE = 10,
    DFTU_WINDOW_SESSIONIZE = 11,
    DFTU_WINDOW_FRAME_SUM = 12,
    DFTU_WINDOW_FRAME_MIN = 13,
    DFTU_WINDOW_FRAME_MAX = 14,
    DFTU_WINDOW_FRAME_COUNT = 15,
    DFTU_WINDOW_FRAME_MEAN = 16,
    DFTU_WINDOW_NTILE = 17,
    DFTU_WINDOW_FIRST_VALUE = 18,
    DFTU_WINDOW_LAST_VALUE = 19,
    DFTU_WINDOW_NTH_VALUE = 20,
    DFTU_WINDOW_PERCENT_RANK = 21,
    DFTU_WINDOW_CUME_DIST = 22,
    DFTU_WINDOW_FILL_FORWARD = 23,
    DFTU_WINDOW_RUNNING_PROD = 24,
    DFTU_WINDOW_FRAME_VAR = 25,
    DFTU_WINDOW_FRAME_STD = 26,
    DFTU_WINDOW_FRAME_QUANTILE = 27,
    DFTU_WINDOW_FRAME_COUNT_DISTINCT = 28,
    DFTU_WINDOW_FRAME_ARG_MAX = 29,
    DFTU_WINDOW_FRAME_ARG_MIN = 30,
    DFTU_WINDOW_FRAME_COLLECT = 31,
    DFTU_WINDOW_FRAME_UDAF = 32
};

/** How a FRAME_* bound reads; mirrors the utilities WindowFrameMode. */
typedef int32_t dftu_window_frame_mode;
enum { DFTU_WINDOW_FRAME_ROWS = 0, DFTU_WINDOW_FRAME_RANGE = 1 };

/** A FRAME_* bound that is unbounded on its side. */
#define DFTU_WINDOW_UNBOUNDED INT64_MAX

/** One appended window column: `func` over `value` (a column name, or NULL
 * when the function takes none) into `out`. FILL_FORWARD is the nearest
 * present value at or before the row in the partition; RUNNING_PROD the
 * product of the present values so far (Int64 or UInt64 over an integer
 * column, wrapping on overflow; Float64 over a float one). `param` holds what
 * only some functions read, and `func` names the member that is set:
 * - `offset`: the LAG/LEAD shift, NTILE bucket count or NTH_VALUE 1-based k.
 * - `frame` (FRAME_*): the present count below which the output is null (0 =
 *   none), the bounds (DFTU_WINDOW_UNBOUNDED = no bound) and their `mode`:
 *   row offsets, or value deltas on the single numeric order column. The
 *   frame functions after FRAME_MEAN skip null values: FRAME_VAR and
 *   FRAME_STD (sample, Float64, null below 2 values), FRAME_QUANTILE
 *   (Float64, exact, linear interpolation at level `q` in [0, 1]; any other
 *   `q` fails), FRAME_COUNT_DISTINCT (Int64, 0 for an empty frame),
 *   FRAME_ARG_MAX / FRAME_ARG_MIN (the `value` at the earliest frame row with
 *   the extreme `by`, null for an empty frame; `by` names the numeric, string
 *   or bool ordering column and is borrowed for the call, NULL = error) and
 *   FRAME_COLLECT (a List of the frame's values in frame order, empty for an
 *   empty frame; more than 2^27 values in all fails). `q` is read by
 *   FRAME_QUANTILE only and `by` by FRAME_ARG_MAX / FRAME_ARG_MIN only; the
 *   other frame functions ignore them. FRAME_COUNT_DISTINCT and FRAME_COLLECT
 *   ignore `min_count`, as FRAME_COUNT does.
 * - `udaf` (FRAME_UDAF): the first four members are those of `frame` and mean
 *   the same. It runs the user-defined aggregate `name` (see c_api_udaf.h)
 *   over the `arg_count` columns named in `args`, bound to their types;
 *   `value` is not read. A UDAF with
 *   DFTU_AGG_HAS_RETRACT slides the frame, one state per partition; any other
 *   UDAF is recomputed over each frame. A row where an argument is null is
 *   left out of the frame unless the UDAF is DFTU_AGG_NULL_AWARE, and the
 *   output is null when no row is in the frame or fewer than `min_count`
 *   are. `name`, `args` and each name in it are borrowed for the call; an
 *   unknown UDAF or a failing callback fails the call. `name` and `out` must
 *   not be NULL, `arg_count` must not be negative, and `args` and each name
 *   in it must not be NULL when `arg_count` is above 0; a spec that breaks
 *   a rule fails the call with NULL.
 * - `rate` (RATE): the time column and, when `counter` is nonzero, the
 *   counter-reset correction.
 * - `session` (SESSIONIZE): the time column, the row-end column (NULL = each
 *   row ends at its time), the gap and the longest session measured from its
 *   first time (0 = no limit), both in the time's unit and not negative. */
typedef struct dftu_window_spec {
    dftu_window_func func;
    const char* value;
    const char* out;
    union {
        int64_t offset;
        struct {
            int64_t min_count;
            int64_t preceding;
            int64_t following;
            dftu_window_frame_mode mode;
            double q;
            const char* by;
        } frame;
        struct {
            int64_t min_count;
            int64_t preceding;
            int64_t following;
            dftu_window_frame_mode mode;
            const char* name;
            const char* const* args;
            int32_t arg_count;
        } udaf;
        struct {
            const char* time;
            int32_t counter;
        } rate;
        struct {
            const char* time;
            const char* end;
            double gap;
            double span;
        } session;
    } param;
} dftu_window_spec;

/** How a generated gap-fill row fills its value columns; mirrors the
 * utilities GapFillMode. */
typedef int32_t dftu_gap_fill_mode;
enum {
    DFTU_GAP_FILL_NONE = 0,
    DFTU_GAP_FILL_LOCF = 1,
    DFTU_GAP_FILL_LINEAR = 2
};

/** Which right row an as-of join picks; mirrors the utilities AsofDirection.
 */
typedef int32_t dftu_asof_direction;
enum { DFTU_ASOF_BACKWARD = 0, DFTU_ASOF_FORWARD = 1, DFTU_ASOF_NEAREST = 2 };

/** SQL window functions over `df`: PARTITION BY `partition_by` (n_part
 * names), ORDER BY `order_by` (n_order names), one appended column per spec.
 * Every input column comes first, then one column per spec, in (partition,
 * order) sorted order with ties in row order and nulls last. NULL on a NULL
 * handle, an unknown column or an unsupported type. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_window(
    const dftu_dataframe* df, const char* const* partition_by, int32_t n_part,
    const char* const* order_by, int32_t n_order, const dftu_window_spec* specs,
    int32_t n_specs);

/** A regular grid of width `bucket` on the integer `time` column, PARTITION
 * BY `partition_by`; `values` (n_values names) fill on generated rows per
 * `mode`. `range` is either NULL / n_range 0 (each partition's own extent) or
 * two int64 [start, end]. NULL on a NULL handle, an unknown column, a bad
 * `mode` or an n_range other than 0 or 2. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_gap_fill(
    const dftu_dataframe* df, const char* const* partition_by, int32_t n_part,
    const char* time, int64_t bucket, const char* const* values,
    int32_t n_values, dftu_gap_fill_mode mode, const int64_t* range,
    int32_t n_range);

/** As-of join of `left` to `right` on the time column `on` within the `by`
 * partition (n_by names); `tolerance` is in the units of `on`, and a
 * negative or NaN one means unbounded. NULL on a NULL handle, an unknown
 * column, a bad `direction` or a type mismatch. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_asof(
    const dftu_dataframe* left, const dftu_dataframe* right, const char* on,
    const char* const* by, int32_t n_by, dftu_asof_direction direction,
    double tolerance);

/** Point-in-range join of `left.point` into `right.[lo, hi]` within the `by`
 * partition (n_by names); nonzero `outer` keeps unmatched left rows with null
 * right values. NULL on a NULL handle, an unknown column or a type mismatch. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_interval(
    const dftu_dataframe* left, const dftu_dataframe* right, const char* point,
    const char* lo, const char* hi, const char* const* by, int32_t n_by,
    int32_t outer);

/** dftu_dataframe_asof over the two plans. Under a memory budget both sides
 * are sorted by (by, `on`) with the spilling sort and merged, so the plan
 * holds only the right rows of the current `by` group window and leaves in
 * morsels within the budget; without one both are collected. The output is
 * the same as dftu_dataframe_asof. `other` is borrowed: the returned plan holds
 * its own copy. The returned plan is owned by the caller and released with
 * dftu_lazyframe_free. NULL on a NULL handle, `on` or `by` (when `n_by` > 0),
 * a bad `direction`, or a column the schemas show absent or of mismatched or
 * non-numeric type; the same errors found only when the plan runs surface at
 * collect. `tolerance` is as in dftu_dataframe_asof. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_asof(
    const dftu_lazyframe* lf, const dftu_lazyframe* other, const char* on,
    const char* const* by, int32_t n_by, dftu_asof_direction direction,
    double tolerance);

/** dftu_dataframe_interval over the two plans, streamed under a memory budget
 * as dftu_lazyframe_asof is: the ranges open at the current point stay in
 * memory and the pairs of one point leave in morsels within the budget.
 * Ownership and error behavior are those of dftu_lazyframe_asof. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_interval(
    const dftu_lazyframe* lf, const dftu_lazyframe* other, const char* point,
    const char* lo, const char* hi, const char* const* by, int32_t n_by,
    int32_t outer);

/* ---- Provider registry --------------------------------------------------- */
/* One name-keyed registry of sources, so a LazyFrame can be built by name over
 * a provider registered from anywhere in the process. Mirrors the op registry
 * below: process-lifetime, name-keyed, and a name already taken is refused
 * rather than replaced. */

/** Opaque per-column type declaration a provider builds to declare its column
   types (mirrors dataframe::Schema/DataType, types.h). The host
   allocates and frees it; a source only appends fields into the instance it
   is handed. */
typedef struct dftu_schema dftu_schema;

/** Append a field to `schema`. `name` is copied; `timezone` is copied if
   non-NULL and ignored (may be NULL) for every type but Timestamp, where NULL
   means no timezone. `time_unit`, `decimal_precision`, `decimal_scale`, and
   `fixed_size` are the matching dataframe::DataType parameters (types.h) and
   are likewise ignored where `type` does not use them. Returns the field's
   index within `schema` (>= 0) - pass it as `parent_index` to
   dftu_schema_add_child_field to nest a child under a List/LargeList/
   FixedSizeList/Struct/Map field - or -1 if `schema` or `name` is NULL, or
   `type` is out of range. */
DFTU_EXPORT int32_t dftu_schema_add_field(
    dftu_schema* schema, const char* name, dftu_dtype type, int32_t nullable,
    dftu_time_unit time_unit, const char* tz, int32_t decimal_precision,
    int32_t decimal_scale, int32_t fixed_size);

/** Append a child field nested under `parent_index`, a field already added to
   `schema` whose type is List/LargeList/FixedSizeList/Struct/Map: the single
   element/entry type for a List-like parent, or one member per call for a
   Struct parent. Same parameters and return convention as
   dftu_schema_add_field. Returns -1 if `parent_index` is out of range or
   names a field whose type cannot nest a child. */
DFTU_EXPORT int32_t dftu_schema_add_child_field(
    dftu_schema* schema, int32_t parent_index, const char* name,
    dftu_dtype type, int32_t nullable, dftu_time_unit time_unit, const char* tz,
    int32_t decimal_precision, int32_t decimal_scale, int32_t fixed_size);

/** Number of top-level fields in `schema`, or -1 if `schema` is NULL. Reads a
   schema a caller was only handed (a node's input schema), which
   otherwise exposes no way to enumerate what an upstream stage declared. */
DFTU_EXPORT int32_t dftu_schema_field_count(const dftu_schema* schema);

/** The name of top-level field `i` of `schema`, borrowed from the schema, or
   NULL if `schema` is NULL or `i` is out of range. */
DFTU_EXPORT const char* dftu_schema_field_name(const dftu_schema* schema,
                                               int32_t i);

/** The dftu_dtype code of top-level field `i`, or -1 if `schema` is NULL or
   `i` is out of range (DFTU_TYPE_UNKNOWN is a valid field type, so it cannot
   double as the error). */
DFTU_EXPORT int32_t dftu_schema_field_type(const dftu_schema* schema,
                                           int32_t i);

/** Whether top-level field `i` is nullable: 1 or 0, or -1 if `schema` is NULL
   or `i` is out of range. */
DFTU_EXPORT int32_t dftu_schema_field_nullable(const dftu_schema* schema,
                                               int32_t i);

/** The dftu_time_unit code field `i` carries, which a Timestamp, Time32,
   Time64 or Duration field uses (DFTU_TIME_UNIT_MICRO for any other type), or
   -1 if `schema` is NULL or `i` is out of range. */
DFTU_EXPORT int32_t dftu_schema_field_time_unit(const dftu_schema* schema,
                                                int32_t i);

/** Appends top-level field `i` of `schema` (name, type, and every parameter,
   including nested children) as a new field of `out`. Returns the new
   field's index in `out` (same convention as dftu_schema_add_field), or -1 if
   `out`/`schema` is NULL or `i` is out of range. The pass-through a node's
   output_schema uses to declare a column unchanged from its input. */
DFTU_EXPORT int32_t dftu_schema_copy_field(dftu_schema* out,
                                           const dftu_schema* schema,
                                           int32_t i);

/** Remove the provider registered as `name`. A caller that can unload (a
 * plugin's shared object) must call this before unloading, the same
 * requirement dftu_op_unregister documents for a user op - otherwise a later
 * dftu_lazyframe_from_provider or in-flight scan calls into unmapped memory.
 * A plan already built over the provider keeps it alive until the plan is
 * freed. A no-op (returns nonzero) if `name` was never registered. */
DFTU_EXPORT int dftu_provider_unregister(const char* name);

/** Build a @ref dftracer::utils::dataframe::LazyFrame "LazyFrame" scanning the
 * provider registered as `name`. Returns a new owned dftu_lazyframe (free with
 * dftu_lazyframe_free), or NULL if no provider is registered under that name.
 * The returned plan keeps the provider alive, so a later
 * dftu_provider_unregister does not affect it. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_from_provider(const char* name);

/* ---- Op registry -------------------------------------------------------- */
/* One name-keyed registry over the engine's ops so a built-in op and a user op
 * are looked up and run the same way (the plugin-ABI foundation). Built-in ops
 * carry a plain name (`add`); a user op should namespace with a module prefix
 * (`mymod.zscore`) so it never shadows a built-in. */

/** Coarse category, for filtering/listing (== dftu_op_kind_of(sig)). */
typedef int32_t dftu_op_kind;
enum {
    DFTU_OP_KIND_SERIES = 0,    /**< column(s) -> column */
    DFTU_OP_KIND_AGGREGATE = 1, /**< column -> scalar (a reducer) */
    DFTU_OP_KIND_FRAME = 2,     /**< table(s) -> table */
    DFTU_OP_KIND_LAZY = 3       /**< lazy plan -> lazy plan (a plan node) */
};

/** Operand/return tokens, one 5-bit field each, so 32 values exist in total. A
 * signature packs a return token plus up to DFTU_OP_MAX_ARGS operand tokens
 * into one integer (DFTU_OP_SIG6), the way a Linux ioctl number packs
 * direction/type/nr/size. APPEND ONLY - a token's value is ABI-stable, and a
 * value past 31 would silently corrupt every signature it appears in. */
typedef int32_t dftu_op_tok;
enum {
    DFTU_TOK_NONE = 0,     /**< empty slot */
    DFTU_TOK_SERIES = 1,   /**< a column operand, or a column return */
    DFTU_TOK_SCALAR = 2,   /**< dftu_scalar operand, or a dftu_scalar return */
    DFTU_TOK_I64 = 3,      /**< int64 operand, or an int64 return */
    DFTU_TOK_BOOL = 4,     /**< int32 0/1 return */
    DFTU_TOK_CMP = 5,      /**< dftu_cmp_op operand */
    DFTU_TOK_PRIM = 6,     /**< dftu_prim_op operand */
    DFTU_TOK_LOGICAL = 7,  /**< dftu_logical_op operand */
    DFTU_TOK_DTYPE = 8,    /**< dftu_dtype operand */
    DFTU_TOK_REDUCE = 9,   /**< dftu_reduce_op operand */
    DFTU_TOK_STR = 10,     /**< (const char*, int32 length) operand */
    DFTU_TOK_CHAR = 11,    /**< char operand */
    DFTU_TOK_F64 = 12,     /**< double operand, or a double return */
    DFTU_TOK_I32 = 13,     /**< int32 flag operand (e.g. a `descending` flag) */
    DFTU_TOK_RANK = 14,    /**< dftu_rank_method operand */
    DFTU_TOK_ROLLING = 15, /**< dftu_rolling_op operand */
    DFTU_TOK_FRAME =
        16, /**< a dftu_dataframe operand, or a dftu_dataframe return */
    DFTU_TOK_STRLIST = 17, /**< a (const char* const*, int32 count) string-list
                         operand */
    DFTU_TOK_I32LIST =
        18, /**< a (const int32_t*, int32 count) int32-list operand */
    DFTU_TOK_LAZY =
        19, /**< a dftu_lazyframe operand, or a dftu_lazyframe return
             */
    DFTU_TOK_EXPR = 20,    /**< a const dftu_expr* operand (a predicate or a
                         projection), borrowed for the call */
    DFTU_TOK_AGGLIST = 21, /**< a (const dftu_group_agg*, int32 count)
                         aggregate-spec list operand */
    DFTU_TOK_U64 = 22, /**< uint64 operand (a byte count, a seed), distinct from
                     I64 so a signature describes the real parameter type
                     and cannot collide with a signed-shaped op */
    DFTU_TOK_I64LIST = 23, /**< a (const int64_t*, int32 count) int64-list
                         operand (e.g. LazyFrame::take's row indices) */
    DFTU_TOK_DUQL = 24, /**< a const dftu_duql* operand (a compiled predicate),
                       borrowed for the call */
    DFTU_TOK_WINLIST = 25 /**< a (const dftu_window_spec*, int32 count)
                        window-spec list operand */
};

/** One past the last token; a macro, so it does not become a case every switch
 * over dftu_op_tok has to answer for. Update alongside the last token. */
#define DFTU_TOK_COUNT (DFTU_TOK_WINLIST + 1)

/** Max operand tokens a signature carries (5-bit fields: a return token plus up
 * to this many operands pack into one int64). */
#define DFTU_OP_MAX_ARGS 7

/** Pack a signature from a return token and up to three operand tokens. Pass
 * each token's suffix (e.g. SERIES for DFTU_TOK_SERIES; pad unused operand
 * slots with NONE) - the suffix is pasted onto DFTU_TOK_. Compose new
 * signatures from tokens rather than allocating opaque ordinals; decode with
 * DFTU_OP_SIG_RET / DFTU_OP_SIG_ARG. Use DFTU_OP_SIG6 (5 operands) or
 * DFTU_OP_SIG8 (7 operands) for a wider op. */
#define DFTU_OP_SIG(ret, o0, o1, o2) DFTU_OP_SIG6(ret, o0, o1, o2, NONE, NONE)
/** Pack a signature with up to five operand tokens (5-bit fields). */
#define DFTU_OP_SIG6(ret, o0, o1, o2, o3, o4) \
    DFTU_OP_SIG8(ret, o0, o1, o2, o3, o4, NONE, NONE)
/** Pack a signature with up to seven operand tokens (5-bit fields): a return
 * token plus 7 operands is 8 * 5 = 40 bits, past int32 range, so every packed
 * field is cast to int64. */
#define DFTU_OP_SIG8(ret, o0, o1, o2, o3, o4, o5, o6)                  \
    ((int64_t)DFTU_TOK_##ret | ((int64_t)DFTU_TOK_##o0 << 5) |         \
     ((int64_t)DFTU_TOK_##o1 << 10) | ((int64_t)DFTU_TOK_##o2 << 15) | \
     ((int64_t)DFTU_TOK_##o3 << 20) | ((int64_t)DFTU_TOK_##o4 << 25) | \
     ((int64_t)DFTU_TOK_##o5 << 30) | ((int64_t)DFTU_TOK_##o6 << 35))
/** The return token of a signature. */
#define DFTU_OP_SIG_RET(sig) ((dftu_op_tok)((int64_t)(sig) & 0x1F))
/** Operand token `i` in [0, DFTU_OP_MAX_ARGS); DFTU_TOK_NONE past the last. */
#define DFTU_OP_SIG_ARG(sig, i) \
    ((dftu_op_tok)(((int64_t)(sig) >> (5 + 5 * (i))) & 0x1F))

/** A packed op signature: build it with DFTU_OP_SIG(ret, o0, o1, o2) at the
 * registration site, and use the same expression as a runner switch-case label
 * (a constant), the way a driver composes and switches on ioctl numbers. There
 * is deliberately no enum of named signatures: an op of an existing shape adds
 * only a registry line, and only a brand-new shape adds a runner case. */
typedef int64_t dftu_op_sig;

#ifdef __cplusplus
static_assert(sizeof(dftu_op_sig) * 8 >= 5 * (DFTU_OP_MAX_ARGS + 1),
              "dftu_op_sig is too narrow for DFTU_OP_MAX_ARGS 5-bit fields "
              "(a return token plus DFTU_OP_MAX_ARGS operands)");
static_assert(DFTU_TOK_COUNT <= 32, "dftu_op_tok exceeds its 5-bit field");
static_assert(sizeof(DFTU_OP_SIG6(SERIES, SERIES, NONE, NONE, NONE, NONE)) ==
                  sizeof(dftu_op_sig),
              "DFTU_OP_SIG6 computes in a narrower type than dftu_op_sig");
static_assert(sizeof(DFTU_OP_SIG8(SERIES, SERIES, NONE, NONE, NONE, NONE, NONE,
                                  NONE)) == sizeof(dftu_op_sig),
              "DFTU_OP_SIG8 computes in a narrower type than dftu_op_sig");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(dftu_op_sig) * 8 >= 5 * (DFTU_OP_MAX_ARGS + 1),
               "dftu_op_sig is too narrow for DFTU_OP_MAX_ARGS 5-bit fields "
               "(a return token plus DFTU_OP_MAX_ARGS operands)");
_Static_assert(DFTU_TOK_COUNT <= 32, "dftu_op_tok exceeds its 5-bit field");
_Static_assert(sizeof(DFTU_OP_SIG6(SERIES, SERIES, NONE, NONE, NONE, NONE)) ==
                   sizeof(dftu_op_sig),
               "DFTU_OP_SIG6 computes in a narrower type than dftu_op_sig");
_Static_assert(sizeof(DFTU_OP_SIG8(SERIES, SERIES, NONE, NONE, NONE, NONE, NONE,
                                   NONE)) == sizeof(dftu_op_sig),
               "DFTU_OP_SIG8 computes in a narrower type than dftu_op_sig");
#endif

/** A function pointer of no particular signature. `dftu_op_desc::fn` holds an
 * op's function as this type; the runner casts it back to the function type
 * the signature names. Cast the op function to this type when filling a
 * `dftu_op_desc`: `(dftu_op_generic_fn)&my_op`. */
typedef void (*dftu_op_generic_fn)(void);

/** A registry record. `sig` is authoritative: it packs the whole signature, so
 * it selects the fn cast, the operands read, and (via dftu_op_kind_of) the
 * category. For a built-in, `name` is a literal and must outlive the process;
 * dftu_op_register copies both the record and the name it points to, so a
 * caller's own `name` storage need not outlive the call. `fn` is always
 * borrowed: it is a pointer into the registrant's code, so it stops being
 * callable once that code unloads (see dftu_op_unregister). */
#ifndef DFTU_TYPEDEF_DFTU_OP_DESC
#define DFTU_TYPEDEF_DFTU_OP_DESC
typedef struct dftu_op_desc dftu_op_desc;
#endif
struct dftu_op_desc {
    const char* name;      /**< registry key, e.g. "add" or "mymod.zscore" */
    dftu_op_sig sig;       /**< the packed signature the runner dispatches on */
    dftu_op_generic_fn fn; /**< the engine function, cast to its real type */
};

/** The coarse category of a signature (decoded from its return token). */
DFTU_EXPORT dftu_op_kind dftu_op_kind_of(dftu_op_sig sig);

/** Number of leading `series` column operands a signature takes (its column
 * arity). */
DFTU_EXPORT uint32_t dftu_op_arity(dftu_op_sig sig);

/** A human-readable spelling of `sig`, e.g. "(series, scalar) -> series",
 * decoded from its tokens, for listing/discovery. Static storage; never NULL.
 */
DFTU_EXPORT const char* dftu_op_signature(dftu_op_sig sig);

/** The registered op named `name` (built-in or user), or NULL if none. The
 * pointer is valid while the name stays registered; after
 * dftu_op_unregister it may dangle. A plan or expression keeps an op past
 * that through the C++ handle @ref dftracer::utils::dataframe::OpDesc. */
DFTU_EXPORT const dftu_op_desc* dftu_op_find(const char* name);

/** Number of registered ops (built-ins + user), for discovery/listing. */
DFTU_EXPORT uint32_t dftu_op_count(void);

/** The op at index `i` in [0, dftu_op_count()), or NULL if out of range. The
 * ordering is unspecified and may change as user ops are registered. */
DFTU_EXPORT const dftu_op_desc* dftu_op_at(uint32_t i);

/** Register a user op. The record and its name are copied; `fn` is still
 * borrowed. Returns 0 on success, non-zero if `desc`/its name is NULL or the
 * name is already registered (no silent shadowing). A registrant whose code
 * can unload (a plugin's .so) must call dftu_op_unregister before unloading,
 * or `fn` dangles in a registry entry no one can remove it from. A plan built
 * over the op keeps its record, not the code `fn` points into: a registrant in
 * plain C must keep that code loaded while a plan can still run the op, and the
 * plugin host does so for a plugin's ops. */
DFTU_EXPORT int dftu_op_register(const dftu_op_desc* desc);

/** Remove a user op previously added with dftu_op_register. Only the name is
 * removed: a plan that already holds the op keeps running it, and a later
 * lookup by name finds nothing. A no-op (returns
 * non-zero) if `name` is NULL, unregistered, or a built-in - built-ins can
 * never be removed this way. Safe to call for a name that was never
 * registered. */
DFTU_EXPORT int dftu_op_unregister(const char* name);

/** One operand slot. Only the union member the operand's token names is read:
 * SCALAR->scalar, I64->i64, F64->f64, CHAR->ch, an enum token (CMP/PRIM/
 * LOGICAL/DTYPE/REDUCE/I32/RANK/ROLLING)->i32, STR->str, STRLIST->list,
 * I32LIST->i32list, I64LIST->i64list, DUQL->duql, and a frame op's SERIES
 * operand (a mask/column)->series. A SERIES/FRAME operand that is the primary
 * operand of a column/frame/lazy op is passed in the runner's in[]/frames[]
 * array, not here; a FRAME operand of a non-frame op (e.g. a SERIES-return op
 * over a whole table) still rides here as ->frame. */
typedef union dftu_op_val {
    dftu_scalar scalar;
    int64_t i64;
    uint64_t u64;
    int32_t i32;
    double f64;
    char ch;
    const dftu_series* series;
    struct {
        const char* ptr;
        int32_t len;
    } str;
    struct {
        const char* const* items;
        int32_t n;
    } list;
    struct {
        const int32_t* items;
        int32_t n;
    } i32list;
    struct {
        const int64_t* items;
        int32_t n;
    } i64list;
    const dftu_lazyframe* lazy;
    const dftu_expr* expr;
    struct {
        const dftu_group_agg* items;
        int32_t n;
    } agglist;
    const dftu_dataframe* frame;
    const dftu_duql* duql;
    struct {
        const dftu_window_spec* items;
        int32_t n;
    } winlist;
} dftu_op_val;

/** The operands an op consumes, one slot per operand token in order (args[i]
 * matches the i-th operand token of the signature). Only the slots a given op
 * needs are read; pass NULL when it needs none. */
#ifndef DFTU_TYPEDEF_DFTU_OP_ARG
#define DFTU_TYPEDEF_DFTU_OP_ARG
typedef struct dftu_op_arg dftu_op_arg;
#endif
struct dftu_op_arg {
    dftu_op_val args[DFTU_OP_MAX_ARGS];
};

/** Run a column op (a signature whose return token is SERIES): `in` are `n`
 * borrowed input columns (n must equal dftu_op_arity(op->sig)), `arg` supplies
 * the remaining operands the signature names (NULL when there are none).
 * Returns a new owned column (free with dftu_series_free), or NULL on a
 * NULL/kind/arity/shape mismatch. */
DFTU_EXPORT dftu_series* dftu_op_run(const dftu_op_desc* op,
                                     const dftu_series* const* in, uint32_t n,
                                     const dftu_op_arg* arg);

/** Run a reducer (a signature whose return token is SCALAR/I64/BOOL) on one
 * column, returning its reduction as a dftu_scalar (an i64/bool return is
 * widened into the I64 domain). `arg` is read only by a signature with an
 * operand token (e.g. REDUCE). On a NULL/kind/shape mismatch sets *ok to 0
 * (when ok != NULL) and returns a zero scalar; otherwise sets *ok to 1. */
DFTU_EXPORT dftu_scalar dftu_op_run_aggregate(const dftu_op_desc* op,
                                              const dftu_series* v,
                                              const dftu_op_arg* arg, int* ok);

/** Run a frame op (a signature whose return token is FRAME): `frames` are the
 * `n` borrowed input dftu_dataframe operands (n == dftu_op_arity(op->sig)), and
 * every other operand (series/scalar/string/list/int) rides `arg`. Returns a
 * new owned dftu_dataframe (free with dftu_dataframe_free), or NULL on a
 * NULL/kind/arity/shape mismatch. */
DFTU_EXPORT dftu_dataframe* dftu_op_run_frame(
    const dftu_op_desc* op, const dftu_dataframe* const* frames, uint32_t n,
    const dftu_op_arg* arg);

/** Run a lazy op (a signature whose return token is LAZY): `in` are the `n`
 * borrowed input dftu_lazyframe operands (n == dftu_op_arity(op->sig)), and
 * every other operand (string/scalar/list/expr/agglist/int) rides `arg`.
 * Returns a new owned dftu_lazyframe (free with dftu_lazyframe_free), or NULL
 * on a NULL/kind/arity/shape mismatch. */
DFTU_EXPORT dftu_lazyframe* dftu_op_run_lazy(const dftu_op_desc* op,
                                             const dftu_lazyframe* const* in,
                                             uint32_t n,
                                             const dftu_op_arg* arg);

/* ---- Node registry ------------------------------------------------------- */
/* dftu_op_run_lazy (above) runs a LAZY-kind op eagerly at the call site and
 * returns a new plan. A node is different: it is a step the ENGINE executes
 * later, inside the pull chain a collect()/stream() drives, the same protocol
 * a built-in op (filter, select, group_by, ...) already runs on - a Cursor
 * pulled with next(). One name-keyed registry, mirroring the provider
 * registry above: process-lifetime, named, and a name already taken is
 * refused rather than replaced. */

/** Remove the node registered as `name`. A caller that can unload (a plugin's
 * shared object) must call this before unloading, the same requirement
 * dftu_provider_unregister documents for a provider - otherwise a later
 * dftu_lazyframe_op call opens a cursor into unmapped memory. A plan already
 * built over the node keeps it alive until the plan is freed. A no-op (returns
 * nonzero) if `name` was never registered. */
DFTU_EXPORT int dftu_node_unregister(const char* name);

/** Append a step running the node registered as `name` over `lf`, with `args`
 * (NULL when the node needs none) copied into the plan by value - a
 * pointer-bearing operand (str/strlist/i32list/i64list/agglist/expr/query)
 * must therefore point at storage that outlives every future execution of the
 * returned @ref dftracer::utils::dataframe::LazyFrame "LazyFrame", not merely
 * this call. Returns a new owned dftu_lazyframe (free with
 * dftu_lazyframe_free), or NULL if `lf`/`name` is NULL or no node is registered
 * under that name - an unknown name fails here, loudly, rather than building a
 * plan that fails later. A node is an optimization barrier: the engine never
 * pushes a filter or a projection through it and never reorders around it,
 * since it has no way to know whether that is safe for an opaque node. An
 * open() that fails, or a node whose produced frame does
 * not match the column count output_schema() declared, surfaces as an error the
 * first time the plan is driven (collect() or stream()), naming the node. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_op(const dftu_lazyframe* lf,
                                              const char* name,
                                              const dftu_op_arg* args);

/** Any registered table -> table op (a dftu.frame.* row, or one another
 * library registered such as dftu.frame.window) as a plan step: `lf` is
 * collected and passed as the op's first FRAME operand, each further FRAME
 * operand takes the next of the `n` `others` (collected when the op runs),
 * every other operand is copied out of `args` now so the plan owns it. A
 * pipeline breaker and an optimizer barrier. `out_names` (n_out of them) are
 * the output column names when the caller can state them from the op's
 * contract; NULL / 0 leaves them data-dependent, known only at collect.
 * `others` are borrowed (the plan holds copies). NULL if a handle or `name`
 * is NULL, no such op is registered, it is not a table -> table op, an
 * operand has no owned form (EXPR / DUQL / LAZY), or `n` does not match the
 * op's frame operands. */
DFTU_EXPORT dftu_lazyframe* dftu_lazyframe_frame_op(
    const dftu_lazyframe* lf, const char* name,
    const dftu_lazyframe* const* others, int32_t n, const dftu_op_arg* args,
    const char* const* out_names, int32_t n_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  // DFTRACER_UTILS_DATAFRAME_C_API_H
